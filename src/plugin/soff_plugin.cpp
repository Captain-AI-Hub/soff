#include <fstream>
#include <sstream>

#include "soff/core/version.hpp"
#include "soff/core/error.hpp"
#include "soff/core/hooks.hpp"
#include "soff/core/perf.hpp"
#include "soff/db/atomic_writer.hpp"
#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"
#include "soff/db/repository.hpp"
#include "soff/diff/session.hpp"
#include "soff/ui/html_diff.hpp"
#include "soff/ui/import_plan.hpp"
#include "soff/ui/line_diff.hpp"

#include <boost/uuid/detail/md5.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdio>
#endif

#include <auto.hpp>
#include <bytes.hpp>
#include <funcs.hpp>
#include <gdl.hpp>
#include <graph.hpp>
#include <hexrays.hpp>
#include <ida.hpp>
#include <idp.hpp>
#include <kernwin.hpp>
#include <loader.hpp>
#include <nalt.hpp>
#include <name.hpp>
#include <typeinf.hpp>
#include <ua.hpp>
#include <lines.hpp>
#include <xref.hpp>
#include <expr.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


namespace {

// The plugin implementation is organized as compile-time modules. Keeping the
// shared anonymous namespace preserves the original IDA callback ABI while the
// focused files below keep export, Hex-Rays/microcode, import, result UI,
// graph UI, and action wiring independently navigable.
#include "modules/common.inc"
#include "modules/ida_compat.inc"
#include "modules/settings.inc"
#include "modules/export_helpers.inc"
#include "modules/hexrays.inc"
#include "modules/microcode.inc"
#include "modules/export.inc"
#include "modules/result_ui.inc"
#include "modules/graph_ui.inc"
#include "modules/result_ui_actions.inc"
#include "modules/import.inc"
#include "modules/actions.inc"
#include "modules/entry.inc"

} // namespace

plugin_t PLUGIN =
{
    IDP_INTERFACE_VERSION,
    PLUGIN_MULTI | PLUGIN_HIDE,
    init,
    nullptr,
    nullptr,
    "soff binary diffing rewrite skeleton",
    nullptr,
    "Soff",
    nullptr,
};
