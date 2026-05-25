#pragma once

#ifdef _WIN32
#define SOFF_API __declspec(dllexport)
#else
#define SOFF_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*soff_progress_fn)(const char* json_line, void* userdata);

typedef struct {
    int enable_slow;
    int enable_unreliable;
    int enable_experimental;
    unsigned int max_rows;
    unsigned int timeout_seconds;
} soff_diff_options;

/// Run diff between two exported SQLite databases.
/// Returns 0 on success, non-zero on error.
/// On error, error_buf is filled with the message (up to error_buf_size).
SOFF_API int soff_diff_run(
    const char* primary_db,
    const char* secondary_db,
    const char* output_path,
    const soff_diff_options* options,
    soff_progress_fn progress_cb,
    void* userdata,
    char* error_buf,
    int error_buf_size
);

/// Get version string.
SOFF_API const char* soff_version(void);

#ifdef __cplusplus
}
#endif
