#pragma once
// ---------------------------------------------------------------------------
// log.h — the engine's only way to say something.
//
// The engine used to call fprintf(stderr, "[Component] ...") from ~200 places.
// That is invisible to a frontend: a GUI cannot show it, a test cannot assert
// on it, and a host embedding the engine cannot route it anywhere. Worse, the
// hot paths measured 5 ms per line against a Windows console, which is why
// several call sites grew ad-hoc "log once" flags.
//
// Every record now carries four things a consumer can act on -- severity, the
// component that spoke, a STABLE event id, and typed fields -- plus the human
// message. A frontend installs a sink and decides what happens. When nobody
// installs one, the built-in fallback writes to stderr; that fallback is the
// single place in the engine allowed to touch a console stream.
//
// Engine-internal header: not installed, so it must not appear in any public
// header.
// ---------------------------------------------------------------------------
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ayther::log {

enum class Severity : uint8_t {
    Trace = 0,  ///< Per-frame detail. Off unless explicitly enabled.
    Debug,      ///< Diagnostics for a developer chasing something.
    Info,       ///< Normal lifecycle: opened, negotiated, loaded.
    Warning,    ///< Degraded but continuing, and the user may care why.
    Error,      ///< A request failed. Something the caller asked for did not happen.
};

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

/// A typed field value. Deliberately small: a log field that needs anything
/// richer is really a report, and reports do not belong in a log line.
class Value {
public:
    Value(bool value) : data_(value) {}                      // NOLINT(*-explicit-*)
    Value(int32_t value) : data_(static_cast<int64_t>(value)) {}
    Value(int64_t value) : data_(value) {}
    Value(uint32_t value) : data_(static_cast<uint64_t>(value)) {}
    Value(uint64_t value) : data_(value) {}
    Value(double value) : data_(value) {}
    Value(const char* value) : data_(std::string(value ? value : "")) {}
    Value(std::string value) : data_(std::move(value)) {}
    Value(std::string_view value) : data_(std::string(value)) {}

    [[nodiscard]] std::string to_string() const;

private:
    std::variant<bool, int64_t, uint64_t, double, std::string> data_;
};

struct Field {
    std::string_view key;
    Value value;
};

struct Record {
    Severity severity = Severity::Info;
    /// Stable, lowercase, no spaces: "session", "audio", "video", "vulkan".
    std::string_view component;
    /// Stable identifier for THIS event, unique within the component:
    /// "device_open_failed". It is the thing a consumer matches on, so it must
    /// outlive rewordings of the message.
    std::string_view event;
    /// Human text, already formatted. May be empty when the fields say it all.
    std::string_view message;
    std::span<const Field> fields;
};

/// Receives every record at or above the minimum severity. Called on the thread
/// that logged, so an implementation must be thread-safe and must not block.
using Sink = std::function<void(const Record&)>;

/// Installs the frontend's sink. Passing nullptr restores the stderr fallback.
/// Call it before the first session; changing sinks concurrently with logging
/// is not supported.
void set_sink(Sink sink);

void set_min_severity(Severity severity) noexcept;
[[nodiscard]] Severity min_severity() noexcept;

/// True when a record at this severity would reach the sink. Guard an expensive
/// message with this rather than formatting it and throwing it away.
[[nodiscard]] bool enabled(Severity severity) noexcept;

void emit(const Record& record);

/// printf-style, so existing messages migrate without being reworded. `fields`
/// may be empty; prefer moving numbers into fields when a consumer might want
/// to read them.
void write(Severity severity, std::string_view component, std::string_view event,
           std::span<const Field> fields, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

void write(Severity severity, std::string_view component, std::string_view event,
           const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

}  // namespace ayther::log
