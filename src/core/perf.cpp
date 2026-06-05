#include "soff/core/perf.hpp"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <vector>

namespace soff::perf {

void reset()
{
    if (!enabled()) {
        return;
    }

    std::lock_guard lock(detail::metrics_mutex);
    detail::metrics.counters.clear();
    detail::metrics.durations_ns.clear();
}

std::string render_report()
{
    if (!enabled()) {
        return {};
    }

    std::lock_guard lock(detail::metrics_mutex);
    std::ostringstream report;
    std::vector<std::string> counter_names;
    counter_names.reserve(detail::metrics.counters.size());
    for (const auto& [name, value] : detail::metrics.counters) {
        counter_names.push_back(name);
    }
    std::sort(counter_names.begin(), counter_names.end());
    for (const auto& name : counter_names) {
        const auto value = detail::metrics.counters.at(name);
        report << "counter " << name << '=' << value << '\n';
    }

    std::vector<std::string> duration_names;
    duration_names.reserve(detail::metrics.durations_ns.size());
    for (const auto& [name, value] : detail::metrics.durations_ns) {
        duration_names.push_back(name);
    }
    std::sort(duration_names.begin(), duration_names.end());
    for (const auto& name : duration_names) {
        const auto value = detail::metrics.durations_ns.at(name);
        report << "duration_ns " << name << '=' << value << '\n';
    }
    return report.str();
}

} // namespace soff::perf
