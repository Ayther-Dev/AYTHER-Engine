// ---------------------------------------------------------------------------
// runtime_options.cpp — parsing and validation for the AYTHER_* options.
//
// Every reader here is strict on purpose. The old code accepted anything: a
// misspelled AYTHER_UPLOAD_BUDGET=tow became atoi("tow") == 0, which then got
// clamped to 1, so a typo silently changed the frame budget and left no trace.
// Now the value is either understood or reported.
// ---------------------------------------------------------------------------
#include "runtime_options.h"

#include "ayther_env.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>

namespace ayther {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

/// Accepts the spellings a person actually types. Anything else is a mistake
/// worth reporting rather than a silent false.
bool parse_bool(std::string_view text, bool& out) {
    const std::string value = lower(text);
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace

void RuntimeOptions::read_bool(const Lookup& lookup, std::string_view name,
                               bool& target) {
    const char* raw = lookup(std::string(name).c_str());
    if (raw == nullptr || *raw == '\0') return;

    bool parsed = false;
    if (!parse_bool(raw, parsed)) {
        diagnostics_.emplace_back(
            std::string(name) + "='" + raw +
            "' is not a boolean (accepted: 0/1, false/true, off/on, no/yes); "
            "keeping " + (target ? "true" : "false"));
        return;
    }
    target = parsed;
}

void RuntimeOptions::read_uint(const Lookup& lookup, std::string_view name,
                               uint32_t min, uint32_t max, uint32_t& target) {
    const char* raw = lookup(std::string(name).c_str());
    if (raw == nullptr || *raw == '\0') return;

    const std::string_view text(raw);
    uint32_t value = 0;
    const auto* const end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || stop != end) {
        diagnostics_.emplace_back(
            std::string(name) + "='" + raw + "' is not a whole number; keeping " +
            std::to_string(target));
        return;
    }
    // Out of range is NOT clamped silently: clamping is how the old code turned
    // a wrong number into a plausible one and hid the mistake.
    if (value < min || value > max) {
        diagnostics_.emplace_back(
            std::string(name) + "=" + std::to_string(value) + " is outside [" +
            std::to_string(min) + ", " + std::to_string(max) + "]; keeping " +
            std::to_string(target));
        return;
    }
    target = value;
}

void RuntimeOptions::read_path(const Lookup& lookup, std::string_view name,
                               std::string& target) {
    const char* raw = lookup(std::string(name).c_str());
    if (raw == nullptr || *raw == '\0') return;
    target = raw;
}

RuntimeOptions RuntimeOptions::parse(const Lookup& lookup) {
    RuntimeOptions options;
    if (!lookup) return options;

    options.read_bool(lookup, "AYTHER_VOICE_ROUTER", options.voice_router_);
    options.read_bool(lookup, "AYTHER_ABI_MIRROR", options.abi_mirror_);
    options.read_bool(lookup, "AYTHER_VK_VERBOSE", options.vulkan_verbose_);
    options.read_bool(lookup, "AYTHER_VIDEO_DEBUG", options.video_debug_);

    options.read_uint(lookup, "AYTHER_UPLOAD_BUDGET", kUploadBudgetMin,
                      kUploadBudgetMax, options.upload_budget_);
    options.read_uint(lookup, "AYTHER_VIDEO_THREADS", kVideoThreadsMin,
                      kVideoThreadsMax, options.video_threads_);

    options.read_path(lookup, "AYTHER_SYSTEM_DIR", options.system_dir_);
    options.read_path(lookup, "AYTHER_AUDIO_DUMP", options.audio_dump_);
    options.read_path(lookup, "AYTHER_SF2_DUMP", options.sf2_dump_);
    options.read_path(lookup, "AYTHER_VOICE_DUMP", options.voice_dump_);

    return options;
}

RuntimeOptions RuntimeOptions::from_environment() {
    return parse([](const char* name) { return env_get(name); });
}

const RuntimeOptions& RuntimeOptions::process() {
    // Read once per process. The options are a snapshot of how the engine was
    // started; a later putenv does not retroactively change decisions already
    // made from them, and pretending otherwise would be worse than freezing.
    static const RuntimeOptions options = from_environment();
    return options;
}

}  // namespace ayther
