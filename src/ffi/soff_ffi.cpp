#include "soff_ffi.h"

#include "soff/core/hooks.hpp"
#include "soff/core/version.hpp"
#include "soff/diff/heuristics.hpp"
#include "soff/diff/session.hpp"

#include <cstring>
#include <exception>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

namespace {

struct FfiProgressHooks : soff::DiffHooks
{
    soff_progress_fn callback = nullptr;
    void* userdata = nullptr;
    std::size_t index = 0;
    std::size_t total = 0;
    std::size_t matches = 0;

    void emit(const std::string& json) const
    {
        if (callback) callback(json.c_str(), userdata);
    }

    std::optional<std::string> on_launch_heuristic(
        std::string_view name, std::string_view sql) override
    {
        ++index;
        std::ostringstream out;
        out << "{\"phase\":\"heuristic\",\"index\":" << index
            << ",\"total\":" << total
            << ",\"matches\":" << matches
            << ",\"name\":\"" << name << "\"}";
        emit(out.str());
        return std::string(sql);
    }

    soff::MatchDecision on_match(const soff::MatchContext& ctx) override
    {
        ++matches;
        return {true, ctx.ratio};
    }
};

} // namespace

extern "C" {

SOFF_API int soff_diff_run(
    const char* primary_db,
    const char* secondary_db,
    const char* output_path,
    const soff_diff_options* options,
    soff_progress_fn progress_cb,
    void* userdata,
    char* error_buf,
    int error_buf_size)
{
    try {
        FfiProgressHooks hooks;
        hooks.callback = progress_cb;
        hooks.userdata = userdata;
        hooks.total = soff::diff::builtin_heuristics().size();

        if (progress_cb) {
            progress_cb("{\"phase\":\"validate\",\"step\":\"primary\"}", userdata);
        }

        soff::diff::DiffSessionOptions diff_options;
        if (options) {
            diff_options.sql.enable_slow = options->enable_slow != 0;
            diff_options.sql.enable_unreliable = options->enable_unreliable != 0;
            diff_options.sql.enable_experimental = options->enable_experimental != 0;
            if (options->max_rows > 0)
                diff_options.sql.max_processed_rows = options->max_rows;
            if (options->timeout_seconds > 0)
                diff_options.sql.timeout_seconds = options->timeout_seconds;
        }
        diff_options.hooks = &hooks;

        if (progress_cb) {
            progress_cb("{\"phase\":\"validate\",\"step\":\"secondary\"}", userdata);
        }

        if (progress_cb) {
            progress_cb("{\"phase\":\"running\"}", userdata);
        }

        const auto summary = soff::diff::DiffSession{diff_options}.run_all(
            primary_db, secondary_db, output_path);

        if (progress_cb) {
            std::ostringstream done;
            done << "{\"phase\":\"done\""
                 << ",\"best\":" << summary.results.best
                 << ",\"partial\":" << summary.results.partial
                 << ",\"unreliable\":" << summary.results.unreliable
                 << ",\"unmatched_primary\":" << summary.results.unmatched_primary
                 << ",\"unmatched_secondary\":" << summary.results.unmatched_secondary
                 << "}";
            progress_cb(done.str().c_str(), userdata);
        }
        return 0;
    } catch (const std::exception& e) {
        if (error_buf && error_buf_size > 0) {
            std::strncpy(error_buf, e.what(), static_cast<std::size_t>(error_buf_size - 1));
            error_buf[error_buf_size - 1] = '\0';
        }
        return 1;
    } catch (...) {
        if (error_buf && error_buf_size > 0) {
            std::strncpy(error_buf, "unknown error", static_cast<std::size_t>(error_buf_size - 1));
            error_buf[error_buf_size - 1] = '\0';
        }
        return 2;
    }
}

SOFF_API const char* soff_version(void)
{
    static std::string version_str{soff::version()};
    return version_str.c_str();
}

} // extern "C"
