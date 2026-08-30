// ---------------------------------------------------------------------------
// log.cpp — sink dispatch and the built-in stderr fallback.
//
// The fallback is the ONLY console writer left in the engine. It exists so a
// host that installs nothing still sees errors; everything else routes through
// whatever sink the frontend installed.
// ---------------------------------------------------------------------------
#include "log.h"

#include <cstdio>
#include <mutex>
#include <vector>

namespace ayther::log {
namespace {

struct State {
    std::mutex mutex;
    Sink sink;
    Severity min_severity = Severity::Info;
};

State& state() {
    static State instance;
    return instance;
}

/// The fallback format keeps the component visible the way the old
/// "[Component] ..." prefixes did, so existing logs stay recognisable, and adds
/// the severity and event id that were never there.
void write_fallback(const Record& record) {
    std::string line;
    line.reserve(record.message.size() + 64);
    line += to_string(record.severity);
    line += " [";
    line.append(record.component.data(), record.component.size());
    line += '/';
    line.append(record.event.data(), record.event.size());
    line += "] ";
    line.append(record.message.data(), record.message.size());
    for (const Field& field : record.fields) {
        line += ' ';
        line.append(field.key.data(), field.key.size());
        line += '=';
        line += field.value.to_string();
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
}

std::string format_message(const char* format, std::va_list args) {
    if (format == nullptr) return {};

    std::va_list measure;
    va_copy(measure, args);
    const int needed = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (needed <= 0) return {};

    std::string text(static_cast<size_t>(needed), '\0');
    std::vsnprintf(text.data(), text.size() + 1, format, args);
    return text;
}

}  // namespace

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::Trace:   return "TRACE";
        case Severity::Debug:   return "DEBUG";
        case Severity::Info:    return "INFO";
        case Severity::Warning: return "WARN";
        case Severity::Error:   return "ERROR";
    }
    return "INFO";
}

std::string Value::to_string() const {
    if (const auto* value = std::get_if<bool>(&data_)) {
        return *value ? "true" : "false";
    }
    if (const auto* value = std::get_if<int64_t>(&data_)) {
        return std::to_string(*value);
    }
    if (const auto* value = std::get_if<uint64_t>(&data_)) {
        return std::to_string(*value);
    }
    if (const auto* value = std::get_if<double>(&data_)) {
        return std::to_string(*value);
    }
    return std::get<std::string>(data_);
}

void set_sink(Sink sink) {
    State& s = state();
    const std::lock_guard<std::mutex> guard(s.mutex);
    s.sink = std::move(sink);
}

void set_min_severity(Severity severity) noexcept {
    State& s = state();
    const std::lock_guard<std::mutex> guard(s.mutex);
    s.min_severity = severity;
}

Severity min_severity() noexcept {
    State& s = state();
    const std::lock_guard<std::mutex> guard(s.mutex);
    return s.min_severity;
}

bool enabled(Severity severity) noexcept {
    return severity >= min_severity();
}

void emit(const Record& record) {
    State& s = state();
    Sink sink;
    {
        const std::lock_guard<std::mutex> guard(s.mutex);
        if (record.severity < s.min_severity) return;
        sink = s.sink;
    }
    // Called outside the lock: a frontend sink may do arbitrary work, and
    // holding the log mutex across it invites a deadlock with its own logging.
    if (sink) {
        sink(record);
        return;
    }
    write_fallback(record);
}

void write(Severity severity, std::string_view component, std::string_view event,
           std::span<const Field> fields, const char* format, ...) {
    if (!enabled(severity)) return;

    std::va_list args;
    va_start(args, format);
    const std::string message = format_message(format, args);
    va_end(args);

    emit(Record{severity, component, event, message, fields});
}

void write(Severity severity, std::string_view component, std::string_view event,
           const char* format, ...) {
    if (!enabled(severity)) return;

    std::va_list args;
    va_start(args, format);
    const std::string message = format_message(format, args);
    va_end(args);

    emit(Record{severity, component, event, message, {}});
}

}  // namespace ayther::log
