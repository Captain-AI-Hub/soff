#pragma once

#include <filesystem>

namespace soff::db {

class AtomicFileWriter
{
public:
    explicit AtomicFileWriter(std::filesystem::path destination, bool seed_from_destination = false);
    ~AtomicFileWriter();

    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

    AtomicFileWriter(AtomicFileWriter&&) = delete;
    AtomicFileWriter& operator=(AtomicFileWriter&&) = delete;

    const std::filesystem::path& destination_path() const noexcept;
    const std::filesystem::path& temporary_path() const noexcept;
    void commit();

private:
    void cleanup() noexcept;

    std::filesystem::path destination_;
    std::filesystem::path temporary_;
    bool committed_ = false;
};

void checkpoint_and_validate_sqlite(const std::filesystem::path& path);

} // namespace soff::db
