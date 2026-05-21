#include "soff/ui/actions.hpp"

namespace soff {

std::string_view action_name(PluginAction action)
{
    switch (action) {
    case PluginAction::export_snapshot:
        return "Export snapshot";
    case PluginAction::import_snapshot:
        return "Import snapshot";
    case PluginAction::diff_snapshot:
        return "Diff snapshot";
    case PluginAction::show_results:
        return "Show results";
    }
    return "Unknown";
}

} // namespace soff
