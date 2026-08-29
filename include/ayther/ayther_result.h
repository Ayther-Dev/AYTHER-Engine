#pragma once
// ---------------------------------------------------------------------------
// ayther_result.h — no-throw error model for the runtime / FFI boundary.
//
// The FFI never propagates exceptions or panics across the binary boundary
// (see docs/API_COMPATIBILITY.md#error-handling). Expected failures (corrupt
// pack, malformed TOML, missing ROM) surface as an ayther::Result, so the
// caller — especially ayther_lab — can show *why* something failed, not just
// *that* it failed.
//
// C++20: no std::expected (that is C++23). This is the project's vehicle.
// ---------------------------------------------------------------------------

#include <optional>
#include <string>
#include <utility>

namespace ayther {

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------

enum class ErrorCode {
    Ok = 0,
    NotFound,       ///< file/asset/symbol not found
    BadFormat,      ///< malformed TOML / pack / ROM
    BadSignature,   ///< ED25519 verification failed
    Io,             ///< filesystem / read error
    Unsupported,    ///< feature/platform not supported
    Internal,       ///< caught Rust panic / unexpected state
};

struct Error {
    ErrorCode   code = ErrorCode::Ok;
    std::string message;
};

// ---------------------------------------------------------------------------
// Result<T> — value-or-error. Result<void> is the no-value specialization.
// ---------------------------------------------------------------------------

template <class T>
struct Result {
    std::optional<T> value;
    Error            error;

    Result(T v) : value(std::move(v)) {}                 // success
    Result(Error e) : error(std::move(e)) {}             // failure

    /// true on success.
    explicit operator bool() const { return error.code == ErrorCode::Ok && value.has_value(); }

    T&       operator*()        { return *value; }
    const T& operator*()  const { return *value; }
    T*       operator->()       { return &*value; }
    const T* operator->() const { return &*value; }
};

template <>
struct Result<void> {
    Error error;

    Result() = default;                                   // success
    Result(Error e) : error(std::move(e)) {}              // failure

    static Result ok() { return {}; }
    static Result fail(ErrorCode c, std::string msg) { return Result{ Error{ c, std::move(msg) } }; }

    explicit operator bool() const { return error.code == ErrorCode::Ok; }
};

}  // namespace ayther
