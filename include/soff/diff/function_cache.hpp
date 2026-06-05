#pragma once

#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"

#include <boost/unordered/unordered_flat_map.hpp>

#include <string>
#include <string_view>

namespace soff::diff {

struct CachedFunction
{
    Address address = 0;
    std::string name;
    std::string clean_assembly;
    std::string clean_pseudo;
    std::string pseudocode_primes;
    std::string bytes_hash;
    std::string md_index;
    std::string constants;
    std::string source_file;
    int nodes = 0;
    int edges = 0;
    int instructions = 0;
    int size = 0;
    int constants_count = 0;
};

struct FunctionCache
{
    boost::unordered_flat_map<Address, CachedFunction> by_address;
    boost::unordered_flat_map<std::string, Address> by_name;
};

FunctionCache load_function_cache(db::Database& database, std::string_view schema_name);

} // namespace soff::diff
