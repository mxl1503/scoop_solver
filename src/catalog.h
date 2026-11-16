#pragma once
// Six-max catalogue: one solved row for every suit-canonical 6-card hand.
#include <cstdint>
#include <string>

namespace scoop {

struct CatalogOptions {
    int deals = -1;          // opponent deals per hand; <=0 -> default
    int boards = -1;         // sampled boards per deal; <=0 -> default
    int threads = 1;
    uint64_t seed = 1;
    int limit = 0;           // stop after this many new hands; 0 = all
    int rmIters = -1;
    double rmTol = -1;
    std::string out = "data/sixmax";
    std::string only;        // if set, print this one hand and do not write the catalogue
};

int runCatalog(const CatalogOptions& opt);

}  // namespace scoop
