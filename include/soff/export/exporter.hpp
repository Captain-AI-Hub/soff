#pragma once

#include "soff/analysis/model.hpp"

#include <string>

namespace soff {

class Exporter
{
public:
    std::string describe(const ProgramSnapshot& snapshot) const;
};

} // namespace soff
