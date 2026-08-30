// ---------------------------------------------------------------------------
// RuntimeOptions parses every AYTHER_* option once, strictly.
//
// The point of the type is that a malformed option stops being invisible, so
// the interesting cases here are the bad ones: the old code turned atoi("tow")
// into 0 and carried on. Every input is a table in this file -- nothing reads
// the real environment, and nothing constructs a session, emulator, or device.
// ---------------------------------------------------------------------------
#include "runtime_options.h"

#include <cstdio>
#include <map>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) ++failures;
}

/// A stand-in environment. Returning a pointer into the map's own storage is
/// safe because the table outlives every parse in this test.
class Env {
public:
    explicit Env(std::map<std::string, std::string> values)
        : values_(std::move(values)) {}

    ayther::RuntimeOptions::Lookup lookup() const {
        return [this](const char* name) -> const char* {
            const auto it = values_.find(name);
            return it == values_.end() ? nullptr : it->second.c_str();
        };
    }

private:
    std::map<std::string, std::string> values_;
};

ayther::RuntimeOptions parse(std::map<std::string, std::string> values) {
    const Env env(std::move(values));
    return ayther::RuntimeOptions::parse(env.lookup());
}

}  // namespace

int main() {
    using ayther::RuntimeOptions;

    // --- Defaults ---------------------------------------------------------
    {
        const RuntimeOptions options = parse({});
        check(options.voice_router(), "the voice router defaults to on");
        check(options.abi_mirror(), "the ABI mirror defaults to on");
        check(!options.vulkan_verbose(), "Vulkan chatter defaults to off");
        check(!options.video_debug(), "video tracing defaults to off");
        check(options.upload_budget() == RuntimeOptions::kUploadBudgetDefault,
              "the upload budget falls back to its documented default");
        check(options.video_threads() == RuntimeOptions::kVideoThreadsAuto,
              "video threads default to automatic");
        check(options.system_dir().empty() && options.audio_dump().empty() &&
                  options.sf2_dump().empty() && options.voice_dump().empty(),
              "no path option is set without the environment saying so");
        check(options.valid(), "an empty environment produces no diagnostics");
    }

    // --- Booleans accept the spellings people actually type ---------------
    {
        const RuntimeOptions options = parse({{"AYTHER_VOICE_ROUTER", "0"},
                                              {"AYTHER_VK_VERBOSE", "true"},
                                              {"AYTHER_VIDEO_DEBUG", "ON"},
                                              {"AYTHER_ABI_MIRROR", "No"}});
        check(!options.voice_router(), "'0' turns the voice router off");
        check(options.vulkan_verbose(), "'true' enables a flag");
        check(options.video_debug(), "'ON' is accepted case-insensitively");
        check(!options.abi_mirror(), "'No' disables a flag");
        check(options.valid(), "well-formed booleans produce no diagnostics");
    }

    // --- A malformed boolean keeps the default AND is reported ------------
    {
        const RuntimeOptions options = parse({{"AYTHER_VOICE_ROUTER", "maybe"}});
        check(options.voice_router(),
              "a malformed boolean leaves the default in place");
        check(options.diagnostics().size() == 1,
              "a malformed boolean is reported exactly once");
        check(!options.valid(), "a malformed option makes the set invalid");
        const bool names_it =
            options.diagnostics().at(0).find("AYTHER_VOICE_ROUTER") !=
            std::string::npos;
        check(names_it, "the diagnostic names the offending variable");
    }

    // --- Integers ---------------------------------------------------------
    {
        const RuntimeOptions options = parse({{"AYTHER_UPLOAD_BUDGET", "8"},
                                              {"AYTHER_VIDEO_THREADS", "4"}});
        check(options.upload_budget() == 8, "an in-range budget is taken");
        check(options.video_threads() == 4, "an in-range thread count is taken");
        check(options.valid(), "in-range integers produce no diagnostics");
    }

    // This is the case the old atoi path got wrong: a typo became 0, was then
    // clamped to something plausible, and nobody found out.
    {
        const RuntimeOptions options = parse({{"AYTHER_UPLOAD_BUDGET", "tow"}});
        check(options.upload_budget() == RuntimeOptions::kUploadBudgetDefault,
              "a non-numeric budget keeps the default instead of becoming zero");
        check(!options.valid(), "a non-numeric budget is reported");
    }

    {
        const RuntimeOptions options = parse({{"AYTHER_UPLOAD_BUDGET", "8x"}});
        check(options.upload_budget() == RuntimeOptions::kUploadBudgetDefault,
              "trailing garbage rejects the whole value");
        check(!options.valid(), "a partially numeric budget is reported");
    }

    // Out of range is reported rather than quietly clamped, so the difference
    // between "I asked for 64" and "I got 16" is visible.
    {
        const RuntimeOptions options = parse({{"AYTHER_UPLOAD_BUDGET", "999"}});
        check(options.upload_budget() == RuntimeOptions::kUploadBudgetDefault,
              "an out-of-range budget is not silently clamped");
        check(!options.valid(), "an out-of-range budget is reported");
    }

    {
        const RuntimeOptions options = parse({{"AYTHER_UPLOAD_BUDGET", "-1"}});
        check(options.upload_budget() == RuntimeOptions::kUploadBudgetDefault,
              "a negative budget is rejected, not wrapped");
        check(!options.valid(), "a negative budget is reported");
    }

    // --- Paths ------------------------------------------------------------
    {
        const RuntimeOptions options = parse({{"AYTHER_AUDIO_DUMP", "out.wav"},
                                              {"AYTHER_SYSTEM_DIR", "/sys"},
                                              {"AYTHER_VOICE_DUMP", ""}});
        check(options.audio_dump() == "out.wav", "a path option is taken verbatim");
        check(options.system_dir() == "/sys", "the system directory is taken");
        check(options.voice_dump().empty(),
              "an empty value is the same as unset, not an empty path");
        check(options.valid(), "path options produce no diagnostics");
    }

    // --- Every bad option is reported, not just the first -----------------
    {
        const RuntimeOptions options = parse({{"AYTHER_UPLOAD_BUDGET", "tow"},
                                              {"AYTHER_VIDEO_THREADS", "many"},
                                              {"AYTHER_VIDEO_DEBUG", "sometimes"}});
        check(options.diagnostics().size() == 3,
              "each malformed option produces its own diagnostic");
    }

    // --- The value is a value: subsystems take a copy or a const& ---------
    {
        const RuntimeOptions options = parse({{"AYTHER_UPLOAD_BUDGET", "8"}});
        const RuntimeOptions copy = options;
        check(copy.upload_budget() == 8,
              "options copy by value, so a subsystem cannot mutate the source");
    }

    // A default-constructed value is the documented default set, so a subsystem
    // handed one in a test behaves like an unconfigured engine.
    {
        const RuntimeOptions options;
        check(options.voice_router() && options.abi_mirror() &&
                  !options.vulkan_verbose() && options.valid(),
              "a default-constructed option set matches an empty environment");
    }

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
