#pragma once

#include <cassert>
#include <string>
#include <utility>
#include <variant>

namespace ldm::base {

// Broad failure classes. Callers branch on the code; the message is for logs and the UI.
enum class ErrorCode {
    Unknown,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    Io,
    DiskFull,
    Database,
    Network,
    Protocol,
    Cancelled,
    Unsupported,
};

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
};

[[nodiscard]] inline Error MakeError(ErrorCode code, std::string message)
{
    return Error{code, std::move(message)};
}

// Value-or-error return type, used instead of exceptions across module boundaries.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool Ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return Ok(); }

    [[nodiscard]] T& Value() &
    {
        assert(Ok());
        return std::get<0>(storage_);
    }
    [[nodiscard]] const T& Value() const&
    {
        assert(Ok());
        return std::get<0>(storage_);
    }
    [[nodiscard]] T&& Value() &&
    {
        assert(Ok());
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] const Error& GetError() const&
    {
        assert(!Ok());
        return std::get<1>(storage_);
    }

private:
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    [[nodiscard]] bool Ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] const Error& GetError() const&
    {
        assert(!ok_);
        return error_;
    }

private:
    Error error_;
    bool ok_ = true;
};

using Status = Result<void>;

}  // namespace ldm::base
