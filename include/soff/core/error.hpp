#pragma once

#include <stdexcept>
#include <string>

namespace soff {

enum class ErrorCode {
    database_invalid,
    export_failed,
    diff_failed,
    function_not_found,
    decompiler_unavailable,
    viewer_failed,
};

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace soff
