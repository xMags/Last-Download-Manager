#pragma once

#include "base/Result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace ldm::storage {

class Statement;

// Owns one SQLite connection. Not thread-safe: the core thread is its only user.
class Connection {
public:
    // Opens or creates a database file (UTF-8 path) or ":memory:".
    [[nodiscard]] static base::Result<Connection> Open(const std::string& path);

    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    // Runs one or more statements without results (schema, pragmas).
    [[nodiscard]] base::Status Execute(std::string_view sql);
    [[nodiscard]] base::Result<Statement> Prepare(std::string_view sql);

    [[nodiscard]] std::int64_t LastInsertRowId() const;
    [[nodiscard]] int ChangedRows() const;
    [[nodiscard]] std::string LastErrorMessage() const;

private:
    explicit Connection(sqlite3* db) noexcept : db_(db) {}

    sqlite3* db_ = nullptr;
};

// One prepared statement. Bind indexes are 1-based, column indexes 0-based, as in SQLite.
class Statement {
public:
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    Statement& Bind(int index, std::int64_t value);
    Statement& Bind(int index, int value) { return Bind(index, static_cast<std::int64_t>(value)); }
    Statement& Bind(int index, bool value) { return Bind(index, static_cast<std::int64_t>(value ? 1 : 0)); }
    Statement& Bind(int index, std::string_view value);
    // Without this overload a string literal would convert to bool, not to string_view.
    Statement& Bind(int index, const char* value) { return Bind(index, std::string_view(value)); }
    Statement& Bind(int index, std::span<const std::uint8_t> value);
    Statement& BindNull(int index);

    template <typename T>
    Statement& Bind(int index, const std::optional<T>& value)
    {
        if (value.has_value()) {
            return Bind(index, *value);
        }
        return BindNull(index);
    }

    // Advances to the next row. Returns true with a row, false when done, error on failure.
    [[nodiscard]] base::Result<bool> Step();
    // Runs a statement that returns no rows.
    [[nodiscard]] base::Status Run();
    void Reset();

    [[nodiscard]] bool IsNull(int column) const;
    [[nodiscard]] std::int64_t Int64(int column) const;
    [[nodiscard]] std::string Text(int column) const;
    [[nodiscard]] std::vector<std::uint8_t> Blob(int column) const;
    [[nodiscard]] std::optional<std::int64_t> OptionalInt64(int column) const;
    [[nodiscard]] std::optional<std::string> OptionalText(int column) const;

private:
    friend class Connection;
    Statement(sqlite3* db, sqlite3_stmt* statement) noexcept : db_(db), statement_(statement) {}

    void RecordBindError(int result);

    sqlite3* db_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
    int bindError_ = 0;
};

// Rolls back unless Commit() succeeded. Use on the stack around multi-statement writes.
class Transaction {
public:
    [[nodiscard]] static base::Result<Transaction> Begin(Connection& connection);

    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    [[nodiscard]] base::Status Commit();

private:
    explicit Transaction(Connection& connection) noexcept : connection_(&connection) {}

    Connection* connection_ = nullptr;
    bool active_ = false;
};

}  // namespace ldm::storage
