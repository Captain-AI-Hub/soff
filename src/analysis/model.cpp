#include "soff/analysis/model.hpp"

namespace soff {

bool is_valid_snapshot(const ProgramSnapshot& snapshot)
{
    for (const auto& function : snapshot.functions) {
        if (function.address == 0 || function.name.empty()) {
            return false;
        }
        for (const auto& block : function.blocks) {
            if (block.start == 0 || block.end < block.start) {
                return false;
            }
        }
    }
    return true;
}

} // namespace soff
