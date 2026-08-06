#include "soff_ffi.h"

#include "soff/core/hooks.hpp"
#include "soff/core/version.hpp"
#include "soff/diff/heuristics.hpp"
#include "soff/diff/session.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void write_error(char* buffer, int buffer_size, std::string_view message)
{
    if (buffer == nullptr || buffer_size <= 0) {
        return;
    }
    const auto capacity = static_cast<std::size_t>(buffer_size);
    const auto copy_size = std::min(message.size(), capacity - 1);
    std::memcpy(buffer, message.data(), copy_size);
    buffer[copy_size] = '\0';
}

std::string json_escape(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(hex[(ch >> 4) & 0x0f]);
                escaped.push_back(hex[ch & 0x0f]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return escaped;
}

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
            << ",\"name\":\"" << json_escape(name) << "\"}";
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
    if (primary_db == nullptr || *primary_db == '\0') {
        write_error(error_buf, error_buf_size, "primary database path is empty");
        return 3;
    }
    if (secondary_db == nullptr || *secondary_db == '\0') {
        write_error(error_buf, error_buf_size, "secondary database path is empty");
        return 3;
    }
    if (output_path == nullptr || *output_path == '\0') {
        write_error(error_buf, error_buf_size, "output database path is empty");
        return 3;
    }

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
        write_error(error_buf, error_buf_size, e.what());
        return 1;
    } catch (...) {
        write_error(error_buf, error_buf_size, "unknown error");
        return 2;
    }
}

SOFF_API unsigned int soff_api_version(void)
{
    return 1U;
}

SOFF_API const char* soff_version(void)
{
    static std::string version_str{soff::version()};
    return version_str.c_str();
}

} // extern "C"
