#include "storage/Sqlite.h"

#include <sqlite3.h>

#include <limits>
#include <utility>

namespace ldm::storage {

namespace {

base::Error DatabaseError(sqlite3* db, std::string_view context)
{
    std::string message(context);
    if (db != nullptr) {
        message += ": ";
        message += ::sqlite3_errmsg(db);
    }
    return base::MakeError(base::ErrorCode::Database, std::move(message));
}

int CheckedSize(size_t size)
{
    return size > static_cast<size_t>(std::numeric_limits<int>::max()) ? -1 : static_cast<int>(size);
}

}  // namespace

// ---- Connection ----

base::Result<Connection> Connection::Open(const std::string& path)
{
    sqlite3* db = nullptr;
    constexpr int kFlags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    const int result = ::sqlite3_open_v2(path.c_str(), &db, kFlags, nullptr);
    if (result != SQLITE_OK) {
        base::Error error = DatabaseError(db, "cannot open database");
        ::sqlite3_close_v2(db);
        return error;
    }
    return Connection(db);
}

Connection::Connection(Connection&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Connection& Connection::operator=(Connection&& other) noexcept
{
    if (this != &other) {
        ::sqlite3_close_v2(db_);
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Connection::~Connection()
{
    ::sqlite3_close_v2(db_);
}

base::Status Connection::Execute(std::string_view sql)
{
    const std::string text(sql);
    char* errorMessage = nullptr;
    const int result = ::sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &errorMessage);
    if (result != SQLITE_OK) {
        std::string message = "statement failed: ";
        message += errorMessage != nullptr ? errorMessage : ::sqlite3_errstr(result);
        ::sqlite3_free(errorMessage);
        return base::MakeError(base::ErrorCode::Database, std::move(message));
    }
    return {};
}

base::Result<Statement> Connection::Prepare(std::string_view sql)
{
    const int length = CheckedSize(sql.size());
    if (length < 0) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "SQL text too long");
    }
    sqlite3_stmt* statement = nullptr;
    const int result = ::sqlite3_prepare_v3(db_, sql.data(), length, 0, &statement, nullptr);
    if (result != SQLITE_OK) {
        return DatabaseError(db_, "cannot prepare statement");
    }
    return Statement(db_, statement);
}

std::int64_t Connection::LastInsertRowId() const
{
    return ::sqlite3_last_insert_rowid(db_);
}

int Connection::ChangedRows() const
{
    return ::sqlite3_changes(db_);
}

std::string Connection::LastErrorMessage() const
{
    return db_ != nullptr ? ::sqlite3_errmsg(db_) : "no connection";
}

// ---- Statement ----

Statement::Statement(Statement&& other) noexcept
    : db_(other.db_), statement_(std::exchange(other.statement_, nullptr)), bindError_(other.bindError_)
{
}

Statement& Statement::operator=(Statement&& other) noexcept
{
    if (this != &other) {
        ::sqlite3_finalize(statement_);
        db_ = other.db_;
        statement_ = std::exchange(other.statement_, nullptr);
        bindError_ = other.bindError_;
    }
    return *this;
}

Statement::~Statement()
{
    ::sqlite3_finalize(statement_);
}

void Statement::RecordBindError(int result)
{
    if (result != SQLITE_OK && bindError_ == 0) {
        bindError_ = result;
    }
}

Statement& Statement::Bind(int index, std::int64_t value)
{
    RecordBindError(::sqlite3_bind_int64(statement_, index, value));
    return *this;
}

Statement& Statement::Bind(int index, std::string_view value)
{
    const int length = CheckedSize(value.size());
    if (length < 0) {
        RecordBindError(SQLITE_TOOBIG);
        return *this;
    }
    // An empty view may have a null data pointer, which SQLite would store as NULL.
    const char* data = value.data() != nullptr ? value.data() : "";
    RecordBindError(::sqlite3_bind_text(statement_, index, data, length, SQLITE_TRANSIENT));
    return *this;
}

Statement& Statement::Bind(int index, std::span<const std::uint8_t> value)
{
    const int length = CheckedSize(value.size());
    if (length < 0) {
        RecordBindError(SQLITE_TOOBIG);
        return *this;
    }
    RecordBindError(::sqlite3_bind_blob(statement_, index, value.data(), length, SQLITE_TRANSIENT));
    return *this;
}

Statement& Statement::BindNull(int index)
{
    RecordBindError(::sqlite3_bind_null(statement_, index));
    return *this;
}

base::Result<bool> Statement::Step()
{
    if (bindError_ != 0) {
        return base::MakeError(base::ErrorCode::Database,
                               std::string("cannot bind parameter: ") + ::sqlite3_errstr(bindError_));
    }
    const int result = ::sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result == SQLITE_DONE) {
        return false;
    }
    return DatabaseError(db_, "statement failed");
}

base::Status Statement::Run()
{
    base::Result<bool> step = Step();
    if (!step) {
        return step.GetError();
    }
    if (step.Value()) {
        return base::MakeError(base::ErrorCode::Database, "statement unexpectedly returned rows");
    }
    return {};
}

void Statement::Reset()
{
    ::sqlite3_reset(statement_);
    ::sqlite3_clear_bindings(statement_);
    bindError_ = 0;
}

bool Statement::IsNull(int column) const
{
    return ::sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

std::int64_t Statement::Int64(int column) const
{
    return ::sqlite3_column_int64(statement_, column);
}

std::string Statement::Text(int column) const
{
    // SQLite returns UTF-8 as unsigned char; reinterpreting as char is the documented usage.
    const auto* text = reinterpret_cast<const char*>(::sqlite3_column_text(statement_, column));
    const int length = ::sqlite3_column_bytes(statement_, column);
    return text != nullptr ? std::string(text, static_cast<size_t>(length)) : std::string();
}

std::vector<std::uint8_t> Statement::Blob(int column) const
{
    const auto* data = static_cast<const std::uint8_t*>(::sqlite3_column_blob(statement_, column));
    const int length = ::sqlite3_column_bytes(statement_, column);
    if (data == nullptr || length <= 0) {
        return {};
    }
    return std::vector<std::uint8_t>(data, data + length);
}

std::optional<std::int64_t> Statement::OptionalInt64(int column) const
{
    if (IsNull(column)) {
        return std::nullopt;
    }
    return Int64(column);
}

std::optional<std::string> Statement::OptionalText(int column) const
{
    if (IsNull(column)) {
        return std::nullopt;
    }
    return Text(column);
}

// ---- Transaction ----

base::Result<Transaction> Transaction::Begin(Connection& connection)
{
    base::Status begin = connection.Execute("BEGIN IMMEDIATE");
    if (!begin) {
        return begin.GetError();
    }
    Transaction transaction(connection);
    transaction.active_ = true;
    return transaction;
}

Transaction::Transaction(Transaction&& other) noexcept
    : connection_(other.connection_), active_(std::exchange(other.active_, false))
{
}

Transaction::~Transaction()
{
    if (active_) {
        // Rollback failure leaves SQLite to roll back on close; nothing more can be done here.
        (void)connection_->Execute("ROLLBACK");
    }
}

base::Status Transaction::Commit()
{
    if (!active_) {
        return base::MakeError(base::ErrorCode::Database, "transaction is not active");
    }
    base::Status commit = connection_->Execute("COMMIT");
    if (commit) {
        active_ = false;
    }
    return commit;
}

}  // namespace ldm::storage
