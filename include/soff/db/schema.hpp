#pragma once

#include <string_view>
#include <vector>

namespace soff::db {

struct IndexDefinition
{
    std::string_view table;
    std::string_view fields;
};

struct SchemaDefinition
{
    std::vector<std::string_view> tables;
    std::vector<IndexDefinition> indexes;
};

const SchemaDefinition& diaphora_compatible_schema();

} // namespace soff::db
