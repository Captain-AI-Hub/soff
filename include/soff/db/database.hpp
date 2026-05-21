#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3_stmt;

namespace soff::db {

class Database;

class QueryRow
{
public:
    QueryRow() = default;
    explicit QueryRow(std::vector<std::string> values);

    bool empty() const noexcept;
    std::size_t size() const noexcept;
    const std::string& operator[](std::size_t index) const;
    std::vector<std::string>::const_iterator begin() const noexcept;
    std::vector<std::string>::const_iterator end() const noexcept;

private:
    std::vector<std::string> values_;
};

class Statement
{
public:
    Statement() = default;
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    void bind(int index, std::string_view value);
    bool step();
    void reset();
    int column_count() const;
    std::string column_text(int index) const;
    std::int64_t column_int64(int index) const;

private:
    friend class Database;

    Statement(Database* database, sqlite3_stmt* statement);
    void finalize() noexcept;

    Database* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

class Database
{
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    void open(const std::filesystem::path& path);
    void close() noexcept;

    bool is_open() const noexcept;
    void execute(std::string_view sql);
    void execute(std::string_view sql, const std::vector<std::string>& values);

    Statement prepare(std::string_view sql);
    std::string query_text(std::string_view sql);
    std::int64_t query_int(std::string_view sql);
    std::vector<QueryRow> query_rows(std::string_view sql);

    void attach(const std::filesystem::path& path, std::string_view schema_name);
    void set_progress_handler(int instruction_interval, std::function<bool()> should_interrupt);
    void clear_progress_handler() noexcept;
    void apply_performance_pragmas();

private:
    friend class Statement;

    struct Impl;
    Impl* impl_ = nullptr;
};

class Transaction
{
public:
    explicit Transaction(Database& database, std::string_view begin_sql = "begin immediate");
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit();
    void rollback() noexcept;

private:
    Database* database_ = nullptr;
    bool active_ = false;
};

} // namespace soff::db
