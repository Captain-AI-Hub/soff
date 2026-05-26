#pragma once

#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"

#include <string>
#include <vector>

namespace soff::diff {

struct BasicBlockMatch
{
    Address primary_start = 0;
    Address secondary_start = 0;
    double confidence = 0.0;
    std::string method;
};

struct BasicBlockMatchResult
{
    std::vector<BasicBlockMatch> matches;
    std::size_t primary_blocks = 0;
    std::size_t secondary_blocks = 0;

    double similarity() const;
};

struct BasicBlockInfo
{
    Address start = 0;
    Address end = 0;
    std::size_t instruction_count = 0;
    std::string prime_product;
    std::string mnemonics;
    std::vector<Address> successors;
    std::size_t in_degree = 0;
    std::size_t out_degree = 0;
    bool is_entry = false;
    bool is_exit = false;
};

BasicBlockMatchResult match_basic_blocks(
    db::Database& database,
    Address primary_function,
    Address secondary_function);

double structural_similarity(const BasicBlockMatchResult& bb_result);

} // namespace soff::diff
