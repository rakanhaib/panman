#include <iostream>
#include <string>
#include <vector>
#include <stack>
#include <tbb/parallel_reduce.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/concurrent_vector.h>
#include <tbb/concurrent_unordered_map.h>
#include <ctime>
#include <iomanip>
#include <mutex>

#include "PangenomeMATNew.hpp"

// std::string getDate(){
//     std::time_t t = std::time(0);   // get time now
//     std::tm* now = std::localtime(&t);
//     std::string date;
//     date += std::to_string(now->tm_year + 1900)
//          + std::to_string(now->tm_mon + 1)
//          +  std::to_string(now->tm_mday);
//     return date;
// }

PangenomeMATNew::Node::Node(std::string id, float len){
    identifier = id;
    level = 1;
    branchLength = len;
    parent = nullptr;
}

PangenomeMATNew::Node::Node(std::string id, Node* par, float len){
    identifier = id;
    branchLength = len;
    parent = par;
    level = par->level + 1;
    par->children.push_back(this);
}

PangenomeMATNew::Block::Block(MATNew::block b){
    primaryBlockId = (b.blockid() >> 32);
    if(b.blockgapexist()){
        secondaryBlockId = (b.blockid() & 0xFFFF);
    } else {
        secondaryBlockId = -1;
    }
    
    chromosomeName = b.chromosomename();
    for(int i = 0; i < b.consensusseq_size(); i++){
        consensusSeq.push_back(b.consensusseq(i));
    }
}

void PangenomeMATNew::stringSplit (std::string const& s, char delim, std::vector<std::string>& words) {
    size_t start_pos = 0, end_pos = 0;
    while ((end_pos = s.find(delim, start_pos)) != std::string::npos) {
        if (end_pos >= s.length()) {
            break;
        }
        words.emplace_back(s.substr(start_pos, end_pos-start_pos));
        start_pos = end_pos+1;
    }
    auto last = s.substr(start_pos, s.size()-start_pos);
    if (last != "") {
        words.push_back(std::move(last));
    }
}

PangenomeMATNew::Node* PangenomeMATNew::Tree::createTreeFromNewickString(std::string newickString) {

    PangenomeMATNew::Node* newTreeRoot;

    std::vector<std::string> leaves;
    std::vector<size_t> numOpen;
    std::vector<size_t> numClose;
    std::vector<std::queue<float>> branchLen (128);  // will be resized later if needed
    size_t level = 0;

    std::vector<std::string> s1;
    stringSplit(newickString, ',', s1);

    numOpen.reserve(s1.size());
    numClose.reserve(s1.size());

    for (auto s: s1) {
        size_t no = 0;
        size_t nc = 0;
        size_t leafDepth = 0;

        bool stop = false;
        bool branchStart = false;
        std::string leaf = "";
        std::string branch = "";

        for (auto c: s) {
            if (c == ':') {
                stop = true;
                branch = "";
                branchStart = true;
            } else if (c == '(') {
                no++;
                level++;
                if (branchLen.size() <= level) {
                    branchLen.resize(level*2);
                }
            } else if (c == ')') {
                stop = true;
                nc++;
                float len = (branch.size() > 0) ? std::stof(branch) : -1.0;
                branchLen[level].push(len);
                level--;
                branchStart = false;
            } else if (!stop) {
                leaf += c;
                branchStart = false;
                leafDepth = level;

            } else if (branchStart) {
                if (isdigit(c)  || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
                    branch += c;
                }
            }
        }
        leaves.push_back(std::move(leaf));
        numOpen.push_back(no);
        numClose.push_back(nc);
        float len = (branch.size() > 0) ? std::stof(branch) : -1.0;
        branchLen[level].push(len);

        // Adjusting max and mean depths
        m_maxDepth = std::max(m_maxDepth, leafDepth);
        m_meanDepth += leafDepth;

    }

    m_meanDepth /= leaves.size();

    if (level != 0) {
        fprintf(stderr, "ERROR: incorrect Newick format!\n");
        exit(1);
    }

    m_numLeaves = leaves.size();

    std::stack<Node*> parentStack;

    for (size_t i=0; i<leaves.size(); i++) {
        auto leaf = leaves[i];
        auto no = numOpen[i];
        auto nc = numClose[i];
        for (size_t j=0; j<no; j++) {
            std::string nid = newInternalNodeId();
            Node* newNode = nullptr;
            if (parentStack.size() == 0) {
                newNode = new Node(nid, branchLen[level].front());
                newTreeRoot = newNode;
            } else {
                newNode = new Node(nid, parentStack.top(), branchLen[level].front());
            }
            branchLen[level].pop();
            level++;

            allNodes[nid] = newNode;
            parentStack.push(newNode);
        }
        Node* leafNode = new Node(leaf, parentStack.top(), branchLen[level].front());
        allLeaves.push_back(leafNode);

        allNodes[leaf] = leafNode;

        branchLen[level].pop();
        for (size_t j=0; j<nc; j++) {
            parentStack.pop();
            level--;
        }
    }

    if (newTreeRoot == nullptr) {
        fprintf(stderr, "WARNING: Tree found empty!\n");
    }

    return newTreeRoot;
}

void PangenomeMATNew::Tree::assignMutationsToNodes(Node* root, size_t& currentIndex, std::vector< MATNew::node >& nodes){
    std::vector< PangenomeMATNew::NucMut > storedNucMutation;

    for(int i = 0; i < nodes[currentIndex].nucmutation_size(); i++){
        storedNucMutation.push_back( PangenomeMATNew::NucMut(nodes[currentIndex].nucmutation(i)) );
    }

    std::vector< PangenomeMATNew::BlockMut > storedBlockMutation;
    for(int i = 0; i < nodes[currentIndex].blockmutation_size(); i++){
        PangenomeMATNew::BlockMut tempBlockMut;
        tempBlockMut.loadFromProtobuf(nodes[currentIndex].blockmutation(i));
        storedBlockMutation.push_back(tempBlockMut);
    }

    root->nucMutation = storedNucMutation;
    root->blockMutation = storedBlockMutation;

    for(auto child: root->children){
        currentIndex++;
        assignMutationsToNodes(child, currentIndex, nodes);
    }

}

void PangenomeMATNew::Tree::invertTree(PangenomeMATNew::Node* root){
    for(auto child: root->children){
        invertTree(child);
    }
    std::reverse(root->children.begin(), root->children.end());
}

PangenomeMATNew::Tree::Tree(std::ifstream& fin){

    MATNew::tree mainTree;

    if(!mainTree.ParseFromIstream(&fin)){
        throw std::invalid_argument("Could not read tree from input file.");
    }

    // Create tree
    root = createTreeFromNewickString(mainTree.newick());
    invertTree(root);

    std::vector< MATNew::node > storedNodes;
    for(int i = 0; i < mainTree.nodes_size(); i++){
        storedNodes.push_back(mainTree.nodes(i));
    }

    size_t initialIndex = 0;

    assignMutationsToNodes(root, initialIndex, storedNodes);

    // Block sequence
    for(int i = 0; i < mainTree.blocks_size(); i++){
        blocks.emplace_back(mainTree.blocks(i));
    }

    // Gap List
    for(int i = 0; i < mainTree.gaps_size(); i++){
        PangenomeMATNew::GapList tempGaps;
        tempGaps.primaryBlockId = (mainTree.gaps(i).blockid() >> 32);
        tempGaps.secondaryBlockId = (mainTree.gaps(i).blockgapexist() ? (mainTree.gaps(i).blockid() & 0xFFFF): -1);
        for(int j = 0; j < mainTree.gaps(i).nucposition_size(); j++){
            tempGaps.nucPosition.push_back(mainTree.gaps(i).nucposition(j));
            tempGaps.nucGapLength.push_back(mainTree.gaps(i).nucgaplength(j));
        }
        gaps.push_back(tempGaps);
    }

    // Block gap list
    for(int i = 0; i < mainTree.blockgaps().blockposition_size(); i++){
        blockGaps.blockPosition.push_back(mainTree.blockgaps().blockposition(i));
        blockGaps.blockGapLength.push_back(mainTree.blockgaps().blockgaplength(i));
    }

}

int getTotalParsimonyParallelHelper(PangenomeMATNew::Node* root, PangenomeMATNew::NucMutationType nucMutType, PangenomeMATNew::BlockMutationType blockMutType){
    int totalMutations = 0;

    totalMutations += tbb::parallel_reduce(tbb::blocked_range<int>(0, root->nucMutation.size()), 0, [&](tbb::blocked_range<int> r, int init) -> int{
        for(int i = r.begin(); i != r.end(); i++){
            
            if(((root->nucMutation[i].mutInfo) & 0x7) == nucMutType){
                if(nucMutType == PangenomeMATNew::NucMutationType::NS){
                    init += ((root->nucMutation[i].mutInfo) >> 4); // Length of contiguous mutation in case of substitution
                } else {
                    init++;
                }
            }
        }
        return init;
    }, [&](int x, int y){
        return x + y;
    });

    if(blockMutType != PangenomeMATNew::BlockMutationType::NONE){
        totalMutations += tbb::parallel_reduce(tbb::blocked_range<int>(0, root->blockMutation.size()), 0, [&](tbb::blocked_range<int> r, int init) -> int{
            for(int i = r.begin(); i != r.end(); i++){
                if(root->blockMutation[i].blockMutInfo == blockMutType){
                    init++;
                }
            }
            return init;
        }, [&](int x, int y){
            return x + y;
        });
    }


    totalMutations += tbb::parallel_reduce(tbb::blocked_range<int>(0, root->children.size()), 0, [&](tbb::blocked_range<int>& r, int init) -> int{
        for(int i = r.begin(); i != r.end(); i++){
            init += getTotalParsimonyParallelHelper(root->children[i], nucMutType, blockMutType);
        }
        return init;
    },
    [](int x, int y) -> int {
        return x+y;
    });

    return totalMutations;
}

int PangenomeMATNew::Tree::getTotalParsimonyParallel(NucMutationType nucMutType, BlockMutationType blockMutType){

    return getTotalParsimonyParallelHelper(root, nucMutType, blockMutType);

}

void PangenomeMATNew::Tree::printSummary(){

    std::cout << "Total Nodes in Tree: " << m_currInternalNode + m_numLeaves << std::endl;
    std::cout << "Total Samples in Tree: " << m_numLeaves << std::endl;
    std::cout << "Total Substitutions: " << getTotalParsimonyParallel(PangenomeMATNew::NucMutationType::NS) << std::endl;
    std::cout << "Total Insertions: " << getTotalParsimonyParallel(PangenomeMATNew::NucMutationType::NI, PangenomeMATNew::BlockMutationType::BI) << std::endl;
    std::cout << "Total Deletions: " << getTotalParsimonyParallel(PangenomeMATNew::NucMutationType::ND, PangenomeMATNew::BlockMutationType::BD) << std::endl;
    std::cout << "Total SNP Substitutions: " << getTotalParsimonyParallel(PangenomeMATNew::NucMutationType::NSNPS) << std::endl;
    std::cout << "Total SNP Insertions: " << getTotalParsimonyParallel(PangenomeMATNew::NucMutationType::NSNPI) << std::endl;
    std::cout << "Total SNP Deletions: " << getTotalParsimonyParallel(PangenomeMATNew::NucMutationType::NSNPD) << std::endl;
    std::cout << "Max Tree Depth: " << m_maxDepth << std::endl;
    std::cout << "Mean Tree Depth: " << m_meanDepth << std::endl;

}

void PangenomeMATNew::Tree::printBfs(Node* node){
    if(node == nullptr){
        node = root;
    }

    // Traversal test
    std::queue<Node *> bfsQueue;
    size_t prevLev = 0;
    
    bfsQueue.push(node);

    while(!bfsQueue.empty()){
        Node* current = bfsQueue.front();
        bfsQueue.pop();

        if(current->level != prevLev){
            std::cout << '\n';
            prevLev = current->level;
        }
        std::cout << '(' << current->identifier << "," << current->branchLength << ") ";

        for(auto child: current->children){
            bfsQueue.push(child);
        }
    }
    std::cout << '\n';
}

void PangenomeMATNew::printSequenceLines(const std::vector< std::pair< std::vector< std::pair< char, std::vector< char > > >, std::vector< std::vector< std::pair< char, std::vector< char > > > > > >& sequence,\
    const std::vector< std::pair< bool, std::vector< bool > > >& blockExists, size_t lineSize, bool aligned, std::ofstream& fout){

    std::string line;

    for(size_t i = 0; i < blockExists.size(); i++){
        for(size_t j = 0; j < blockExists[i].second.size(); j++){
            if(blockExists[i].second[j]){

                for(size_t k = 0; k < sequence[i].second[j].size(); k++){
                    for(size_t w = 0; w < sequence[i].second[j][k].second.size(); w++){
                        if(sequence[i].second[j][k].second[w] != '-'){
                            line += sequence[i].second[j][k].second[w];
                        } else if(aligned){
                            line += '-';
                        }
                        if(line.length() == lineSize){
                            fout << line << '\n';
                            line = "";
                        }
                    }
                    if(sequence[i].second[j][k].first != 'x'){
                        if(sequence[i].second[j][k].first != '-'){
                            line += sequence[i].second[j][k].first;
                        } else if(aligned){
                            line += '-';
                        }
                        if(line.length() == lineSize){
                            fout << line << '\n';
                            line = "";
                        }
                    }
                }
            } else if(aligned) {
                for(size_t k = 0; k < sequence[i].second[j].size(); k++){
                    for(size_t w = 0; w < sequence[i].second[j][k].second.size(); w++){
                        line += '-';
                        if(line.length() == lineSize){
                            fout << line << '\n';
                            line = "";
                        }
                    }
                    if(sequence[i].second[j][k].first != 'x'){
                        line += '-';
                        if(line.length() == lineSize){
                            fout << line << '\n';
                            line = "";
                        }
                    }
                }
            }
        }

        if(blockExists[i].first){
            for(size_t j = 0; j < sequence[i].first.size(); j++){
                for(size_t k = 0; k < sequence[i].first[j].second.size(); k++){
                    if(sequence[i].first[j].second[k] != '-'){
                        line += sequence[i].first[j].second[k];
                    } else if(aligned){
                        line += '-';
                    }
                    if(line.length() == lineSize){
                        fout << line << '\n';
                        line = "";
                    }
                }
                if(sequence[i].first[j].first != 'x'){
                    if(sequence[i].first[j].first != '-'){
                        line += sequence[i].first[j].first;
                    } else if(aligned){
                        line += '-';
                    }
                    if(line.length() == lineSize){
                        fout << line << '\n';
                        line = "";
                    }
                }
            }
        }

    }

    if(line.length()){
        fout << line << '\n';
        line = "";
    }

}

char PangenomeMATNew::getNucleotideFromCode(int code){
    switch(code){
        case 1:
            return 'A';
        case 2:
            return 'C';
        case 4:
            return 'G';
        case 8:
            return 'T';
        case 5:
            return 'R';
        case 10:
            return 'Y';
        case 6:
            return 'S';
        case 9:
            return 'W';
        case 12:
            return 'K';
        case 3:
            return 'M';
        case 14:
            return 'B';
        case 13:
            return 'D';
        case 11:
            return 'H';
        case 7:
            return 'V';
        default:
            return 'N';
    }
}

int reverseNucs(int nucs){
    int res = 0;
    for(int i = 0; i < 6; i++){
        res  = (res ^ (((nucs >> (4*i)) & 0xF) << 4*(5-i)));
    }
    return res;
}

void printFASTAHelper(PangenomeMATNew::Node* root,\
    std::vector< std::pair< std::vector< std::pair< char, std::vector< char > > >, std::vector< std::vector< std::pair< char, std::vector< char > > > > > >& sequence,\
    std::vector< std::pair< bool, std::vector< bool > > >& blockExists,\
    std::ofstream& fout, bool aligned = false){

    // Apply mutations
    // Block mutations - ignored for now since the block IDs don't seem right in the files

    std::vector< std::tuple< int32_t, int32_t, bool, bool > > blockMutationInfo;

    for(auto mutation: root->blockMutation){
        int32_t primaryBlockId = mutation.primaryBlockId;
        int32_t secondaryBlockId = mutation.secondaryBlockId;
        bool type = (mutation.blockMutInfo);

        if(type == 1){
            bool oldVal;
            if(secondaryBlockId != -1){
                oldVal = blockExists[primaryBlockId].second[secondaryBlockId];
                blockExists[primaryBlockId].second[secondaryBlockId] = true;
            } else {
                oldVal = blockExists[primaryBlockId].first;
                blockExists[primaryBlockId].first = true;
            }
            blockMutationInfo.push_back( std::make_tuple(mutation.primaryBlockId, mutation.secondaryBlockId, oldVal, true) );
        } else {
            bool oldVal;
            if(secondaryBlockId != -1){
                oldVal = blockExists[primaryBlockId].second[secondaryBlockId];
                blockExists[primaryBlockId].second[secondaryBlockId] = false;
            } else {
                oldVal = blockExists[primaryBlockId].first;
                blockExists[primaryBlockId].first = false;
            }
            blockMutationInfo.push_back( std::make_tuple(mutation.primaryBlockId, mutation.secondaryBlockId, oldVal, false) );
        }

    }

    // For backtracking. primaryBlockId, secondaryBlockId, pos, gapPos, (oldVal, newVal) in substitution, ('-', newVal) in insertion, (oldVal, '-') in deletion
    std::vector< std::tuple< int32_t, int32_t, int, int, char, char > > mutationInfo;

    // Nuc mutations
    for(size_t i = 0; i < root->nucMutation.size(); i++){
        int32_t primaryBlockId = root->nucMutation[i].primaryBlockId;
        int32_t secondaryBlockId = root->nucMutation[i].secondaryBlockId;

        int32_t nucPosition = root->nucMutation[i].nucPosition;
        int32_t nucGapPosition = root->nucMutation[i].nucGapPosition;
        uint32_t type = (root->nucMutation[i].mutInfo & 0x7);
        char newVal = '-';

        if(type < 3){
            // Either S, I or D

            int len = ((root->nucMutation[i].mutInfo) >> 4);

            if(type == PangenomeMATNew::NucMutationType::NS){
                if(secondaryBlockId != -1){
                    if(nucGapPosition != -1){
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition+j];
                            newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> (4*(5-j))) & 0xF);
                            sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition+j] = newVal;
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition+j, oldVal, newVal));
                        }
                    } else {
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition + j].first;
                            newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> (4*(5-j))) & 0xF);
                            sequence[primaryBlockId].second[secondaryBlockId][nucPosition + j].first = newVal;
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition + j, nucGapPosition, oldVal, newVal));
                        }

                    }
                } else {
                    if(nucGapPosition != -1){
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].first[nucPosition].second[nucGapPosition+j];
                            newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> (4*(5-j))) & 0xF);
                            sequence[primaryBlockId].first[nucPosition].second[nucGapPosition+j] = newVal;
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition+j, oldVal, newVal));   
                        }
                    } else {
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].first[nucPosition+j].first;
                            newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> (4*(5-j))) & 0xF);
                            sequence[primaryBlockId].first[nucPosition+j].first = newVal;
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition + j, nucGapPosition, oldVal, newVal));   
                        }
                    }
                }
            }
            else if(type == PangenomeMATNew::NucMutationType::NI){
                if(secondaryBlockId != -1){
                    if(nucGapPosition != -1){
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition + j];
                            newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> (4*(5-j))) & 0xF);
                            sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition+j] = newVal;
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition+j, oldVal, newVal));
                        }
                    } else {
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition + j].first;
                            newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> (4*(5-j))) & 0xF);
                            sequence[primaryBlockId].second[secondaryBlockId][nucPosition + j].first = newVal;
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition + j, nucGapPosition, oldVal, newVal));
                        }

                    }
                } else {
                    if(nucGapPosition != -1){
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].first[nucPosition].second[nucGapPosition+j];
                            newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> (4*(5-j))) & 0xF);
                            sequence[primaryBlockId].first[nucPosition].second[nucGapPosition+j] = newVal;
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition+j, oldVal, newVal));   
                        }
                    } else {
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].first[nucPosition+j].first;
                            newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> (4*(5-j))) & 0xF);
                            sequence[primaryBlockId].first[nucPosition+j].first = newVal;
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition + j, nucGapPosition, oldVal, newVal));   
                        }
                    }
                }
            }
            else if(type == PangenomeMATNew::NucMutationType::ND){
                if(secondaryBlockId != -1){
                    if(nucGapPosition != -1){
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition+j];
                            sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition+j] = '-';
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition+j, oldVal, '-'));
                        }
                    } else {
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition + j].first;
                            sequence[primaryBlockId].second[secondaryBlockId][nucPosition + j].first = '-';
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition + j, nucGapPosition, oldVal, '-'));
                        }

                    }
                } else {
                    if(nucGapPosition != -1){
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].first[nucPosition].second[nucGapPosition+j];
                            sequence[primaryBlockId].first[nucPosition].second[nucGapPosition+j] = '-';
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition+j, oldVal, '-'));
                        }
                    } else {
                        for(int j = 0; j < len; j++){
                            char oldVal = sequence[primaryBlockId].first[nucPosition+j].first;
                            sequence[primaryBlockId].first[nucPosition+j].first = '-';
                            mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition + j, nucGapPosition, oldVal, '-'));
                        }
                    }
                }
            }
        } 
        else {
            if(type == PangenomeMATNew::NucMutationType::NSNPS){
                newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> 20) & 0xF);
                if(secondaryBlockId != -1){
                    if(nucGapPosition != -1){
                        char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition];
                        sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition] = newVal;
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, newVal));
                    } else {
                        char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].first;
                        sequence[primaryBlockId].second[secondaryBlockId][nucPosition].first = newVal;
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, newVal));
                    }
                } else {
                    if(nucGapPosition != -1){
                        char oldVal = sequence[primaryBlockId].first[nucPosition].second[nucGapPosition];
                        sequence[primaryBlockId].first[nucPosition].second[nucGapPosition] = newVal;
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, newVal));
                    } else {
                        char oldVal = sequence[primaryBlockId].first[nucPosition].first;
                        sequence[primaryBlockId].first[nucPosition].first = newVal;
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, newVal));   
                    }
                }
            }
            else if(type == PangenomeMATNew::NucMutationType::NSNPI){
                newVal = PangenomeMATNew::getNucleotideFromCode(((root->nucMutation[i].nucs) >> 20) & 0xF);
                if(secondaryBlockId != -1){
                    if(nucGapPosition != -1){
                        char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition];
                        sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition] = newVal;
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, newVal));
                    } else {
                        char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].first;
                        sequence[primaryBlockId].second[secondaryBlockId][nucPosition].first = newVal;
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, newVal));
                    }
                } else {
                    if(nucGapPosition != -1){
                        char oldVal = sequence[primaryBlockId].first[nucPosition].second[nucGapPosition];
                        sequence[primaryBlockId].first[nucPosition].second[nucGapPosition] = newVal;
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, newVal));   
                    } else {
                        char oldVal = sequence[primaryBlockId].first[nucPosition].first;
                        sequence[primaryBlockId].first[nucPosition].first = newVal;
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, newVal));   
                    }
                }
            }
            else if(type == PangenomeMATNew::NucMutationType::NSNPD){
                if(secondaryBlockId != -1){
                    if(nucGapPosition != -1){
                        char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition];
                        sequence[primaryBlockId].second[secondaryBlockId][nucPosition].second[nucGapPosition] = '-';
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, '-'));
                    } else {
                        char oldVal = sequence[primaryBlockId].second[secondaryBlockId][nucPosition].first;
                        sequence[primaryBlockId].second[secondaryBlockId][nucPosition].first = '-';
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, '-'));
                    }
                } else {
                    if(nucGapPosition != -1){
                        char oldVal = sequence[primaryBlockId].first[nucPosition].second[nucGapPosition];
                        sequence[primaryBlockId].first[nucPosition].second[nucGapPosition] = '-';
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, '-'));
                    } else {
                        char oldVal = sequence[primaryBlockId].first[nucPosition].first;
                        sequence[primaryBlockId].first[nucPosition].first = '-';
                        mutationInfo.push_back(std::make_tuple(primaryBlockId, secondaryBlockId, nucPosition, nucGapPosition, oldVal, '-'));
                    }
                }
            }
        }
    }
    
    if(root->children.size() == 0){
        // Print sequence
        fout << '>' << root->identifier << std::endl;

        PangenomeMATNew::printSequenceLines(sequence, blockExists, 70, aligned, fout);

    } else {
        // DFS on children
        for(PangenomeMATNew::Node* child: root->children){
            printFASTAHelper(child, sequence, blockExists, fout, aligned);
        }
    }

    for(auto it = blockMutationInfo.rbegin(); it != blockMutationInfo.rend(); it++){
        auto mutation = *it;
        if(std::get<1>(mutation) != -1){
            blockExists[std::get<0>(mutation)].second[std::get<1>(mutation)] = std::get<2>(mutation);
        } else {
            blockExists[std::get<0>(mutation)].first = std::get<2>(mutation);
        }
    }

    // Undo nuc mutations
    for(auto it = mutationInfo.rbegin(); it != mutationInfo.rend(); it++){
        auto mutation = *it;
        if(std::get<1>(mutation) != -1){
            if(std::get<3>(mutation) != -1){
                sequence[std::get<0>(mutation)].second[std::get<1>(mutation)][std::get<2>(mutation)].second[std::get<3>(mutation)] = std::get<4>(mutation);
            } else {
                sequence[std::get<0>(mutation)].second[std::get<1>(mutation)][std::get<2>(mutation)].first = std::get<4>(mutation);
            }
        } else {
            if(std::get<3>(mutation) != -1){
                sequence[std::get<0>(mutation)].first[std::get<2>(mutation)].second[std::get<3>(mutation)] = std::get<4>(mutation);
            } else {
                sequence[std::get<0>(mutation)].first[std::get<2>(mutation)].first = std::get<4>(mutation);
            }
        }
    }
}

void PangenomeMATNew::Tree::printFASTA(std::ofstream& fout, bool aligned, int parallelism){
    // List of blocks. Each block has a nucleotide list. Along with each nucleotide is a gap list.
    std::vector< std::pair< std::vector< std::pair< char, std::vector< char > > >, std::vector< std::vector< std::pair< char, std::vector< char > > > > > > sequence(blocks.size() + 1);
    std::vector< std::pair< bool, std::vector< bool > > > blockExists(blocks.size() + 1, {false, {}});

    // Assigning block gaps
    for(size_t i = 0; i < blockGaps.blockPosition.size(); i++){
        sequence[blockGaps.blockPosition[i]].second.resize(blockGaps.blockGapLength[i] + 1);
        blockExists[blockGaps.blockPosition[i]].second.resize(blockGaps.blockGapLength[i] + 1, false);
    }

    int32_t maxBlockId = 0;

    for(size_t i = 0; i < blocks.size(); i++){
        
        int32_t primaryBlockId = ((int32_t)blocks[i].primaryBlockId);
        int32_t secondaryBlockId = ((int32_t)blocks[i].secondaryBlockId);

        maxBlockId = std::max(maxBlockId, primaryBlockId);

        for(size_t j = 0; j < blocks[i].consensusSeq.size(); j++){
            bool endFlag = false;
            for(size_t k = 0; k < 8; k++){
                const int nucCode = (((blocks[i].consensusSeq[j]) >> (4*(7 - k))) & 15);
                if(nucCode == 0){
                    endFlag = true;
                    break;
                }
                const char nucleotide = PangenomeMATNew::getNucleotideFromCode(nucCode);
                if(secondaryBlockId != -1){
                    sequence[primaryBlockId].second[secondaryBlockId].push_back({nucleotide, {}});
                } else {
                    sequence[primaryBlockId].first.push_back({nucleotide, {}});
                }
            }
            if(endFlag){
                break;
            }
        }

        // End character to incorporate for gaps at the end
        if(secondaryBlockId != -1){
            sequence[primaryBlockId].second[secondaryBlockId].push_back({'x', {}});
        } else {
            sequence[primaryBlockId].first.push_back({'x', {}});
        }
    }

    sequence.resize(maxBlockId + 1);
    blockExists.resize(maxBlockId + 1);

    // Assigning nucleotide gaps
    for(size_t i = 0; i < gaps.size(); i++){
        int32_t primaryBId = (gaps[i].primaryBlockId);
        int32_t secondaryBId = (gaps[i].secondaryBlockId);

        for(size_t j = 0; j < gaps[i].nucPosition.size(); j++){
            int len = gaps[i].nucGapLength[j];
            int pos = gaps[i].nucPosition[j];

            if(secondaryBId != -1){
                sequence[primaryBId].second[secondaryBId][pos].second.resize(len, '-');
            } else {
                sequence[primaryBId].first[pos].second.resize(len, '-');
            }
        }
    }

    printFASTAHelper(root, sequence, blockExists, fout, aligned);   

}

// Merge parent node and child node into parent node
void PangenomeMATNew::Tree::mergeNodes(PangenomeMATNew::Node* par, PangenomeMATNew::Node* chi){
    
    par->identifier = chi->identifier;
    par->branchLength += chi->branchLength;
    par->children = chi->children;

    // For block mutations, we cancel out irrelevant mutations
    std::map< std::pair<int, int>, PangenomeMATNew::BlockMutationType > bidMutations;

    for(auto mutation: par->blockMutation){
        int primaryBlockId = mutation.primaryBlockId;
        int secondaryBlockId = mutation.secondaryBlockId;

        bool type = (mutation.blockMutInfo);
        if(type == PangenomeMATNew::BlockMutationType::BI){
            bidMutations[std::make_pair(primaryBlockId, secondaryBlockId)] = PangenomeMATNew::BlockMutationType::BI;
        } else {
            if(bidMutations.find(std::make_pair(primaryBlockId, secondaryBlockId)) != bidMutations.end()){
                if(bidMutations[std::make_pair(primaryBlockId, secondaryBlockId)] == PangenomeMATNew::BlockMutationType::BI){
                    // If it was insertion earlier, cancel out
                    bidMutations.erase(std::make_pair(primaryBlockId, secondaryBlockId));
                }
                // Otherwise, it remains deletion
            } else {
                bidMutations[std::make_pair(primaryBlockId, secondaryBlockId)] = PangenomeMATNew::BlockMutationType::BD;
            }
        }
    }

    for(auto mutation: chi->blockMutation){
        int primaryBlockId = mutation.primaryBlockId;
        int secondaryBlockId = mutation.secondaryBlockId;

        int type = (mutation.blockMutInfo);
        if(type == PangenomeMATNew::BlockMutationType::BI){
            bidMutations[std::make_pair(primaryBlockId, secondaryBlockId)] = PangenomeMATNew::BlockMutationType::BI;
        } else {
            if(bidMutations.find(std::make_pair(primaryBlockId, secondaryBlockId)) != bidMutations.end()){
                if(bidMutations[std::make_pair(primaryBlockId, secondaryBlockId)] == PangenomeMATNew::BlockMutationType::BI){
                    // If it was insertion earlier, cancel out
                    bidMutations.erase(std::make_pair(primaryBlockId, secondaryBlockId));
                }
                // Otherwise, it remains deletion
            } else {
                bidMutations[std::make_pair(primaryBlockId, secondaryBlockId)] = PangenomeMATNew::BlockMutationType::BD;
            }
        }
    }

    std::vector< PangenomeMATNew::BlockMut > newBlockMutation;
    for(auto mutation: bidMutations){
        if(mutation.second == PangenomeMATNew::BlockMutationType::BI){
            PangenomeMATNew::BlockMut tempBlockMut;
            tempBlockMut.primaryBlockId = mutation.first.first;
            tempBlockMut.secondaryBlockId = mutation.first.second;
            tempBlockMut.blockMutInfo = PangenomeMATNew::BlockMutationType::BI;
            newBlockMutation.push_back( tempBlockMut );
        } else {
            PangenomeMATNew::BlockMut tempBlockMut;
            tempBlockMut.primaryBlockId = mutation.first.first;
            tempBlockMut.secondaryBlockId = mutation.first.second;
            tempBlockMut.blockMutInfo = PangenomeMATNew::BlockMutationType::BD;
            newBlockMutation.push_back( tempBlockMut );
        }
    }

    par->blockMutation = newBlockMutation;

    for(auto mutation: chi->nucMutation){
        par->nucMutation.push_back(mutation);
    }

    delete chi;
}

// Replace old type, char pair with new type char pair
std::pair< int, int > PangenomeMATNew::replaceMutation(std::pair<int,int> oldMutation, std::pair<int, int> newMutation){
    std::pair<int, int> ans = newMutation;
    if(oldMutation.first == newMutation.first){
        ans = newMutation;
    } else if(oldMutation.first == PangenomeMATNew::NucMutationType::NSNPS){
        // Insertion after substitution (doesn't make sense but just in case)
        if(newMutation.first == PangenomeMATNew::NucMutationType::NSNPI){
            ans.first = PangenomeMATNew::NucMutationType::NSNPS;
        } else if(newMutation.first == PangenomeMATNew::NucMutationType::NSNPD){
            ans = newMutation;
        }
    } else if(oldMutation.first == PangenomeMATNew::NucMutationType::NSNPI){
        if(newMutation.first == PangenomeMATNew::NucMutationType::NSNPS){
            ans.first = PangenomeMATNew::NucMutationType::NSNPI;
        } else if(newMutation.first == PangenomeMATNew::NucMutationType::NSNPD){
            // Cancel out the two mutations if deletion after insertion
            ans = std::make_pair(404, 404);
        }
    } else if(oldMutation.first == PangenomeMATNew::NucMutationType::NSNPD){
        if(newMutation.first == PangenomeMATNew::NucMutationType::NSNPI){
            ans.first = PangenomeMATNew::NucMutationType::NSNPS;
        } else if(newMutation.first == PangenomeMATNew::NucMutationType::NSNPS){
            // Substitution after deletion. Doesn't make sense but still
            ans.first = PangenomeMATNew::NucMutationType::NSNPI;
        }
    }
    return ans;
}

// bool debugSimilarity(const std::vector< PangenomeMAT::NucMut > array1, const std::vector< PangenomeMAT::NucMut > array2){
//     std::map< std::tuple< int, int, int >, std::pair< int, int > > mutationRecords1, mutationRecords2;

//     for(auto mutation: array1){
//         int bid = ((mutation.condensed) >> 8);
//         int pos = mutation.position;
//         int gapPos = mutation.gapPosition;

//         // I'm using int instead of NucMutationType because I want the 404 mutation too.
//         int type = ((mutation.condensed) & 0x7);
//         int len = (((mutation.condensed) >> 3) & 0x1F);

//         if(type >= 3){
//             len = 1;
//         }

//         // Replace variable length mutations into SNP. They will be combined later
//         int newType = type;
//         switch(type){
//             case PangenomeMAT::NucMutationType::NS:
//                 newType = PangenomeMAT::NucMutationType::NSNPS;
//                 break;
//             case PangenomeMAT::NucMutationType::ND:
//                 newType = PangenomeMAT::NucMutationType::NSNPD;
//                 break;
//             case PangenomeMAT::NucMutationType::NI:
//                 newType = PangenomeMAT::NucMutationType::NSNPI;
//                 break;
//         }

//         for(int i = 0; i < len; i++){
//             int newChar;
//             if(type < 3){
//                 newChar = (((mutation.nucs) >> (4*(15 - i))) & 0xF);
//             } else {
//                 // SNP
//                 newChar = ((mutation.condensed >> 3) & 0xF);
//             }

//             std::pair< int, int > newMutation = std::make_pair( newType, newChar );
//             if(gapPos != -1){
//                 if(mutationRecords1.find(std::make_tuple( bid, pos, gapPos + i )) == mutationRecords1.end()){
//                     mutationRecords1[std::make_tuple( bid, pos, gapPos + i )] = newMutation;
//                 } else {
//                     std::pair< int, int > oldMutation = mutationRecords1[std::make_tuple( bid, pos, gapPos + i )];
//                     newMutation = replaceMutation(oldMutation, newMutation);
//                     if(newMutation.first != 404){
//                         mutationRecords1[std::make_tuple( bid, pos, gapPos + i )] = newMutation;
//                     } else {
//                         mutationRecords1.erase(std::make_tuple( bid, pos, gapPos + i ));
//                     }
//                 }
//             } else {
//                 if(mutationRecords1.find(std::make_tuple( bid, pos + i, gapPos )) == mutationRecords1.end()){
//                     mutationRecords1[std::make_tuple( bid, pos + i, gapPos )] = newMutation;
//                 } else {
//                     std::pair< int, int > oldMutation = mutationRecords1[std::make_tuple( bid, pos + i, gapPos )];
//                     newMutation = replaceMutation(oldMutation, newMutation);
//                     if(newMutation.first != 404){
//                         mutationRecords1[std::make_tuple( bid, pos + i, gapPos )] = newMutation;
//                     } else {
//                         mutationRecords1.erase(std::make_tuple( bid, pos + i, gapPos ));
//                     }
//                 }
//             }
//         }
//     }

//     for(auto mutation: array2){
//         int bid = ((mutation.condensed) >> 8);
//         int pos = mutation.position;
//         int gapPos = mutation.gapPosition;

//         // I'm using int instead of NucMutationType because I want the 404 mutation too.
//         int type = ((mutation.condensed) & 0x7);
//         int len = (((mutation.condensed) >> 3) & 0x1F);

//         if(type >= 3){
//             len = 1;
//         }

//         // Replace variable length mutations into SNP. They will be combined later
//         int newType = type;
//         switch(type){
//             case PangenomeMAT::NucMutationType::NS:
//                 newType = PangenomeMAT::NucMutationType::NSNPS;
//                 break;
//             case PangenomeMAT::NucMutationType::ND:
//                 newType = PangenomeMAT::NucMutationType::NSNPD;
//                 break;
//             case PangenomeMAT::NucMutationType::NI:
//                 newType = PangenomeMAT::NucMutationType::NSNPI;
//                 break;
//         }

//         for(int i = 0; i < len; i++){
//             int newChar;
//             if(type < 3){
//                 newChar = (((mutation.nucs) >> (4*(15 - i))) & 0xF);
//             } else {
//                 // SNP
//                 newChar = ((mutation.condensed >> 3) & 0xF);
//             }

//             std::pair< int, int > newMutation = std::make_pair( newType, newChar );
//             if(gapPos != -1){
//                 if(mutationRecords2.find(std::make_tuple( bid, pos, gapPos + i )) == mutationRecords2.end()){
//                     mutationRecords2[std::make_tuple( bid, pos, gapPos + i )] = newMutation;
//                 } else {
//                     std::pair< int, int > oldMutation = mutationRecords2[std::make_tuple( bid, pos, gapPos + i )];
//                     newMutation = replaceMutation(oldMutation, newMutation);
//                     if(newMutation.first != 404){
//                         mutationRecords2[std::make_tuple( bid, pos, gapPos + i )] = newMutation;
//                     } else {
//                         mutationRecords2.erase(std::make_tuple( bid, pos, gapPos + i ));
//                     }
//                 }
//             } else {
//                 if(mutationRecords2.find(std::make_tuple( bid, pos + i, gapPos )) == mutationRecords2.end()){
//                     mutationRecords2[std::make_tuple( bid, pos + i, gapPos )] = newMutation;
//                 } else {
//                     std::pair< int, int > oldMutation = mutationRecords2[std::make_tuple( bid, pos + i, gapPos )];
//                     newMutation = replaceMutation(oldMutation, newMutation);
//                     if(newMutation.first != 404){
//                         mutationRecords2[std::make_tuple( bid, pos + i, gapPos )] = newMutation;
//                     } else {
//                         mutationRecords2.erase(std::make_tuple( bid, pos + i, gapPos ));
//                     }
//                 }
//             }
//         }
//     }

//     std::vector< std::tuple< int, int, int, int, int > > mutationArray1, mutationArray2;
//     for(auto u: mutationRecords1){
//         mutationArray1.push_back( std::make_tuple( std::get<0>(u.first), std::get<1>(u.first), std::get<2>(u.first), u.second.first, u.second.second ) );
//     }
//     for(auto u: mutationRecords2){
//         mutationArray2.push_back( std::make_tuple( std::get<0>(u.first), std::get<1>(u.first), std::get<2>(u.first), u.second.first, u.second.second ) );
//     }

//     if(mutationArray1.size() != mutationArray2.size()){
//         std::cout << "sizes don't match " << mutationArray1.size() << " " << mutationArray2.size() << std::endl;
//         return false;
//     }

//     for(size_t i = 0; i < mutationArray1.size(); i++){
//         if(mutationArray1[i] != mutationArray2[i]){
//             std::cout << i << "th index doesn't match" << std::endl;
//             return false;
//         }
//     }

//     return true;
// }

std::vector< PangenomeMATNew::NucMut > consolidateNucMutations(const std::vector< PangenomeMATNew::NucMut >& nucMutation){
    // primaryBid, secondaryBid, pos, gap_pos -> type, nuc
    std::map< std::tuple< int32_t, int32_t, int32_t, int32_t >, std::pair< int, int > > mutationRecords;
    for(auto mutation: nucMutation){
        int primaryBlockId = mutation.primaryBlockId;
        int secondaryBlockId = mutation.secondaryBlockId;
        int pos = mutation.nucPosition;
        int gapPos = mutation.nucGapPosition;

        // I'm using int instead of NucMutationType because I want the 404 mutation too.
        int type = ((mutation.mutInfo) & 0x7);
        int len = (((mutation.mutInfo) >> 4));

        if(type >= 3){
            len = 1;
        }

        // Replace variable length mutations into SNP. They will be combined later
        int newType = type;
        switch(type){
            case PangenomeMATNew::NucMutationType::NS:
                newType = PangenomeMATNew::NucMutationType::NSNPS;
                break;
            case PangenomeMATNew::NucMutationType::ND:
                newType = PangenomeMATNew::NucMutationType::NSNPD;
                break;
            case PangenomeMATNew::NucMutationType::NI:
                newType = PangenomeMATNew::NucMutationType::NSNPI;
                break;
        }

        for(int i = 0; i < len; i++){
            int newChar = (((mutation.nucs) >> (4*(5-i))) & 0xF);

            std::pair< int, int > newMutation = std::make_pair( newType, newChar );
            if(gapPos != -1){
                if(mutationRecords.find(std::make_tuple( primaryBlockId, secondaryBlockId, pos, gapPos + i )) == mutationRecords.end()){
                    mutationRecords[std::make_tuple( primaryBlockId, secondaryBlockId, pos, gapPos + i )] = newMutation;
                } else {
                    std::pair< int, int > oldMutation = mutationRecords[std::make_tuple( primaryBlockId, secondaryBlockId, pos, gapPos + i )];
                    newMutation = PangenomeMATNew::replaceMutation(oldMutation, newMutation);
                    if(newMutation.first != 404){
                        mutationRecords[std::make_tuple( primaryBlockId, secondaryBlockId, pos, gapPos + i )] = newMutation;
                    } else {
                        mutationRecords.erase(std::make_tuple( primaryBlockId, secondaryBlockId, pos, gapPos + i ));
                    }
                }
            } else {
                if(mutationRecords.find(std::make_tuple( primaryBlockId, secondaryBlockId, pos + i, gapPos )) == mutationRecords.end()){
                    mutationRecords[std::make_tuple( primaryBlockId, secondaryBlockId, pos + i, gapPos )] = newMutation;
                } else {
                    std::pair< int, int > oldMutation = mutationRecords[std::make_tuple( primaryBlockId, secondaryBlockId, pos + i, gapPos )];
                    newMutation = PangenomeMATNew::replaceMutation(oldMutation, newMutation);
                    if(newMutation.first != 404){
                        mutationRecords[std::make_tuple( primaryBlockId, secondaryBlockId, pos + i, gapPos )] = newMutation;
                    } else {
                        mutationRecords.erase(std::make_tuple( primaryBlockId, secondaryBlockId, pos + i, gapPos ));
                    }
                }
            }
        }
    }

    // primaryBlockId, secondaryBlockId, pos, gapPos, type, char
    std::vector< std::tuple< int, int, int, int, int, int > > mutationArray;
    for(auto u: mutationRecords){
        mutationArray.push_back( std::make_tuple( std::get<0>(u.first), std::get<1>(u.first), std::get<2>(u.first), std::get<3>(u.first), u.second.first, u.second.second ) );
    }
    
    // mutation array is already sorted since mutationRecord was sorted
    std::vector< PangenomeMATNew::NucMut > consolidatedMutationArray;

    for(size_t i = 0; i < mutationArray.size(); i++){
        size_t j = i + 1;
        for(; j < std::min(i + 6, mutationArray.size()); j++){
            if(std::get<3>(mutationArray[i]) != -1){
                // gapPos exists
                if(!(std::get<0>(mutationArray[i]) == std::get<0>(mutationArray[j]) && std::get<1>(mutationArray[i]) == std::get<1>(mutationArray[j]) && std::get<2>(mutationArray[i]) == std::get<2>(mutationArray[j])
                    && std::get<4>(mutationArray[i]) == std::get<4>(mutationArray[j]) && (size_t)(std::get<3>(mutationArray[j]) - std::get<3>(mutationArray[i])) == j - i)){
                    break;
                }
            } else {
                if(!(std::get<0>(mutationArray[i]) == std::get<0>(mutationArray[j]) && std::get<1>(mutationArray[i]) == std::get<1>(mutationArray[j]) && (size_t)(std::get<2>(mutationArray[j]) - std::get<2>(mutationArray[i])) == j - i
                    && std::get<4>(mutationArray[i]) == std::get<4>(mutationArray[j]) && std::get<3>(mutationArray[j]) == std::get<3>(mutationArray[i]))){
                    break;
                }
            }
        }

        if(j - i <= 1){
            consolidatedMutationArray.push_back(PangenomeMATNew::NucMut(mutationArray[i]));
            continue;
        }
        // combine mutations from i to j
        auto newMutation = PangenomeMATNew::NucMut(mutationArray, i, j);

        consolidatedMutationArray.push_back(newMutation);

        i = j - 1;
    }

    return consolidatedMutationArray;

}

// void dfsExpansion(PangenomeMAT::Node* node, std::vector< PangenomeMAT::Node* >& vec){
//     vec.push_back(node);
//     for(auto child: node->children){
//         dfsExpansion(child, vec);
//     }
// }

// std::string PangenomeMAT::Tree::getNewickString(Node* node){
//     std::vector< PangenomeMAT::Node* > traversal;
//     dfsExpansion(node, traversal);

//     std::string newick;

//     size_t level_offset = node->level-1;
//     size_t curr_level = 0;
//     bool prev_open = true;

//     std::stack<std::string> node_stack;
//     std::stack<float> branch_length_stack;

//     for (auto n: traversal) {
//         size_t level = n->level-level_offset;
//         float branch_length = n->branchLength;
        
//         if(curr_level < level){
//             if (!prev_open) {
//                 newick += ',';
//             }
//             size_t l = level - 1;
//             if (curr_level > 1) {
//                 l = level - curr_level;
//             }
//             for (size_t i=0; i < l; i++) {
//                 newick += '(';
//                 prev_open = true;
//             }
//             if (n->children.size() == 0) {

//                 newick += n->identifier;

//                 if (branch_length >= 0) {
//                     newick += ':';
//                     newick += branch_length;
//                 }
//                 prev_open = false;
//             } else {
//                 node_stack.push(n->identifier);
//                 branch_length_stack.push(branch_length);
//             }
//         } else if (curr_level > level) {
//             prev_open = false;
//             for (size_t i = level; i < curr_level; i++) {
//                 newick += ')';

//                 newick += node_stack.top();

//                 if (branch_length_stack.top() >= 0) {
//                     newick += ':';
//                     newick += branch_length_stack.top();
//                 }
//                 node_stack.pop();
//                 branch_length_stack.pop();
//             }
//             if (n->children.size() == 0) {
                
//                 newick += ',';
//                 newick += n->identifier;

//                 if (branch_length >= 0) {
//                     newick += ':';
//                     newick += branch_length;
//                 }
//             } else {
//                 node_stack.push(n->identifier);
//                 branch_length_stack.push(branch_length);
//             }
//         } else {
//             prev_open = false;
//             if (n->children.size() == 0) {
                
//                 newick += ',';
//                 newick += n->identifier;

//                 if (branch_length >= 0) {
//                     newick += ':';
//                     newick += branch_length;
//                 }
//             } else {
//                 node_stack.push(n->identifier);
//                 branch_length_stack.push(branch_length);
//             }
//         }
//         curr_level = level;
//     }
//     size_t remaining = node_stack.size();
//     for (size_t i = 0; i < remaining; i++) {
//         newick += ')';
//         newick += node_stack.top();
        
//         if (branch_length_stack.top() >= 0) {
//             newick += ':';
//             newick += branch_length_stack.top();
//         }
//         node_stack.pop();
//         branch_length_stack.pop();
//     }

//     newick += ';';

//     return newick;

// }

void PangenomeMATNew::Tree::compressTreeParallel(PangenomeMATNew::Node* node, size_t level){
    node->level = level;

    if(node->children.size() == 0){
        return;
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, (int)node->children.size()), [&](tbb::blocked_range<int> r){
        for(int i = r.begin(); i < r.end(); i++){
            while(node->children[i]->children.size() == 1){
                mergeNodes(node->children[i], node->children[i]->children[0]);
            }
            // consolidate mutations of parent
            auto oldVector = node->children[i]->nucMutation;

            node->children[i]->nucMutation = consolidateNucMutations(node->children[i]->nucMutation);

//             debugSimilarity(oldVector, node->children[i]->nucMutation);

            compressTreeParallel(node->children[i], level + 1);
        }
    });
}

PangenomeMATNew::Node* subtreeExtractParallelHelper(PangenomeMATNew::Node* node, const tbb::concurrent_unordered_map< PangenomeMATNew::Node*, size_t >& ticks){
    if(ticks.find(node) == ticks.end()){
        return nullptr;
    }

    PangenomeMATNew::Node* newNode = new PangenomeMATNew::Node(node->identifier, node->branchLength);

    for(auto mutation: node->nucMutation){
        newNode->nucMutation.push_back(mutation);
    }

    for(auto mutation: node->blockMutation){
        newNode->blockMutation.push_back(mutation);
    }

    newNode->children.resize(node->children.size(), nullptr);

    tbb::parallel_for(tbb::blocked_range(0, (int)node->children.size()), [&](tbb::blocked_range<int> r){
        for(int i = r.begin(); i < r.end(); i++){
            PangenomeMATNew::Node* child = node->children[i];
            if(ticks.find(child) != ticks.end()){

                PangenomeMATNew::Node* newChild = subtreeExtractParallelHelper(child, ticks);

                newChild->parent = newNode;
                newNode->children[i] = newChild;
            }
        }
    });

    size_t i = 0, j = 0;
    while(j < newNode->children.size()){
        if(newNode->children[j] != nullptr){
            std::swap(newNode->children[i], newNode->children[j]);
            i++;
        }
        j++;
    }
    newNode->children.resize(i);
    
    return newNode;

}

PangenomeMATNew::Node* PangenomeMATNew::Tree::subtreeExtractParallel(std::vector< std::string > nodeIds){
    tbb::concurrent_vector< PangenomeMATNew::Node* > requiredNodes;
    
    tbb::parallel_for_each(nodeIds.begin(), nodeIds.end(), [&]( std::string& id ) {
        requiredNodes.push_back(allNodes[id]);
    });

    tbb::concurrent_unordered_map< PangenomeMATNew::Node*, size_t > ticks;

    tbb::parallel_for_each(requiredNodes.begin(), requiredNodes.end(), [&](PangenomeMATNew::Node*& node){
        Node* current = node;

        while(current != nullptr){
            ticks[current]++;
            current = current->parent;
        }
    });

    PangenomeMATNew::Node* newTreeRoot = subtreeExtractParallelHelper(root, ticks);

    compressTreeParallel(newTreeRoot, 1);

    return newTreeRoot;
}

// void getNodesPreorder(PangenomeMAT::Node* root, MAT::tree& treeToWrite){
    
//     MAT::node n;
    
//     MAT::block_mut bm;
//     for(auto mutation: root->blockMutation.condensedBlockMut){
//         bm.add_condensed_block_mut(mutation);
//     }

//     *n.mutable_block_mutation() = bm;

//     for(size_t i = 0; i < root->nucMutation.size(); i++){
//         const PangenomeMAT::NucMut& mutation = root->nucMutation[i];

//         MAT::nuc_mut nm;
//         nm.set_position(mutation.position);
//         if(mutation.gapPosition != -1){
//             nm.set_gap_position(mutation.gapPosition);
//         }
//         nm.set_condensed(mutation.condensed);
//         nm.set_nucs(mutation.nucs);

//         n.add_nuc_mutation();
//         *n.mutable_nuc_mutation(i) = nm;
//     }

//     treeToWrite.add_nodes();
//     *treeToWrite.mutable_nodes( treeToWrite.nodes_size() - 1 ) = n;

//     for(auto child: root->children){
//         getNodesPreorder(child, treeToWrite);
//     }
// }

// void PangenomeMAT::Tree::writeToFile(std::ofstream& fout, Node* node){
//     if(node == nullptr){
//         node = root;
//     }

//     MAT::tree treeToWrite;
//     getNodesPreorder(node, treeToWrite);

//     std::string newick = getNewickString(node);

//     treeToWrite.set_newick(newick);

//     for(auto block: blocks){
//         MAT::block b;
//         b.set_block_id(block.blockId);
//         b.set_chromosome_name(block.chromosomeName);
//         for(auto n: block.consensusSeq){
//             b.add_consensus_seq(n);
//         }
//         treeToWrite.add_blocks();
//         *treeToWrite.mutable_blocks( treeToWrite.blocks_size() - 1 ) = b;
//     }

//     MAT::gap_list gl;
//     for(size_t i = 0; i < gaps.position.size(); i++){
//         gl.add_position(gaps.position[i]);
//         gl.add_block_id(gaps.blockId[i]);
//         gl.add_gap_length(gaps.gapLength[i]);
//     }

//     *treeToWrite.mutable_gaps() = gl;

//     if (!treeToWrite.SerializeToOstream(&fout)) {
// 		std::cerr << "Failed to write output file." << std::endl;
//     }

// }

// std::string PangenomeMAT::Tree::getStringFromReference(std::string reference){
//     Node* referenceNode = nullptr;
    
//     for(auto u: allNodes){
//         if(u.second->children.size() == 0 && u.first == reference){
//             referenceNode = u.second;
//             break;
//         }
//     }

//     if(referenceNode == nullptr){
//         return "No such leaf node found.";
//     }

//     // Categorize gaps by blockId
//     std::map< int, std::vector< std::pair< int, int > > > gapSplit;
    
//     for(size_t i = 0; i < gaps.position.size(); i++){

//         int bId = gaps.blockId[i];

//         int len = gaps.gapLength[i];

//         int pos = gaps.position[i];
//         gapSplit[bId].push_back( std::make_pair(pos, len) );

//     }

//     std::vector< PangenomeMAT::Node* > path;
//     Node* it = referenceNode;

//     while(it != root){
//         path.push_back(it);
//         it = it->parent;
//     }
//     path.push_back(root);

//     // Get all blocks on the path
//     std::unordered_set< uint32_t > blockIds;
//     for(auto node = path.rbegin(); node != path.rend(); node++){
//         for(uint32_t mutation: (*node)->blockMutation.condensedBlockMut){
//             int bid = ((mutation >> 8) & 0xFFFFFF);
//             int type = (mutation & 0x1);

//             if(type == PangenomeMAT::BlockMutationType::BI){
//                 blockIds.insert(bid);
//             } else {
//                 blockIds.erase(bid);
//             }
//         }
//     }

//     // Create the required blocks
//     std::map< int, std::vector< std::pair< char, std::vector< char > > > > sequence;
//     for(auto bid: blockIds){
//         if(blocks[bid - 1].blockId != bid){
//             std::cout << "Block not in correct position in blocks array" << std::endl;
//         }

//         for(size_t i = 0; i < blocks[bid - 1].consensusSeq.size(); i++){
//             for(int j = 0; j < 8; j++){
//                 const int nucCode = (((blocks[bid - 1].consensusSeq[i]) >> (4*(7 - j))) & 0xF);
//                 sequence[bid].push_back({ getNucleotideFromCode(nucCode), {} });
//             }
//         }
//         sequence[bid].push_back({'x',{}});

//         for(auto g: gapSplit[bid]){
//             sequence[bid][g.first].second.resize(g.second, '-');
//         }

//     }

//     // Apply nucleotide mutations
//     for(auto node = path.rbegin(); node != path.rend(); node++){

//         for(size_t i = 0; i < (*node)->nucMutation.size(); i++){

//             int bid = (((*node)->nucMutation[i].condensed >> 8) & 0xFFFFFF);

//             if(sequence.find(bid) == sequence.end()){
//                 continue;
//             }

//             int pos = (*node)->nucMutation[i].position;
//             int gapPos = (*node)->nucMutation[i].gapPosition;
//             int type = (((*node)->nucMutation[i].condensed) & 0x7);
//             char newVal = '-';

//             if(type < 3){
//                 // Either S, I or D

//                 int len = ((((*node)->nucMutation[i].condensed) >> 3) & 0x1F);

//                 if(type == PangenomeMAT::NucMutationType::NS){
//                     for(int j = 0; j < len; j++){
//                         newVal = getNucleotideFromCode((((*node)->nucMutation[i].nucs) >> (4*(15-j))) & 15);

//                         sequence[bid][pos + j].first = newVal;
//                     }
//                 }
//                 else if(type == PangenomeMAT::NucMutationType::NI){

//                     if(gapPos == -1){
//                         for(int j = 0; j < len; j++){
//                             newVal = getNucleotideFromCode((((*node)->nucMutation[i].nucs) >> (4*(15-j))) & 15);
                            
//                             sequence[bid][pos + j].first = newVal;
//                         }
//                     } else {
//                         for(int j = 0; j < len; j++){
//                             newVal = getNucleotideFromCode((((*node)->nucMutation[i].nucs) >> (4*(15-j))) & 15);

//                             sequence[bid][pos].second[gapPos + j] = newVal;
//                         }
//                     }
//                 } else if(type == PangenomeMAT::NucMutationType::ND){
//                     if(gapPos == -1){
//                         for(int j = 0; j < len; j++){
//                             sequence[bid][pos + j].first = '-';
//                         }
//                     } else {
//                         for(int j = 0; j < len; j++){
//                             sequence[bid][pos].second[gapPos + j] = '-';
//                         }
//                     }
//                 }
//             } else {
//                 if(type == PangenomeMAT::NucMutationType::NSNPS){
//                     newVal = getNucleotideFromCode((((*node)->nucMutation[i].condensed) >> 3) & 0xF);
//                     sequence[bid][pos].first = newVal;
//                 }
//                 else if(type == PangenomeMAT::NucMutationType::NSNPI){
//                     newVal = getNucleotideFromCode((((*node)->nucMutation[i].condensed) >> 3) & 0xF);

//                     if(gapPos == -1){
//                         sequence[bid][pos].first = newVal;
//                     } else {
//                         sequence[bid][pos].second[gapPos] = newVal;
//                     }
//                 }
//                 else if(type == PangenomeMAT::NucMutationType::NSNPD){
//                     if(gapPos == -1){
//                         sequence[bid][pos].first = '-';
//                     } else {
//                         sequence[bid][pos].second[gapPos] = '-';
//                     }
//                 }
//             }
//         }
//     }

//     std::string sequenceString;
//     for(size_t i = 0; i < blocks.size(); i++){
//         if(sequence.find(blocks[i].blockId) == sequence.end()){
//             for(size_t j = 0; j < blocks[i].consensusSeq.size(); j++){
//                 for(int k = 0; k < 8; k++){
//                     sequenceString += '-';
//                 }
//             }
//             sequenceString += '-'; // For last character that might have been inserted (x)

//             for(auto g: gapSplit[blocks[i].blockId]){
//                 for(int j = 0; j < g.second; j++){
//                     sequenceString += '-';
//                 }
//             }
//         } else {
//             for(size_t j = 0; j < sequence[blocks[i].blockId].size(); j++){
//                 for(size_t k = 0; k < sequence[blocks[i].blockId][j].second.size(); k++){
//                     sequenceString += sequence[blocks[i].blockId][j].second[k];
//                 }

//                 if(sequence[blocks[i].blockId][j].first != 'x'){
//                     sequenceString += sequence[blocks[i].blockId][j].first;
//                 } else {
//                     sequenceString += '-';
//                 }
//             }
//         }
//     }

//     return sequenceString;

// }

// std::string stripString(std::string s){
//     while(s.length() && s[s.length() - 1] == ' '){
//         s.pop_back();
//     }
//     for(size_t i = 0; i < s.length(); i++){
//         if(s[i] != ' '){
//             return s.substr(i);
//         }
//     }
//     return s;
// }

// std::string stripGaps(std::string sequenceString){
//     std::string result;
//     for(auto u: sequenceString){
//         if(u != '-'){
//             result+=u;
//         }
//     }
//     return result;
// }

// std::string PangenomeMAT::Tree::getSequenceFromVCF(std::string sequenceId, std::ifstream& fin){
//     std::string line;

//     // get reference line
//     for(int i = 0; i < 4; i++){
//         std::getline(fin, line);
//     }

//     if(line.substr(0,12) != "##reference="){
//         std::cout << "Incorrect line format: " << line << std::endl;
//         return "";
//     }

//     std::string referenceSequenceId = line.substr(12);

//     std::string referenceSequence = stripGaps(getStringFromReference(referenceSequenceId));

//     // column headers
//     std::getline(fin, line);

//     std::vector< std::string > columnWords;
//     std::string word;

//     for(size_t i = 0; i < line.size(); i++){
//         if(line[i] != ' '){
//             word += line[i];
//         } else {
//             if(word.length()){
//                 columnWords.push_back(word);
//                 word = "";
//             }
//         }
//     }
//     if(word.length()){
//         columnWords.push_back(word);
//     }

//     int sequenceIndex = -1;

//     for(size_t i = 9; i < columnWords.size(); i++){
//         if(columnWords[i] == sequenceId){
//             sequenceIndex = i;
//             break;
//         }
//     }

//     if(sequenceIndex == -1){
//         std::cout << "sequence not found!" << std::endl;
//         return "";
//     }

//     std::vector< std::pair< char, std::vector< char > > > alteredSequence;
//     for(auto u: referenceSequence){
//         alteredSequence.push_back({u, {}});
//     }

//     while(getline(fin, line)){
//         std::vector< std::string > words;
//         std::string word;

//         for(size_t i = 0; i < line.size(); i++){
//             if(line[i] != ' '){
//                 word += line[i];
//             } else {
//                 if(word.length()){
//                     words.push_back(word);
//                     word = "";
//                 }
//             }
//         }
//         if(word.length()){
//             words.push_back(word);
//             word="";
//         }

//         int choice = std::stoll(words[sequenceIndex]);
//         if(choice == 0){
//             continue;
//         }

//         choice--;

//         int position = std::stoll(words[1]);

//         // if(position >= alteredSequence.size()){
//         //     std::cout << "position too high " << position << " " << alteredSequence.size() << std::endl;
//         // }

//         std::string ref = words[3];
//         std::string altStrings = words[4];

//         std::string currentAlt;
//         std::vector< std::string > altChoices;

//         for(auto u: altStrings){
//             if(u != ','){
//                 currentAlt += u;
//             } else {
//                 if(currentAlt.length()){
//                     altChoices.push_back(currentAlt);
//                     currentAlt = "";
//                 }
//             }
//         }

//         if(currentAlt.length()){
//             altChoices.push_back(currentAlt);
//             currentAlt = "";
//         }

//         // if(choice >= altChoices.size()){
//         //     std::cout << altChoices.size() << " " << choice << " " << altStrings << " " << position << std::endl;
//         // }

//         std::string alt = altChoices[choice];

//         if(ref != "."){
//             int len = ref.length();
//             for(int i = position; i < position + len; i++){
//                 // if(i >= alteredSequence.size()){
//                 //     std::cout << "index exceeding!!!" << std::endl;
//                 // }
//                 alteredSequence[i].first = '-';
//             }
//         }

//         if(alt != "."){
//             if(alt.length() && alteredSequence[position].second.size()){
//                 std::cout << "alternate sequence already exists!" << std::endl;
//             }
//             for(size_t i = 0; i < alt.length(); i++){
//                 alteredSequence[position].second.push_back(alt[i]);
//             }
//         }

//     }

//     std::string finalSequence;
//     for(size_t i = 0; i < alteredSequence.size(); i++){
//         for(size_t j = 0; j < alteredSequence[i].second.size();j++){
//             if(alteredSequence[i].second[j] != '-'){
//                 finalSequence += alteredSequence[i].second[j];
//             }
//         }
//         if(alteredSequence[i].first != '-'){
//             finalSequence += alteredSequence[i].first;
//         }

//     }

//     std::string alteredSequenceOriginal = stripGaps(getStringFromReference(sequenceId));

//     std::cout << (alteredSequenceOriginal.length() == finalSequence.length()) << (alteredSequenceOriginal == finalSequence) << std::endl;

//     return finalSequence;

// }

// void PangenomeMAT::Tree::printVCF(std::string reference, std::ofstream& fout){

//     std::string referenceSequence = getStringFromReference(reference);

//     if(referenceSequence == "No such leaf node found."){
//         std::cerr << referenceSequence << std::endl;
//         return;
//     }

//     size_t recordID = 0;

//     std::map< int, std::map< std::string, std::map< std::string, std::vector< std::string > > > > vcfMap;

//     for(auto n: allNodes){
//         if(n.second->children.size() == 0 && n.first != reference){
//             std::string altSequence = getStringFromReference(n.first);
//             if(altSequence.length() != referenceSequence.length()){
//                 std::cerr << "Logic error. String lengths don't match: " << referenceSequence.length() << " " << altSequence.length() << std::endl;
//                 return;
//             }

//             std::string currentRefString, currentAltString;
//             int currentCoordinate = 0;

//             int diffStart = 0;

//             for(size_t i = 0; i < referenceSequence.length(); i++){

//                 if(referenceSequence[i] == '-' && altSequence[i] == '-'){
//                     continue;
//                 } else if(referenceSequence[i] != '-' && altSequence[i] == '-'){
//                     if(currentRefString == "" && currentAltString == ""){
//                         diffStart = currentCoordinate;
//                     }

//                     currentRefString += referenceSequence[i];
//                 } else if(referenceSequence[i] == '-' && altSequence[i] != '-'){
//                     if(currentRefString == "" && currentAltString == ""){
//                         diffStart = currentCoordinate;
//                     }

//                     currentAltString += altSequence[i];
//                 } else if(referenceSequence[i] != altSequence[i]){
//                     if(currentRefString == "" && currentAltString == ""){
//                         diffStart = currentCoordinate;
//                     }
//                     if(currentRefString == currentAltString){
//                         currentRefString = "";
//                         currentAltString = "";
//                         diffStart = currentCoordinate;
//                     }
//                     currentRefString += referenceSequence[i];
//                     currentAltString += altSequence[i];
//                 } else if(referenceSequence[i] == altSequence[i]){
//                     if(currentRefString == currentAltString){
//                         // Reset
//                         diffStart = currentCoordinate;
//                         currentRefString = "";
//                         currentRefString += referenceSequence[i];
//                         currentAltString = currentRefString;
//                     } else {
//                         // Create VCF record at position i

//                         vcfMap[diffStart][currentRefString][currentAltString].push_back(n.first);

//                         // Reset
//                         diffStart = currentCoordinate;
//                         currentRefString = "";
//                         currentRefString += referenceSequence[i];
//                         currentAltString = currentRefString;
//                     }
//                 }

//                 if(referenceSequence[i] != '-'){
//                     currentCoordinate++;
//                 }
//             }

//             if(currentRefString != currentAltString){
//                 vcfMap[diffStart][currentRefString][currentAltString].push_back(n.first);
//                 // Reset
//                 diffStart = referenceSequence.size();
//                 currentRefString = "";
//                 currentAltString = currentRefString;
//             }
//         }
//     }

//     std::map< std::string, size_t > sequenceIds;
//     for(auto u: allNodes){
//         if(u.second->children.size() == 0 && u.first != reference){
//             sequenceIds[u.first] = 0;
//         }
//     }


//     fout << "##fileformat=VCFv" << VCF_VERSION << '\n';
//     fout << "##fileDate=" << getDate() << '\n';
//     fout << "##source=PanMATv" << PMAT_VERSION << '\n';
//     fout << "##reference=" << reference << '\n';
//     fout << std::left << std::setw(20) << "#CHROM " << std::setw(20) << "POS " << std::setw(20) << "ID " << std::setw(20) << "REF " << std::setw(20) << "ALT " << std::setw(20) << "QUAL " << std::setw(20) << "FILTER " << std::setw(20) << "INFO " << std::setw(20) << "FORMAT ";
//     for(auto u: sequenceIds){
//         fout << std::left << std::setw(20) << u.first + " ";
//     }
//     fout << '\n';

//     for(auto u: vcfMap){
//         for(auto v: u.second){
//             if(v.first == ""){
//                 fout << std::left << std::setw(20) << ". " << std::setw(20) << u.first << " " << std::setw(20) << recordID++ << " " << std::setw(20) << ". ";
//             } else {
//                 fout << std::left << std::setw(20) << ". " << std::setw(20) << u.first << " " << std::setw(20) << recordID++ << " " << std::setw(20) << v.first << " ";
//             }
            
//             std::map< std::string, size_t > tempSequenceIds = sequenceIds;

//             int ctr = 1;
//             std::string altStrings;

//             for(auto w: v.second){
//                 altStrings += (w.first == "" ? ".": w.first);
//                 altStrings += ",";
//                 for(auto uu: w.second){
//                     tempSequenceIds[uu] = ctr;
//                 }
//                 ctr++;
//             }

//             altStrings.pop_back();
//             // altStrings += "\t.\t.\t.\t.\t";

//             fout << std::left << std::setw(20) << altStrings << " " << std::setw(20) << ". " << std::setw(20) << ". " << std::setw(20) << ". " << std::setw(20) << ". ";

//             for(auto w: tempSequenceIds){
//                 fout << std::left << std::setw(20) << w.second << " ";
//             }

//             fout << '\n';
//         }
//     }
// }

// void PangenomeMAT::Tree::printVCFParallel(std::string reference, std::ofstream& fout){

//     std::string referenceSequence = getStringFromReference(reference);

//     if(referenceSequence == "No such leaf node found."){
//         std::cerr << referenceSequence << std::endl;
//         return;
//     }

//     size_t recordID = 0;

//     std::mutex vcfMapMutex;
//     std::map< int, std::map< std::string, std::map< std::string, std::vector< std::string > > > > vcfMap;

//     tbb::parallel_for_each(allNodes, [&](auto& n){
//         if(n.second->children.size() == 0 && n.first != reference){
//             std::string altSequence = getStringFromReference(n.first);
//             if(altSequence.length() != referenceSequence.length()){
//                 std::cerr << "Logic error. String lengths don't match: " << referenceSequence.length() << " " << altSequence.length() << std::endl;
//                 return;
//             }

//             std::string currentRefString, currentAltString;
//             int currentCoordinate = 0;

//             int diffStart = 0;

//             for(size_t i = 0; i < referenceSequence.length(); i++){

//                 if(referenceSequence[i] == '-' && altSequence[i] == '-'){
//                     continue;
//                 } else if(referenceSequence[i] != '-' && altSequence[i] == '-'){
//                     if(currentRefString == "" && currentAltString == ""){
//                         diffStart = currentCoordinate;
//                     }

//                     currentRefString += referenceSequence[i];
//                 } else if(referenceSequence[i] == '-' && altSequence[i] != '-'){
//                     if(currentRefString == "" && currentAltString == ""){
//                         diffStart = currentCoordinate;
//                     }

//                     currentAltString += altSequence[i];
//                 } else if(referenceSequence[i] != altSequence[i]){
//                     if(currentRefString == "" && currentAltString == ""){
//                         diffStart = currentCoordinate;
//                     }
//                     if(currentRefString == currentAltString){
//                         currentRefString = "";
//                         currentAltString = "";
//                         diffStart = currentCoordinate;
//                     }
//                     currentRefString += referenceSequence[i];
//                     currentAltString += altSequence[i];
//                 } else if(referenceSequence[i] == altSequence[i]){
//                     if(currentRefString == currentAltString){
//                         // Reset
//                         diffStart = currentCoordinate;
//                         currentRefString = "";
//                         currentRefString += referenceSequence[i];
//                         currentAltString = currentRefString;
//                     } else {
//                         // Create VCF record at position i

//                         vcfMapMutex.lock();
//                         vcfMap[diffStart][currentRefString][currentAltString].push_back(n.first);
//                         vcfMapMutex.unlock();

//                         // Reset
//                         diffStart = currentCoordinate;
//                         currentRefString = "";
//                         currentRefString += referenceSequence[i];
//                         currentAltString = currentRefString;
//                     }
//                 }

//                 if(referenceSequence[i] != '-'){
//                     currentCoordinate++;
//                 }
//             }

//             if(currentRefString != currentAltString){
//                 vcfMapMutex.lock();
//                 vcfMap[diffStart][currentRefString][currentAltString].push_back(n.first);
//                 vcfMapMutex.unlock();

//                 // Reset
//                 diffStart = referenceSequence.size();
//                 currentRefString = "";
//                 currentAltString = currentRefString;
//             }
//         }
//     });

//     std::mutex sequenceIdsMutex;
//     std::map< std::string, size_t > sequenceIds;
//     tbb::parallel_for_each(allNodes, [&](auto& u){
//         if(u.second->children.size() == 0 && u.first != reference){
//             sequenceIdsMutex.lock();
//             sequenceIds[u.first] = 0;
//             sequenceIdsMutex.unlock();
//         }
//     });


//     fout << "##fileformat=VCFv" << VCF_VERSION << '\n';
//     fout << "##fileDate=" << getDate() << '\n';
//     fout << "##source=PanMATv" << PMAT_VERSION << '\n';
//     fout << "##reference=" << reference << '\n';
//     fout << std::left << std::setw(20) << "#CHROM " << std::setw(20) << "POS " << std::setw(20) << "ID " << std::setw(20) << "REF " << std::setw(20) << "ALT " << std::setw(20) << "QUAL " << std::setw(20) << "FILTER " << std::setw(20) << "INFO " << std::setw(20) << "FORMAT ";
//     for(auto u: sequenceIds){
//         fout << std::left << std::setw(20) << u.first + " ";
//     }
//     fout << '\n';

//     for(auto u: vcfMap){
//         for(auto v: u.second){
//             if(v.first == ""){
//                 fout << std::left << std::setw(20) << ". " << std::setw(20) << u.first << " " << std::setw(20) << recordID++ << " " << std::setw(20) << ". ";
//             } else {
//                 fout << std::left << std::setw(20) << ". " << std::setw(20) << u.first << " " << std::setw(20) << recordID++ << " " << std::setw(20) << v.first << " ";
//             }
            
//             std::map< std::string, size_t > tempSequenceIds = sequenceIds;

//             int ctr = 1;
//             std::string altStrings;

//             for(auto w: v.second){
//                 altStrings += (w.first == "" ? ".": w.first);
//                 altStrings += ",";
//                 for(auto uu: w.second){
//                     tempSequenceIds[uu] = ctr;
//                 }
//                 ctr++;
//             }

//             altStrings.pop_back();
//             // altStrings += "\t.\t.\t.\t.\t";

//             fout << std::left << std::setw(20) << altStrings << " " << std::setw(20) << ". " << std::setw(20) << ". " << std::setw(20) << ". " << std::setw(20) << ". ";

//             for(auto w: tempSequenceIds){
//                 fout << std::left << std::setw(20) << w.second << " ";
//             }

//             fout << '\n';
//         }
//     }
// }

// std::vector< std::string > PangenomeMAT::Tree::searchByAnnotation(std::string annotation){
//     if(annotationsToNodes.find(annotation) != annotationsToNodes.end()){
//         return annotationsToNodes[annotation];
//     }
//     return {};
// }

// void PangenomeMAT::Tree::annotate(std::ifstream& fin){
//     std::string line;
//     while(getline(fin, line)){
//         std::string word;
//         std::string nodeId;

//         // Extract node ID
//         size_t i = 0;
//         for(;i < line.length() && line[i]!=','; i++){
//             word+=line[i];
//         }

//         word = stripString(word);

//         if(word.length()){
//             nodeId = word;
//             word = "";
//         } else {
//             std::cout << "File in incorrect format. Line: " << line << std::endl;
//             return;
//         }

//         if(i >= line.length()){
//             // comma not found
//             std::cout << "File in incorrect format. Line: " << line << std::endl;
//             return;
//         }

//         if(allNodes.find(nodeId) == allNodes.end()){
//             std::cout << "Node ID not found. Line: " << line << std::endl;
//         }

//         Node* nodeToAnnotate = allNodes[nodeId];

//         // Extract annotations
//         for(;i < line.length(); i++){
//             if(line[i] != ','){
//                 word += line[i];
//             } else {
//                 word = stripString(word);
//                 if(word.length()){
//                     std::string annotation = word;
//                     nodeToAnnotate->annotations.push_back(annotation);
//                     annotationsToNodes[annotation].push_back(nodeId);
//                     word = "";
//                 }
//             }
//         }

//         word = stripString(word);
//         if(word.length()){
//             std::string annotation = word;
//             nodeToAnnotate->annotations.push_back(annotation);
//             annotationsToNodes[annotation].push_back(nodeId);
//             word = "";
//         }

//     }
// }