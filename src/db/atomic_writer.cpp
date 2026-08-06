#include "soff/db/atomic_writer.hpp"

#include "soff/db/database.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace soff::db {
namespace {

std::atomic<unsigned long long> temporary_counter{0};

std::filesystem::path make_temporary_path(const std::filesystem::path& destination)
{
    const auto parent = destination.has_parent_path()
        ? destination.parent_path()
        : std::filesystem::path(".");
    const auto base = destination.filename().string();
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
        const auto sequence = temporary_counter.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = parent / (
            base + ".tmp-" + std::to_string(stamp) + "-" + std::to_string(sequence));
        if (!std::filesystem::exists(candidate)
            && !std::filesystem::exists(candidate.string() + "-wal")
            && !std::filesystem::exists(candidate.string() + "-shm")) {
            return candidate;
        }
    }
    throw std::runtime_error("failed to allocate a temporary output path for: " + destination.string());
}

void remove_path(const std::filesystem::path& path) noexcept
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

void restore_sidecar(
    const std::filesystem::path& backup,
    const std::filesystem::path& destination) noexcept
{
    if (!std::filesystem::exists(backup)) {
        return;
    }
    std::error_code error;
    std::filesystem::rename(backup, destination, error);
}

} // namespace

AtomicFileWriter::AtomicFileWriter(
    std::filesystem::path destination,
    bool seed_from_destination)
    : destination_(std::move(destination))
    , temporary_(make_temporary_path(destination_))
{
    if (seed_from_destination && std::filesystem::exists(destination_)) {
        checkpoint_and_validate_sqlite(destination_);
        std::filesystem::copy_file(
            destination_, temporary_, std::filesystem::copy_options::overwrite_existing);
    } else {
        std::ofstream file(temporary_, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("failed to create temporary output file: " + temporary_.string());
        }
    }
}

AtomicFileWriter::~AtomicFileWriter()
{
    cleanup();
}

const std::filesystem::path& AtomicFileWriter::destination_path() const noexcept
{
    return destination_;
}

const std::filesystem::path& AtomicFileWriter::temporary_path() const noexcept
{
    return temporary_;
}

void AtomicFileWriter::commit()
{
    if (committed_) {
        return;
    }
    if (!std::filesystem::exists(temporary_)) {
        throw std::runtime_error("temporary output file does not exist: " + temporary_.string());
    }

    const auto destination_wal = std::filesystem::path(destination_.string() + "-wal");
    const auto destination_shm = std::filesystem::path(destination_.string() + "-shm");
    const auto wal_backup = std::filesystem::path(temporary_.string() + ".old-wal");
    const auto shm_backup = std::filesystem::path(temporary_.string() + ".old-shm");

    std::error_code sidecar_error;
    if (std::filesystem::exists(destination_wal)) {
        std::filesystem::rename(destination_wal, wal_backup, sidecar_error);
        if (sidecar_error) {
            throw std::runtime_error(
                "failed to preserve existing SQLite WAL before replacement: "
                + sidecar_error.message());
        }
    }
    if (std::filesystem::exists(destination_shm)) {
        std::filesystem::rename(destination_shm, shm_backup, sidecar_error);
        if (sidecar_error) {
            restore_sidecar(wal_backup, destination_wal);
            throw std::runtime_error(
                "failed to preserve existing SQLite SHM before replacement: "
                + sidecar_error.message());
        }
    }

    try {
#if defined(_WIN32)
        if (!MoveFileExW(
                temporary_.c_str(),
                destination_.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            throw std::runtime_error(
                "failed to atomically replace output file (Windows error "
                + std::to_string(error) + "): " + destination_.string());
        }
#else
        std::error_code error;
        std::filesystem::rename(temporary_, destination_, error);
        if (error) {
            throw std::runtime_error(
                "failed to atomically replace output file: " + destination_.string()
                + ": " + error.message());
        }
#endif
    } catch (...) {
        restore_sidecar(wal_backup, destination_wal);
        restore_sidecar(shm_backup, destination_shm);
        throw;
    }

    committed_ = true;
    remove_path(wal_backup);
    remove_path(shm_backup);
    remove_path(temporary_.string() + "-wal");
    remove_path(temporary_.string() + "-shm");
}

void AtomicFileWriter::cleanup() noexcept
{
    if (committed_) {
        return;
    }
    remove_path(temporary_);
    remove_path(temporary_.string() + "-wal");
    remove_path(temporary_.string() + "-shm");
}

void checkpoint_and_validate_sqlite(const std::filesystem::path& path)
{
    Database database;
    database.open(path);
    try {
        database.execute("pragma wal_checkpoint(truncate)");
        (void)database.query_text("pragma journal_mode=delete");
    } catch (...) {
        // Non-WAL databases or SQLite builds without WAL support still need integrity validation.
    }
    const auto integrity = database.query_text("pragma integrity_check");
    if (integrity != "ok") {
        throw std::runtime_error(
            "SQLite integrity check failed for " + path.string() + ": " + integrity);
    }
}

} // namespace soff::db
