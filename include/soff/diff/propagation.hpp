#pragma once

#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"
#include "soff/diff/ratio.hpp"

#include <cstddef>
#include <string>
#include <boost/unordered/unordered_flat_set.hpp>
#include <vector>

namespace soff::diff {

struct PropagationOptions
{
    bool enabled = true;
    bool enable_slow = true;
    int max_iterations = 3;
    double same_name_min_ratio = 0.5;
    double affine_min_ratio = 0.5;
    double diffing_min_ratio = 0.5;
    double related_min_ratio = 0.8;
    int max_functions_per_gap = 40;
    bool same_processor = true;
};

struct PropagationStats
{
    std::size_t same_name_matches = 0;
    std::size_t affine_matches = 0;
    std::size_t diffing_matches = 0;
    std::size_t related_constants_matches = 0;
    int iterations_run = 0;
};

PropagationStats run_propagation(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    const PropagationOptions& options);

std::size_t find_same_name(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio,
    bool same_processor);

std::size_t find_locally_affine_functions(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio,
    int max_gap_size,
    bool same_processor);

std::size_t find_matches_diffing(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio,
    bool same_processor);

std::size_t find_related_constants(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio);

std::size_t find_compilation_unit_matches(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio,
    bool same_processor);

} // namespace soff::diff
