#include "soff/db/database.hpp"

#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct sqlite3;
namespace soff::db {
namespace {

constexpr int sqlite_ok = 0;
constexpr int sqlite_row = 100;
constexpr int sqlite_done = 101;
constexpr int sqlite_open_readwrite = 0x00000002;
constexpr int sqlite_open_create = 0x00000004;

using sqlite3_open_v2_t = int (*)(const char*, sqlite3**, int, const char*);
using sqlite3_close_t = int (*)(sqlite3*);
using sqlite3_exec_t = int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
using sqlite3_free_t = void (*)(void*);
using sqlite3_errmsg_t = const char* (*)(sqlite3*);
using sqlite3_prepare_v2_t = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using sqlite3_step_t = int (*)(sqlite3_stmt*);
using sqlite3_finalize_t = int (*)(sqlite3_stmt*);
using sqlite3_reset_t = int (*)(sqlite3_stmt*);
using sqlite3_clear_bindings_t = int (*)(sqlite3_stmt*);
using sqlite3_bind_text_t = int (*)(sqlite3_stmt*, int, const char*, int, void (*)(void*));
using sqlite3_column_text_t = const unsigned char* (*)(sqlite3_stmt*, int);
using sqlite3_column_int64_t = long long (*)(sqlite3_stmt*, int);
using sqlite3_column_count_t = int (*)(sqlite3_stmt*);
using sqlite3_progress_handler_t = void (*)(sqlite3*, int, int (*)(void*), void*);

struct SqliteApi
{
    sqlite3_open_v2_t open_v2 = nullptr;
    sqlite3_close_t close = nullptr;
    sqlite3_exec_t exec = nullptr;
    sqlite3_free_t free = nullptr;
    sqlite3_errmsg_t errmsg = nullptr;
    sqlite3_prepare_v2_t prepare_v2 = nullptr;
    sqlite3_step_t step = nullptr;
    sqlite3_finalize_t finalize = nullptr;
    sqlite3_reset_t reset = nullptr;
    sqlite3_clear_bindings_t clear_bindings = nullptr;
    sqlite3_bind_text_t bind_text = nullptr;
    sqlite3_column_text_t column_text = nullptr;
    sqlite3_column_int64_t column_int64 = nullptr;
    sqlite3_column_count_t column_count = nullptr;
    sqlite3_progress_handler_t progress_handler = nullptr;
};

class SqliteLibrary
{
public:
    SqliteLibrary()
    {
#if defined(_WIN32)
        if (const char* configured = std::getenv("SOFF_SQLITE_DLL")) {
            handle_ = LoadLibraryA(configured);
        }
        const char* names[] = {
            "sqlite3.dll",
            "libsqlite3-0.dll",
            "E:\\msys2\\clang64\\bin\\libsqlite3-0.dll",
        };
        if (handle_ == nullptr) {
            for (const char* name : names) {
                handle_ = LoadLibraryA(name);
                if (handle_ != nullptr) {
                    break;
                }
            }
        }
#else
        if (const char* configured = std::getenv("SOFF_SQLITE_DLL")) {
            handle_ = dlopen(configured, RTLD_NOW | RTLD_LOCAL);
        }
        const char* names[] = {"libsqlite3.so", "libsqlite3.so.0", "libsqlite3.dylib"};
        if (handle_ == nullptr) {
            for (const char* name : names) {
                handle_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
                if (handle_ != nullptr) {
                    break;
                }
            }
        }
#endif
        if (handle_ == nullptr) {
            throw std::runtime_error("SQLite runtime library was not found");
        }

        load("sqlite3_open_v2", api_.open_v2);
        load("sqlite3_close", api_.close);
        load("sqlite3_exec", api_.exec);
        load("sqlite3_free", api_.free);
        load("sqlite3_errmsg", api_.errmsg);
        load("sqlite3_prepare_v2", api_.prepare_v2);
        load("sqlite3_step", api_.step);
        load("sqlite3_finalize", api_.finalize);
        load("sqlite3_reset", api_.reset);
        load("sqlite3_clear_bindings", api_.clear_bindings);
        load("sqlite3_bind_text", api_.bind_text);
        load("sqlite3_column_text", api_.column_text);
        load("sqlite3_column_int64", api_.column_int64);
        load("sqlite3_column_count", api_.column_count);
        load("sqlite3_progress_handler", api_.progress_handler);
    }

    ~SqliteLibrary()
    {
#if defined(_WIN32)
        if (handle_ != nullptr) {
            FreeLibrary(static_cast<HMODULE>(handle_));
        }
#else
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
#endif
    }

    const SqliteApi& api() const noexcept
    {
        return api_;
    }

private:
    template <typename Function>
    void load(const char* name, Function& out)
    {
#if defined(_WIN32)
        out = reinterpret_cast<Function>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
        out = reinterpret_cast<Function>(dlsym(handle_, name));
#endif
        if (out == nullptr) {
            throw std::runtime_error(std::string("SQLite symbol was not found: ") + name);
        }
    }

    void* handle_ = nullptr;
    SqliteApi api_;
};

SqliteLibrary& sqlite_library()
{
    static SqliteLibrary library;
    return library;
}

std::string path_string(const std::filesystem::path& path)
{
    return path.u8string();
}

std::string db_error(const SqliteApi& api, sqlite3* db, std::string_view prefix)
{
    std::string message(prefix);
    message += ": ";
    message += db != nullptr ? api.errmsg(db) : "no database handle";
    return message;
}

int sqlite_progress_callback(void* user_data)
{
    auto* callback = static_cast<std::function<bool()>*>(user_data);
    return callback != nullptr && *callback && (*callback)() ? 1 : 0;
}

} // namespace

struct Database::Impl
{
    const SqliteApi* api = nullptr;
    sqlite3* db = nullptr;
    std::function<bool()> progress_callback;
};

QueryRow::QueryRow(std::vector<std::string> values)
    : values_(std::move(values))
{
}

bool QueryRow::empty() const noexcept
{
    return values_.empty();
}

std::size_t QueryRow::size() const noexcept
{
    return values_.size();
}

const std::string& QueryRow::operator[](std::size_t index) const
{
    return values_[index];
}

std::vector<std::string>::const_iterator QueryRow::begin() const noexcept
{
    return values_.begin();
}

std::vector<std::string>::const_iterator QueryRow::end() const noexcept
{
    return values_.end();
}

Statement::Statement(Database* database, sqlite3_stmt* statement)
    : database_(database)
    , statement_(statement)
{
}

Statement::~Statement()
{
    finalize();
}

Statement::Statement(Statement&& other) noexcept
    : database_(std::exchange(other.database_, nullptr))
    , statement_(std::exchange(other.statement_, nullptr))
{
}

Statement& Statement::operator=(Statement&& other) noexcept
{
    if (this != &other) {
        finalize();
        database_ = std::exchange(other.database_, nullptr);
        statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
}

void Statement::bind(int index, std::string_view value)
{
    if (database_ == nullptr || statement_ == nullptr) {
        throw std::runtime_error("cannot bind a finalized SQLite statement");
    }
    const std::string text(value);
    const int rc = database_->impl_->api->bind_text(
        statement_,
        index,
        text.c_str(),
        -1,
        reinterpret_cast<void (*)(void*)>(-1));
    if (rc != sqlite_ok) {
        throw std::runtime_error(db_error(*database_->impl_->api, database_->impl_->db, "SQLite bind failed"));
    }
}

bool Statement::step()
{
    if (database_ == nullptr || statement_ == nullptr) {
        throw std::runtime_error("cannot step a finalized SQLite statement");
    }
    const int rc = database_->impl_->api->step(statement_);
    if (rc == sqlite_row) {
        return true;
    }
    if (rc == sqlite_done) {
        return false;
    }
    throw std::runtime_error(db_error(*database_->impl_->api, database_->impl_->db, "SQLite step failed"));
}

void Statement::reset()
{
    if (database_ == nullptr || statement_ == nullptr) {
        throw std::runtime_error("cannot reset a finalized SQLite statement");
    }
    int rc = database_->impl_->api->reset(statement_);
    if (rc != sqlite_ok) {
        throw std::runtime_error(db_error(*database_->impl_->api, database_->impl_->db, "SQLite reset failed"));
    }
    rc = database_->impl_->api->clear_bindings(statement_);
    if (rc != sqlite_ok) {
        throw std::runtime_error(db_error(*database_->impl_->api, database_->impl_->db, "SQLite clear bindings failed"));
    }
}

int Statement::column_count() const
{
    if (database_ == nullptr || statement_ == nullptr) {
        throw std::runtime_error("cannot inspect a finalized SQLite statement");
    }
    return database_->impl_->api->column_count(statement_);
}

std::string Statement::column_text(int index) const
{
    if (database_ == nullptr || statement_ == nullptr) {
        throw std::runtime_error("cannot read a finalized SQLite statement");
    }
    const unsigned char* text = database_->impl_->api->column_text(statement_, index);
    return text != nullptr ? reinterpret_cast<const char*>(text) : "";
}

std::int64_t Statement::column_int64(int index) const
{
    if (database_ == nullptr || statement_ == nullptr) {
        throw std::runtime_error("cannot read a finalized SQLite statement");
    }
    return static_cast<std::int64_t>(database_->impl_->api->column_int64(statement_, index));
}

void Statement::finalize() noexcept
{
    if (database_ != nullptr && statement_ != nullptr) {
        database_->impl_->api->finalize(statement_);
    }
    database_ = nullptr;
    statement_ = nullptr;
}

Database::Database()
    : impl_(new Impl{&sqlite_library().api(), nullptr})
{
}

Database::~Database()
{
    close();
    delete impl_;
}

Database::Database(Database&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr))
{
}

Database& Database::operator=(Database&& other) noexcept
{
    if (this != &other) {
        close();
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

void Database::open(const std::filesystem::path& path)
{
    close();
    const auto filename = path_string(path);
    sqlite3* db = nullptr;
    const int rc = impl_->api->open_v2(
        filename.c_str(),
        &db,
        sqlite_open_readwrite | sqlite_open_create,
        nullptr);
    if (rc != sqlite_ok) {
        std::string message = db_error(*impl_->api, db, "failed to open SQLite database");
        if (db != nullptr) {
            impl_->api->close(db);
        }
        throw std::runtime_error(message);
    }
    impl_->db = db;
}

void Database::apply_performance_pragmas()
{
    execute("pragma journal_mode = WAL");
    execute("pragma synchronous = 1");
}

void Database::close() noexcept
{
    if (impl_ != nullptr && impl_->db != nullptr) {
        clear_progress_handler();
        impl_->api->close(impl_->db);
        impl_->db = nullptr;
    }
}

bool Database::is_open() const noexcept
{
    return impl_ != nullptr && impl_->db != nullptr;
}

void Database::execute(std::string_view sql)
{
    char* error = nullptr;
    const std::string sql_text(sql);
    const int rc = impl_->api->exec(impl_->db, sql_text.c_str(), nullptr, nullptr, &error);
    if (rc != sqlite_ok) {
        std::string message = error != nullptr ? error : db_error(*impl_->api, impl_->db, "SQLite exec failed");
        if (error != nullptr) {
            impl_->api->free(error);
        }
        throw std::runtime_error(message);
    }
}

void Database::execute(std::string_view sql, const std::vector<std::string>& values)
{
    auto statement = prepare(sql);
    for (int i = 0; i < static_cast<int>(values.size()); ++i) {
        statement.bind(i + 1, values[static_cast<std::size_t>(i)]);
    }
    statement.step();
}

Statement Database::prepare(std::string_view sql)
{
    if (!is_open()) {
        throw std::runtime_error("cannot prepare SQLite statement on a closed database");
    }
    sqlite3_stmt* stmt = nullptr;
    const std::string sql_text(sql);
    const int rc = impl_->api->prepare_v2(impl_->db, sql_text.c_str(), -1, &stmt, nullptr);
    if (rc != sqlite_ok) {
        throw std::runtime_error(db_error(*impl_->api, impl_->db, "SQLite prepare failed"));
    }
    return Statement(this, stmt);
}

std::string Database::query_text(std::string_view sql)
{
    auto statement = prepare(sql);
    if (statement.step()) {
        return statement.column_text(0);
    }
    return "";
}

std::int64_t Database::query_int(std::string_view sql)
{
    auto statement = prepare(sql);
    if (statement.step()) {
        return statement.column_int64(0);
    }
    return 0;
}

std::vector<QueryRow> Database::query_rows(std::string_view sql)
{
    auto statement = prepare(sql);
    std::vector<QueryRow> rows;
    const int columns = statement.column_count();
    while (statement.step()) {
        std::vector<std::string> row;
        row.reserve(static_cast<std::size_t>(columns));
        for (int i = 0; i < columns; ++i) {
            row.push_back(statement.column_text(i));
        }
        rows.emplace_back(std::move(row));
    }
    return rows;
}

void Database::attach(const std::filesystem::path& path, std::string_view schema_name)
{
    const std::string sql = "attach ? as " + std::string(schema_name);
    execute(sql, {path_string(path)});
}

void Database::set_progress_handler(int instruction_interval, std::function<bool()> should_interrupt)
{
    if (!is_open()) {
        throw std::runtime_error("cannot set SQLite progress handler on a closed database");
    }
    if (instruction_interval <= 0 || !should_interrupt) {
        clear_progress_handler();
        return;
    }

    impl_->progress_callback = std::move(should_interrupt);
    impl_->api->progress_handler(
        impl_->db,
        instruction_interval,
        sqlite_progress_callback,
        &impl_->progress_callback);
}

void Database::clear_progress_handler() noexcept
{
    if (impl_ != nullptr && impl_->db != nullptr) {
        impl_->api->progress_handler(impl_->db, 0, nullptr, nullptr);
    }
    if (impl_ != nullptr) {
        impl_->progress_callback = {};
    }
}

Transaction::Transaction(Database& database, std::string_view begin_sql)
    : database_(&database)
    , active_(true)
{
    database_->execute(begin_sql);
}

Transaction::~Transaction()
{
    rollback();
}

void Transaction::commit()
{
    if (active_) {
        database_->execute("commit");
        active_ = false;
    }
}

void Transaction::rollback() noexcept
{
    if (active_) {
        try {
            database_->execute("rollback");
        } catch (...) {
        }
        active_ = false;
    }
}

} // namespace soff::db
