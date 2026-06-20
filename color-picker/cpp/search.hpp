// uvy scan + CIE76 best-match search. Mirrors main.py's _search / _do_search.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "swirl.hpp"

struct Result {
    std::string target_hex;  // "#rrggbb"
    std::string hex;         // matched color "#rrggbb"
    double sim;              // similarity, rounded to 4 decimals (as in r dict)
    double uvy;              // uvy, rounded to 4 decimals
    int slot;                // 1-based best slot
    RGBA colors[5];          // the 5 dye colors at the best uvy
};

// target_hex_stripped: hex without leading '#'.
// Returns false if the texture can't be loaded.
bool search(const std::string& target_hex_stripped, uint32_t picture_id,
            const std::vector<double>& params, const std::string& texdir,
            Result& out);
