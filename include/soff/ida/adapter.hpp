#pragma once

#include "soff/analysis/model.hpp"

#include <string_view>

namespace soff {

class IdaDatabaseReader
{
public:
    virtual ~IdaDatabaseReader() = default;
    virtual ProgramSnapshot build_snapshot() const = 0;
};

std::string_view ida_adapter_name();

} // namespace soff
