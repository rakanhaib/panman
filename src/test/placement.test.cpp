#define BOOST_TEST_MODULE Index construction
#include <boost/test/included/unit_test.hpp>
#include "../PangenomeMAT.hpp"
#include <stack>
#include <boost/program_options.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <iostream>
#include <json/json.h>
#include <filesystem>




using namespace PangenomeMAT;

BOOST_AUTO_TEST_CASE(_removeIndices) {
/* should erase elements at the given positions */
    std::vector<kmer_t> a = {
        kmer_t{"zero"},
        kmer_t{"one"},
        kmer_t{"two"},
        kmer_t{"three"},
        kmer_t{"four"},
        kmer_t{"five"}      
    };
    
    // smaller values at the top
    std::stack<int32_t> indices;

    indices.push(4);
    indices.push(2);
    indices.push(0);

    std::vector<kmer_t> expected = {
        kmer_t{"one"},
        kmer_t{"three"},
        kmer_t{"five"}
    };

    removeIndices(a, indices);

    BOOST_TEST(
        a == expected
    );

}

BOOST_AUTO_TEST_CASE(_isSyncmer) {
/* should return true only for syncmers */
    std::string a = "ABCDEF";

    BOOST_TEST(
        is_syncmer(a, 3, true) == true
    );
    BOOST_TEST(
		is_syncmer(a, 3, false) == true
    );
    
    std::string b = "ZZZAAAZZZ";

    BOOST_TEST(
		is_syncmer(b, 6, true) == false
    );
    BOOST_TEST(
		is_syncmer(b, 6, false) == true
    );

    std::string c = "Z";
 
    BOOST_TEST(
		is_syncmer(c, 1, true) == true
    );
}

BOOST_AUTO_TEST_CASE(_syncmerize) {
/* should pick syncmers correctly */
    std::string a = "AAAAAABB"; // k=3 s=2 open
    /*             * AAA
                   *  AAA
                   *   AAA
                   *    AAA
                   *     AAB
                   *      ABB
    */
    std::vector<kmer_t> ret_a = {
        kmer_t{"AAA", 0, -1},
        kmer_t{"AAA", 1, -1},
        kmer_t{"AAA", 2, -1},
        kmer_t{"AAA", 3, -1},
        kmer_t{"AAB", 4, -1},
        kmer_t{"ABB", 5, -1}                
    };
    std::vector<kmer_t> ret_a_padded = {
        kmer_t{"AAA", 42, -1},
        kmer_t{"AAA", 43, -1},
        kmer_t{"AAA", 44, -1},
        kmer_t{"AAA", 45, -1},
        kmer_t{"AAB", 46, -1},
        kmer_t{"ABB", 47, -1}                
    };
    BOOST_TEST(
		syncmerize(a, 3, 2, true, false, 0) == ret_a
    );
    BOOST_TEST(
		syncmerize(a, 3, 2, true, false, 42) == ret_a_padded
    );

    std::string b = "AABCBAAACC"; // k=6 s=3 open
  /*               * AABCBA 
                   *  ABCBAA
                       BCBAAA
                        CBAAAC
                         BAAACC
  */
    std::vector<kmer_t> ret_b = {
        kmer_t{"AABCBA", 0, -1},
        kmer_t{"ABCBAA", 1, -1}
    };
    std::string c = "AABCBAAACC"; // k=6 s=3 closed
  /*               * AABCBA 
                   *  ABCBAA
                   *   BCBAAA
                        CBAAAC
                         BAAACC
  */
    std::vector<kmer_t> ret_c = {
        kmer_t{"AABCBA", 0, -1},
        kmer_t{"ABCBAA", 1, -1},
        kmer_t{"BCBAAA", 2, -1}
    };

    BOOST_TEST(
		syncmerize(b, 6, 3, true, false, 0) == ret_b
    );

    std::cout << "ret c: " << std::endl;
    for (auto kmer : ret_c) {
        std::cout << kmer.seq << " " << kmer.pos << std::endl;
    }
    std::cout << "syncmerize c: " << std::endl;
    for (auto kmer : syncmerize(c, 6, 3, false, false, 0)) {
        std::cout << kmer.seq  << " " << kmer.pos << std::endl;
    }
    BOOST_TEST(
		syncmerize(c, 6, 3, false, false, 0) == ret_c
    );

    std::string d = "--A-A-B--CBAAAC-C-"; // k=6 s=3 closed aligned
  /*               * AABCBA 
                   *  ABCBAA
                   *   BCBAAA
                        CBAAAC
                         BAAACC
  */
     std::vector<kmer_t> ret_d = {
        kmer_t{"AABCBA", 2, -1},
        kmer_t{"ABCBAA", 4, -1},
        kmer_t{"BCBAAA", 6, -1}
    };
    BOOST_TEST(
		syncmerize(d, 6, 3, false, true, 0) == ret_d
    );

}


template<>
struct boost::test_tools::tt_detail::print_log_value<std::pair<int,int>> {
    void operator()(std::ostream& os, std::pair<int,int> const& value) {
        os << '{' << value.first << ',' << value.second << '}';
    }
};

BOOST_AUTO_TEST_CASE(_getRecomputePositions)
{
    int32_t k = 3;

    std::pair<int32_t, int32_t> a = {6, 1};
    std::pair<int32_t, int32_t> b = {22, 3};
    std::pair<int32_t, int32_t> c = {31, 2};

    //                 length:  0               3        2
    //                          *               ***      **  
    //      recompute range: xxxxxxx       xxxxxxxxxx  xxxxx
    std::string gapped = "ABCD-EF-GH-IJKLM-N-O--P-QRSTUVWXYZ";
    //                    0123456789111111111122222222223333
    //                              012345678901234567890123

    std::pair<int32_t, int32_t> expected_a = {3, 9};
    std::pair<int32_t, int32_t> expected_b = {17, 26};
    std::pair<int32_t, int32_t> expected_c = {29, 33};

    BOOST_TEST(
        getRecomputePositions(a, gapped, k) == expected_a
    );
    BOOST_TEST(
        getRecomputePositions(b, gapped, k) == expected_b
    );
    BOOST_TEST(
        getRecomputePositions(c, gapped, k) == expected_c
    );
}

BOOST_AUTO_TEST_CASE(_alignedEndPos) {
 /* should compute a syncmer's end pos with gaps */

    std::string a = "--AAAAA--A-A-A-----A--AAA--";
    //               012345678911111111112222222
    //                         01234567890123456
    //                            ^^^^^^^^^^^

    BOOST_TEST(
      alignedEndPos(13, 4, a) ==  23 
    );
}

BOOST_AUTO_TEST_CASE(_discardSyncmers) {
/*  should discard syncmers contained in any range in B 
    unless they are going to be inserted. Then remove from
    to_insert (based on sequence for now). Also, track variable syncmers.
*/
    size_t k = 4;

    std::vector<kmer_t> seeds = {
        kmer_t{"AAAA", 0, -1}, // x
        kmer_t{"ACCA", 2, -1}, //
        kmer_t{"AABA", 7, -1}, // x
        kmer_t{"AACA", 8, -1}, // x
        kmer_t{"ABDA", 9, -1}, //
        kmer_t{"ABBA", 15,-1}, // gapped length exceeds bound  
        kmer_t{"CDEF", 23,-1}, 
    };
    std::string seq = "xxxxxxxxxxxxxxxABB---Axxxxxx"; // len 28

    std::vector<std::pair<int32_t, int32_t>> B = {
        {0, 4},
        {7, 11},
        {14, 20},
        {22, 35}
    };
    std::vector<kmer_t> expected_seeds = {
        kmer_t{"ACCA", 2, -1},
        kmer_t{"ABDA", 9, -1},
        kmer_t{"ABBA", 15,-1},
        kmer_t{"CDEF", 23,-1}, 
    };

    std::unordered_map<std::string, kmer_t> to_insert = {
        {"XYZW", kmer_t{"XYZW", 8, -1}},
        {"CDEF", kmer_t{"CDEF", 23, -1}}
    };
    std::unordered_map<std::string, kmer_t> expected_to_insert = {
        {"XYZW", kmer_t{"XYZW", 8, -1}}
    };

    seedIndex index;
    
    std::vector<kmer_t> expected_deletions = {
                kmer_t{"AACA", 8, 3},
                kmer_t{"AABA", 7, 2},
                kmer_t{"AAAA", 0, 0},
    };
    std::string nid = "my_node_id";

    std::unordered_map<std::string, bool> variable_syncmers;

    std::unordered_map<std::string, bool> expected_variable_syncmers = {
        {"AAAA", true},
        {"AABA", true},
        {"AACA", true}
    };

    discardSyncmers(seeds, B, seq, to_insert, variable_syncmers, index, nid, k);

    BOOST_TEST(
        seeds == expected_seeds
    );
    BOOST_TEST(
        to_insert == expected_to_insert
    );

    BOOST_TEST(
        index.deletions[nid] == expected_deletions
    );
    BOOST_TEST(index.deletions[nid][0].pos == 8);
    BOOST_TEST(index.deletions[nid][0].idx == 3);
    BOOST_TEST(index.deletions[nid][1].pos == 7);
    BOOST_TEST(index.deletions[nid][1].idx == 2);
    BOOST_TEST(index.deletions[nid][2].pos == 0);
    BOOST_TEST(index.deletions[nid][2].idx == 0);

    BOOST_TEST(
        variable_syncmers == expected_variable_syncmers
    );
}

// BOOST_AUTO_TEST_CASE(_indexSyncmers) {
//     size_t k = 13;
//     size_t s = 4;
//     std::ifstream is("../src/test/test.pmat");
//     boost::iostreams::filtering_streambuf< boost::iostreams::input> inPMATBuffer;
//     inPMATBuffer.push(boost::iostreams::gzip_decompressor());
//     inPMATBuffer.push(is);
//     std::istream inputStream(&inPMATBuffer);
//     Tree *T = new Tree(inputStream);
//     std::ofstream os("./test.out");
//     indexSyncmers(T, os, k, s);
// }

// BOOST_AUTO_TEST_CASE(_indexSyncmers) {
//     size_t k = 13;
//     size_t s = 4;
//     std::ifstream is("../src/test/test.pmat");
//     boost::iostreams::filtering_streambuf< boost::iostreams::input> inPMATBuffer;
//     inPMATBuffer.push(boost::iostreams::gzip_decompressor());
//     inPMATBuffer.push(is);
//     std::istream inputStream(&inPMATBuffer);
//     Tree *T = new Tree(inputStream);
//     std::ofstream os("./test.out");
//     indexSyncmers(T, os, k, s);
// }

// BOOST_AUTO_TEST_CASE(accuracy) {
//     std::vector<std::string> files = {"../fastq/MT509467.1.1kreads.fastq_R1.fastq","../fastq/ON776243.1.1kreads.fastq_R1.fastq","../fastq/MW782840.1.1kreads.fastq_R1.fastq","../fastq/ON837682.1.1kreads.fastq_R1.fastq","../fastq/MW848911.1.1kreads.fastq_R1.fastq","../fastq/ON874535.1.1kreads.fastq_R1.fastq","../fastq/MZ239798.1.1kreads.fastq_R1.fastq","../fastq/ON882659.1.1kreads.fastq_R1.fastq","../fastq/MZ334642.1.1kreads.fastq_R1.fastq","../fastq/ON998249.1.1kreads.fastq_R1.fastq","../fastq/OL709938.1.1kreads.fastq_R1.fastq","../fastq/OP015056.1.1kreads.fastq_R1.fastq","../fastq/OL723459.1.1kreads.fastq_R1.fastq","../fastq/OP052055.1.1kreads.fastq_R1.fastq","../fastq/OL725440.1.1kreads.fastq_R1.fastq","../fastq/OP138076.1.1kreads.fastq_R1.fastq","../fastq/OL750883.1.1kreads.fastq_R1.fastq","../fastq/OP182116.1.1kreads.fastq_R1.fastq","../fastq/OL777915.1.1kreads.fastq_R1.fastq","../fastq/OP192794.1.1kreads.fastq_R1.fastq","../fastq/OL835243.1.1kreads.fastq_R1.fastq","../fastq/OP283063.1.1kreads.fastq_R1.fastq","../fastq/OL970200.1.1kreads.fastq_R1.fastq","../fastq/OP336755.1.1kreads.fastq_R1.fastq","../fastq/OM097226.1.1kreads.fastq_R1.fastq","../fastq/OP397183.1.1kreads.fastq_R1.fastq","../fastq/OM162869.1.1kreads.fastq_R1.fastq","../fastq/OP490141.1.1kreads.fastq_R1.fastq","../fastq/OM214216.1.1kreads.fastq_R1.fastq","../fastq/OP567795.1.1kreads.fastq_R1.fastq","../fastq/OM356599.1.1kreads.fastq_R1.fastq","../fastq/OP570454.1.1kreads.fastq_R1.fastq","../fastq/OM628072.1.1kreads.fastq_R1.fastq","../fastq/OP708482.1.1kreads.fastq_R1.fastq","../fastq/ON072955.1.1kreads.fastq_R1.fastq","../fastq/OP849268.1.1kreads.fastq_R1.fastq","../fastq/ON137843.1.1kreads.fastq_R1.fastq","../fastq/OP880035.1.1kreads.fastq_R1.fastq","../fastq/ON141711.1.1kreads.fastq_R1.fastq","../fastq/OP969895.1.1kreads.fastq_R1.fastq","../fastq/ON174724.1.1kreads.fastq_R1.fastq","../fastq/OQ173993.1.1kreads.fastq_R1.fastq","../fastq/ON177108.1.1kreads.fastq_R1.fastq","../fastq/OQ577811.1.1kreads.fastq_R1.fastq","../fastq/ON230821.1.1kreads.fastq_R1.fastq","../fastq/OQ618526.1.1kreads.fastq_R1.fastq","../fastq/ON439765.1.1kreads.fastq_R1.fastq","../fastq/OQ739976.1.1kreads.fastq_R1.fastq","../fastq/ON499190.1.1kreads.fastq_R1.fastq","../fastq/OQ778157.1.1kreads.fastq_R1.fastq","../fastq/ON589167.1.1kreads.fastq_R1.fastq","../fastq/OQ778358.1.1kreads.fastq_R1.fastq","../fastq/ON655095.1.1kreads.fastq_R1.fastq","../fastq/OR301852.1.1kreads.fastq_R1.fastq","../fastq/ON659980.1.1kreads.fastq_R1.fastq","../fastq/OX653254.1.1kreads.fastq_R1.fastq","../fastq/ON717660.1.1kreads.fastq_R1.fastq","../fastq/OY308619.1.1kreads.fastq_R1.fastq","../fastq/ON772334.1.1kreads.fastq_R1.fastq"};
//     size_t k = 14;
//     size_t s = 7;
//     std::ifstream is("../sars2k.pmat");
//      boost::iostreams::filtering_streambuf< boost::iostreams::input> inPMATBuffer;
//     inPMATBuffer.push(boost::iostreams::gzip_decompressor());
//     inPMATBuffer.push(is);
//     std::istream inputStream(&inPMATBuffer);
//     Tree *T = new Tree(inputStream);
//     std::ofstream os("./test.out");
//     indexSyncmers(T, os, k, s);
    
//     is.close();
//     os.close();
//     PangenomeMAT::Node *root = T->root;
//     struct seedIndex index;
//     std::ifstream indexFile("./test.out");
//     PangenomeMAT::loadIndex(T->root, indexFile, index);
    
//     for (std::string f : files) {

//         std::vector<read_t> reads;
//         auto fastq_start = std::chrono::high_resolution_clock::now();
//         std::set<kmer_t> readSyncmers = syncmersFromFastq(f, reads, k, s);
//         auto fastq_end = std::chrono::high_resolution_clock::now();

//         std::cout << "fastq time: " << std::chrono::duration_cast<std::chrono::milliseconds>(fastq_end - fastq_start).count() << "\n";

//         auto place_start = std::chrono::high_resolution_clock::now();

//         std::set<kmer_t> rootSyncmers = std::set<kmer_t>(index.rootSeeds.begin(), index.rootSeeds.end());

//         std::cerr << "\n";
//         std::cerr << "Placing sample... " << f << "\n";


//         struct dynamicJaccard dj;
    
//         dj.intersectionSize = intersection_size(rootSyncmers, readSyncmers);
//         dj.unionSize = rootSyncmers.size() + readSyncmers.size() - dj.intersectionSize;
//         dj.jaccardIndex = (float)dj.intersectionSize / dj.unionSize;
        
        
//         std::cout << "root seeds: " << rootSyncmers.size() << "\n";
//         std::cout << "read seeds: " << readSyncmers.size() << "\n";
//         for (const auto &k : readSyncmers) {
//             std::cout << k.seq << "\n";
//         }
//         std::cout << "initial jaccard: " << dj.jaccardIndex << "\n";

//         std::unordered_map<std::string, float> scores;
//         std::unordered_map<std::string, bool> readSyncmersMap;
//         for (const auto &k : readSyncmers) {
//             readSyncmersMap[k.seq] = true;
//         }

//         placeDFS(root, index.rootSeeds, readSyncmersMap, index, dj, scores);

//         auto place_end = std::chrono::high_resolution_clock::now();

//         std::cout << "place time: " << std::chrono::duration_cast<std::chrono::milliseconds>(place_end - place_start).count() << "\n";

//         std::vector<std::pair<std::string, float>> v;
//         for ( const auto &p : scores ) {
//             v.push_back(std::make_pair(p.first, p.second));
//         } 
//         std::sort(v.begin(), v.end(), [] (auto &left, auto &right) {
//             return left.second > right.second;
//         });

//         std::string best_match = v[0].first;
//         for (const auto &s : v) {
//             std::cerr << s.first << ": " << s.second << "\n";
//         }

//     }
// }

// BOOST_AUTO_TEST_CASE(_indexSyncmers) {
//     size_t k = 16;
//     size_t s = 5;
//     std::ifstream is("../mammal_mito_refseq.pmat");
// //    std::string fastqPath = "";
//     boost::iostreams::filtering_streambuf< boost::iostreams::input> inPMATBuffer;
//     inPMATBuffer.push(boost::iostreams::gzip_decompressor());
//     inPMATBuffer.push(is);
//     std::istream inputStream(&inPMATBuffer);
//     Tree *T = new Tree(inputStream);
//     std::ofstream os("./test.out");
//     indexSyncmers(T, os, k, s);
    
//     is.close();
//     os.close();

    // PangenomeMAT::Node *root = T->root;
    // std::vector<read_t> reads;

    // struct seedIndex index;
    // std::ifstream indexFile("./test.out");

    // PangenomeMAT::loadIndex(T->root, indexFile, index);

    // auto fastq_start = std::chrono::high_resolution_clock::now();
    // std::set<kmer_t> readSyncmers = syncmersFromFastq(fastqPath, reads, k, s);
    // auto fastq_end = std::chrono::high_resolution_clock::now();

    // std::cout << "fastq time: " << std::chrono::duration_cast<std::chrono::milliseconds>(fastq_end - fastq_start).count() << "\n";

    // auto place_start = std::chrono::high_resolution_clock::now();

    // std::set<kmer_t> rootSyncmers = std::set<kmer_t>(index.rootSeeds.begin(), index.rootSeeds.end());

    // std::cerr << "\n";
    // std::cerr << "Placing sample...\n";


    // struct dynamicJaccard dj;
 
    // dj.intersectionSize = intersection_size(rootSyncmers, readSyncmers);
    // dj.unionSize = rootSyncmers.size() + readSyncmers.size() - dj.intersectionSize;
    // dj.jaccardIndex = (float)dj.intersectionSize / dj.unionSize;
    
    
    // std::cout << "root seeds: " << rootSyncmers.size() << "\n";
    // std::cout << "read seeds: " << readSyncmers.size() << "\n";
    // for (const auto &k : readSyncmers) {
    //     std::cout << k.seq << "\n";
    // }
    // std::cout << "initial jaccard: " << dj.jaccardIndex << "\n";

    // std::unordered_map<std::string, float> scores;
    // std::unordered_map<std::string, bool> readSyncmersMap;
    // for (const auto &k : readSyncmers) {
    //     readSyncmersMap[k.seq] = true;
    // }

    // placeDFS(root, index.rootSeeds, readSyncmersMap, index, dj, scores);

    // auto place_end = std::chrono::high_resolution_clock::now();

    // std::cout << "place time: " << std::chrono::duration_cast<std::chrono::milliseconds>(place_end - place_start).count() << "\n";

    // std::vector<std::pair<std::string, float>> v;
    // for ( const auto &p : scores ) {
    //     v.push_back(std::make_pair(p.first, p.second));
    // } 
    // std::sort(v.begin(), v.end(), [] (auto &left, auto &right) {
    //     return left.second > right.second;
    // });

    // std::string best_match = v[0].first;
    // for (const auto &s : v) {
    //     std::cerr << s.first << ": " << s.second << "\n";
    // }

// }

