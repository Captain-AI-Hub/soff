#pragma once

#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"

#include <filesystem>
#include <string_view>

namespace soff {

enum class SnapshotVersionPolicy
{
    soff,
    diaphora_34,
};

class SnapshotRepository
{
public:
    static constexpr const char* soff_version_value = "soff-0.1.0";
    static constexpr const char* diaphora_version_value = "3.4";

    explicit SnapshotRepository(SnapshotVersionPolicy version_policy = SnapshotVersionPolicy::soff);

    void create_schema(const std::filesystem::path& path) const;
    void create_indices(const std::filesystem::path& path) const;
    void attach_diff(
        db::Database& database,
        const std::filesystem::path& diff_path,
        std::string_view schema_name = "diff") const;

    bool save(const ProgramSnapshot& snapshot, const std::filesystem::path& path) const;
    void begin_incremental_save(const ProgramSnapshot& snapshot, const std::filesystem::path& path, bool replace) const;
    void append_functions(const std::vector<FunctionFeature>& functions, const std::filesystem::path& path) const;
    void replace_program_data(const std::vector<ProgramDataItem>& program_data, const std::filesystem::path& path) const;
    void update_callgraph_primes(const std::string& primes, const std::string& all_primes, const std::filesystem::path& path) const;
    void finalize_incremental_save(const std::filesystem::path& path) const;
    void save_compilation_units(const std::filesystem::path& path) const;
    ProgramSnapshot load(const std::filesystem::path& path) const;
    std::string_view export_version() const noexcept;

private:
    SnapshotVersionPolicy version_policy_ = SnapshotVersionPolicy::soff;
};

} // namespace soff
