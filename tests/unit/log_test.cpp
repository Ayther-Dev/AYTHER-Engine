// ---------------------------------------------------------------------------
// Structured logging: severity, component, stable event id, typed fields, and
// a sink the frontend owns.
//
// Nothing here constructs a session, opens a device, or writes to a console:
// the sink IS the observation point, which is the whole reason the facility
// exists.
// ---------------------------------------------------------------------------
#include "log.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) ++failures;
}

/// A record captured by value: Record holds views into the emitter's storage,
/// so a sink that wants to keep one must copy the parts it needs.
struct Captured {
    ayther::log::Severity severity;
    std::string component;
    std::string event;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
};

std::vector<Captured> captured;

void install_capturing_sink() {
    captured.clear();
    ayther::log::set_sink([](const ayther::log::Record& record) {
        Captured copy;
        copy.severity = record.severity;
        copy.component.assign(record.component);
        copy.event.assign(record.event);
        copy.message.assign(record.message);
        for (const ayther::log::Field& field : record.fields) {
            copy.fields.emplace_back(std::string(field.key),
                                     field.value.to_string());
        }
        captured.push_back(std::move(copy));
    });
}

}  // namespace

int main() {
    using ayther::log::Field;
    using ayther::log::Severity;

    ayther::log::set_min_severity(Severity::Trace);
    install_capturing_sink();

    // --- The envelope reaches the sink intact ----------------------------
    {
        ayther::log::write(Severity::Error, "audio", "device_open_failed",
                           "could not open %s at %d Hz", "default", 44100);
        check(captured.size() == 1, "a record reaches the installed sink");
        check(captured[0].severity == Severity::Error, "severity survives");
        check(captured[0].component == "audio", "the component survives");
        check(captured[0].event == "device_open_failed",
              "the stable event id survives");
        check(captured[0].message == "could not open default at 44100 Hz",
              "the printf-style message is formatted before dispatch");
        check(captured[0].fields.empty(), "no fields were claimed");
    }

    // --- Typed fields -----------------------------------------------------
    {
        captured.clear();
        const Field fields[] = {
            {"rate", 44100u},
            {"channels", int64_t{2}},
            {"muted", false},
            {"gain", 0.5},
            {"asset", "hd/boss.wav"},
        };
        ayther::log::write(Severity::Info, "audio", "stream_opened", fields,
                           "stream ready");
        check(captured.size() == 1, "a record with fields reaches the sink");
        check(captured[0].fields.size() == 5, "every field is delivered");
        check(captured[0].fields[0].first == "rate" &&
                  captured[0].fields[0].second == "44100",
              "an unsigned field keeps its value");
        check(captured[0].fields[1].second == "2", "a signed field keeps its value");
        check(captured[0].fields[2].second == "false",
              "a boolean renders as a word, not as 0");
        check(captured[0].fields[4].second == "hd/boss.wav",
              "a string field keeps its value");
    }

    // --- Severity filtering happens before formatting --------------------
    {
        captured.clear();
        ayther::log::set_min_severity(Severity::Warning);
        ayther::log::write(Severity::Debug, "video", "frame_selected",
                           "frame %d", 7);
        check(captured.empty(), "a record below the threshold is dropped");
        check(!ayther::log::enabled(Severity::Debug),
              "enabled() lets a caller skip building an expensive message");
        check(ayther::log::enabled(Severity::Error),
              "enabled() admits severities at or above the threshold");

        ayther::log::write(Severity::Error, "video", "decode_failed", "bad frame");
        check(captured.size() == 1, "a record at or above the threshold passes");
        ayther::log::set_min_severity(Severity::Trace);
    }

    // --- Ordering ---------------------------------------------------------
    {
        captured.clear();
        for (int i = 0; i < 3; ++i) {
            ayther::log::write(Severity::Info, "session", "step", "%d", i);
        }
        check(captured.size() == 3 && captured[0].message == "0" &&
                  captured[2].message == "2",
              "records arrive in the order they were emitted");
    }

    // --- The frontend can hand the engine back to the fallback -----------
    {
        captured.clear();
        ayther::log::set_sink(nullptr);
        ayther::log::write(Severity::Error, "session", "sink_removed",
                           "this one goes to the central fallback");
        check(captured.empty(), "after reset the old sink no longer receives");
        install_capturing_sink();
        ayther::log::write(Severity::Info, "session", "sink_restored", "back");
        check(captured.size() == 1, "a sink can be installed again");
    }

    ayther::log::set_sink(nullptr);
    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
