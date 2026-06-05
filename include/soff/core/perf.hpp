#pragma once

#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace soff::perf {

namespace detail {

struct Metrics {
    std::unordered_map<std::string, std::uint64_t> counters;
    std::unordered_map<std::string, std::uint64_t> durations_ns;
};

inline std::mutex metrics_mutex;
inline Metrics metrics;

inline std::uint64_t now_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

inline bool compute_enabled()
{
    const char* value = std::getenv("SOFF_PERF");
    return value != nullptr && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}

inline const bool enabled_value = compute_enabled();

} // namespace detail

inline bool enabled()
{
    return detail::enabled_value;
}

void reset();

inline void add_counter(std::string_view name, std::uint64_t value = 1)
{
    if (!enabled()) {
        return;
    }

    std::lock_guard lock(detail::metrics_mutex);
    detail::metrics.counters[std::string(name)] += value;
}

inline void add_duration_ns(std::string_view name, std::uint64_t nanoseconds)
{
    if (!enabled()) {
        return;
    }

    std::lock_guard lock(detail::metrics_mutex);
    detail::metrics.durations_ns[std::string(name)] += nanoseconds;
}

std::string render_report();

class ScopedTimer {
public:
    explicit ScopedTimer(std::string_view name);
    ~ScopedTimer() noexcept;

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::string name_;
    std::uint64_t start_ns_ = 0;
    bool active_ = false;
};

inline ScopedTimer::ScopedTimer(std::string_view name)
{
    if (!enabled()) {
        return;
    }

    name_ = std::string(name);
    start_ns_ = detail::now_ns();
    active_ = true;
}

inline ScopedTimer::~ScopedTimer() noexcept
{
    try {
        if (!active_) {
            return;
        }

        add_duration_ns(name_, detail::now_ns() - start_ns_);
    } catch (...) {
    }
}

} // namespace soff::perf
