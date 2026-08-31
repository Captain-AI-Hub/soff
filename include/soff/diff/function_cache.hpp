#pragma once

#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"

#include <boost/unordered/unordered_flat_map.hpp>

#include <list>
#include <string>
#include <string_view>
#include <vector>

namespace soff::diff {

struct CachedFunction
{
    Address address = 0;
    std::string name;
    std::string mangled_function;
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
    boost::unordered_flat_map<std::string, std::vector<Address>> by_name;
    boost::unordered_flat_map<std::string, std::vector<Address>> by_mangled_name;
};

FunctionCache load_function_cache(db::Database& database, std::string_view schema_name);

// Lazily resolves the large text columns (clean_assembly / clean_pseudo) by
// function address with a small LRU memo. Holding them for every function
// was the dominant memory cost on 100k+ function databases; the passes that
// need them only touch a small subset.
//
// Not thread-safe: the propagation passes that use this run single-threaded.
class FunctionTextResolver
{
public:
    struct Texts
    {
        std::string clean_assembly;
        std::string clean_pseudo;
    };

    FunctionTextResolver(db::Database& database, std::string_view schema_name)
        : database_(&database)
        , prefix_(
              schema_name.empty() || schema_name == "main"
                  ? std::string()
                  : std::string(schema_name) + ".")
    {
    }

    // Returns a reference valid until the next get() that evicts this entry.
    const Texts& get(Address address) const
    {
        if (const auto it = index_.find(address); it != index_.end()) {
            entries_.splice(entries_.begin(), entries_, it->second);
            return it->second->second;
        }
        return resolve(address);
    }

private:
    const Texts& resolve(Address address) const
    {
        Texts texts;
        auto statement = database_->prepare(
            "select coalesce(clean_assembly, ''), coalesce(clean_pseudo, '') from "
            + prefix_ + "functions where address = ? limit 1");
        statement.bind(1, std::to_string(address));
        if (statement.step()) {
            texts.clean_assembly = statement.column_text(0);
            texts.clean_pseudo = statement.column_text(1);
        }

        if (index_.size() >= capacity_) {
            index_.erase(entries_.back().first);
            entries_.pop_back();
        }
        entries_.emplace_front(address, std::move(texts));
        index_.emplace(address, entries_.begin());
        return entries_.front().second;
    }

    db::Database* database_;
    std::string prefix_;
    static constexpr std::size_t capacity_ = 4096;
    mutable std::list<std::pair<Address, Texts>> entries_;
    mutable boost::unordered_flat_map<Address, std::list<std::pair<Address, Texts>>::iterator> index_;
};

} // namespace soff::diff
