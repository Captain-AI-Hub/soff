#pragma once

#include <string_view>

namespace soff {

enum class PluginAction
{
    export_snapshot,
    import_snapshot,
    diff_snapshot,
    show_results,
};

std::string_view action_name(PluginAction action);

} // namespace soff
