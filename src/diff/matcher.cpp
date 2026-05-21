#include "soff/diff/matcher.hpp"

namespace soff {

std::vector<FunctionMatch> DiffMatcher::match(
    const ProgramSnapshot& primary,
    const ProgramSnapshot& secondary) const
{
    (void)primary;
    (void)secondary;
    return {};
}

} // namespace soff
