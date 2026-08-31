// ---------------------------------------------------------------------------
// e2e_determinism — the whole pipeline, twice, and the answers must match.
//
// Every other oracle in this repository measures one seam. This one runs the
// product: a synthetic ROM, the test core, a signed pack, and a scripted input
// track go into a real AytherSession, and what comes out -- frames, audio,
// events, RAM -- is hashed.
//
// It asserts two different things, and the difference matters:
//
//   1. RUN-TO-RUN. Two independent sessions in this process, same inputs, must
//      produce identical hashes. This catches nondeterminism from uninitialised
//      memory, pointer values, iteration order, or time.
//
//   2. RUN-TO-GOLDEN. The hashes must equal constants pinned in this file.
//      This is the half that catches a difference BETWEEN platforms, and it is
//      the reason the numbers are written down rather than merely compared to
//      each other: two runs on one machine agreeing proves nothing about the
//      other machine. CI running this on Windows and on Linux is what turns
//      those constants into a cross-platform claim.
//
// Everything hashed is integer data, but not every axis turns out to be
// invariant across BUILDS. Measured on 2026-08-30, same machine, same source,
// only the optimisation level differing (RelWithDebInfo against the -O0
// coverage build), with the core reporting the same frame count either way:
//
//   frames  identical      pinned as a golden
//   audio   identical      pinned as a golden
//   events  differs        run-to-run only (1 event found vs 2)
//   ram     differs        run-to-run only
//
// So `events` and `ram` are compared between the two runs -- which is the
// property that catches real nondeterminism -- but are NOT pinned. The event
// list comes out of floating-point audio analysis, and pinning a number that
// moves with the optimiser would make the oracle fail for a reason that has
// nothing to do with the engine being wrong. The RAM difference has the same
// build-dependence; its exact mechanism is not established here, and claiming
// one without measuring it would be a guess dressed as a finding.
//
// This is not a detail: the C++ coverage job builds -O0 and runs this same
// suite, so pinning all four axes would leave that job permanently red.
// ---------------------------------------------------------------------------
#include "../common/synth_rom.h"

#include "ayther_recording.h"
#include "ayther_session.h"
#include "ayther_core_ffi.h"
#include "trusted_pack_fixture.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

// ---- FNV-1a, so the hash itself cannot be a source of difference ----------

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime  = 1099511628211ULL;

struct Hash {
    uint64_t value = kFnvOffset;

    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            value ^= p[i];
            value *= kFnvPrime;
        }
    }
    // Integers are folded byte by byte in a fixed order rather than hashed by
    // their in-memory layout, so the result does not depend on the target.
    void u64(uint64_t v) {
        for (int shift = 0; shift < 64; shift += 8) {
            const uint8_t byte = static_cast<uint8_t>(v >> shift);
            bytes(&byte, 1);
        }
    }
    void u32(uint32_t v) { u64(v); }
    void u8(uint8_t v) { u64(v); }
};

/// What one run of the pipeline produced.
struct RunResult {
    uint64_t frames = 0;   ///< every emulator framebuffer, in order
    uint64_t audio  = 0;   ///< the recording's per-frame audio occurrence hashes
    uint64_t events = 0;   ///< what the event analysis found in the take
    uint64_t ram    = 0;   ///< work RAM at the end of the run
    uint32_t frame_count = 0;
    uint32_t event_count = 0;
    uint64_t core_frames = 0;  ///< frames the CORE ran, per its own counter
    bool     pack_loaded = false;
    bool     ok = false;
};

constexpr int kFrames = 240;

/// A fixed input track. Scripted rather than random precisely so that the
/// inputs are part of the pinned answer: a different track is a different run.
uint16_t input_for_frame(int frame) {
    switch (frame % 30) {
        case 0:  return 0x0001;             // B
        case 7:  return 0x0080;             // A
        case 13: return 0x0040 | 0x0001;    // right + B
        case 21: return 0x0020;             // left
        default: return 0x0000;
    }
}

RunResult run_once(const std::string& core, const std::string& rom,
                   const std::string& pack, const std::string& registry) {
    RunResult out;

    ayther::AytherSession::Config config;
    config.core_path = core;
    config.rom_path = rom;
    config.pack_path = pack;
    config.trust_registry = registry;
    // No device: CI has no sound card, and opening one would make the run
    // depend on the host's audio stack.
    config.enable_audio = false;
    // The pack under test is the one handed in; deriving a second one from the
    // core path would silently change what is loaded.
    config.derive_core_pack = false;

    auto created = ayther::AytherSession::create(config);
    if (!created) return out;
    auto& session = *created;
    // A pack that silently failed to open would leave the rest of this run
    // measuring a pipeline with a stage missing, and passing anyway.
    out.pack_loaded = session->has_pack();

    Hash frames;
    Hash ram;

    session->record_start();
    for (int frame = 0; frame < kFrames; ++frame) {
        session->set_input(0, input_for_frame(frame));
        const ayther::FrameView& view = session->step();
        frames.u32(view.fb_width);
        frames.u32(view.fb_height);
        if (view.fb_pixels != nullptr && view.fb_height > 0) {
            // Row by row, using the declared pitch: hashing the whole buffer
            // would fold in inter-row padding that is not part of the picture.
            const auto* rows = static_cast<const uint8_t*>(view.fb_pixels);
            const size_t row_bytes = static_cast<size_t>(view.fb_width) * 2;
            for (uint32_t y = 0; y < view.fb_height; ++y) {
                frames.bytes(rows + static_cast<size_t>(y) * view.fb_pitch,
                             row_bytes);
            }
        }
        ++out.frame_count;
    }
    session->record_stop();

    if (const uint8_t* work = session->work_ram()) {
        ram.bytes(work, session->work_ram_size());
        // The test core stamps its own frame counter into the first eight
        // bytes of work RAM. Reading it back distinguishes "the RAM hash
        // changed because the emulation diverged" from "the session ran the
        // core a different number of times", which look identical in a hash.
        if (session->work_ram_size() >= sizeof(uint64_t)) {
            std::memcpy(&out.core_frames, work, sizeof(out.core_frames));
        }
    }

    const ayther::AytherRecording take = session->take_recording();

    Hash audio;
    audio.u64(take.audio_hashes.size());
    for (const uint64_t hash : take.audio_hashes) audio.u64(hash);
    for (const uint32_t offset : take.audio_offsets) audio.u32(offset);

    Hash events;
    out.event_count = session->analyze_audio_events(take);
    events.u32(out.event_count);
    if (const AytherAudioEvent* list = session->audio_events()) {
        for (uint32_t i = 0; i < out.event_count; ++i) {
            events.u64(list[i].signature);
            events.u64(list[i].instrument);
            events.u32(list[i].start_frame);
            events.u32(list[i].end_frame);
            events.u8(list[i].chip);
            events.u8(list[i].channel);
            events.u8(list[i].pitch);
        }
    }

    out.frames = frames.value;
    out.ram = ram.value;
    out.audio = audio.value;
    out.events = events.value;
    out.ok = true;
    return out;
}

/// Bakes a production-signed pack, so the run exercises the path a shipped
/// frontend actually takes. An optimized build refuses an unsigned pack AND
/// refuses the development key, so a signed pack plus its registry is the only
/// combination that opens one -- which is exactly why the session has to be
/// able to be handed a registry at all.
std::string bake_pack(ayther::test::TrustedPackFixture& fixture) {
    const std::string manifest =
        "[pack]\n"
        "name       = \"e2e_determinism\"\n"
        "version    = \"1.0.0\"\n"
        "game_id    = \"crc32:e2e00001\"\n"
        "ayther_min = \"0.1.0\"\n"
        "\n[regions]\n"
        "default = \"NTSC\"\n"
        "supported = [\"NTSC\"]\n";
    const std::string asset = "deterministic asset bytes";

    char error[512] = {};
    const bool staged =
        fixture.add_bytes("manifest.toml",
                          reinterpret_cast<const uint8_t*>(manifest.data()),
                          manifest.size()) &&
        fixture.add_bytes("assets/tone.bin",
                          reinterpret_cast<const uint8_t*>(asset.data()),
                          asset.size());
    if (!staged || !fixture.finish(error, sizeof(error))) {
        std::printf("  pack builder: %s\n", error);
        return {};
    }
    return fixture.pack_path();
}

}  // namespace

int main() {
    namespace fs = std::filesystem;

    std::printf("=== e2e_determinism — ROM + core + pack + inputs, twice ===\n");

    const char* core_env = ayther::env_get("AYTHER_ABI_CORE");
    const std::string core = core_env != nullptr ? core_env : "";
    if (core.empty() || !fs::exists(core)) {
        // Unlike the ABI oracles this has no legacy half to fall back to: with
        // no core there is no pipeline to measure, and saying so is the only
        // honest outcome.
        std::printf("[skip] no core at AYTHER_ABI_CORE\n");
        return 77;
    }

    const std::string rom = ayther::synth::probe_rom_path();
    if (rom.empty() || !fs::exists(rom)) {
        std::printf("[skip] could not prepare a ROM\n");
        return 77;
    }

    ayther::test::TrustedPackFixture fixture{"e2e_determinism"};
    const std::string pack = bake_pack(fixture);
    const std::string registry = fixture.registry_path();
    if (pack.empty()) {
        std::printf("[skip] could not bake the fixture pack\n");
        return 77;
    }

    std::printf("  core: %s\n  rom:  %s\n  pack: %s\n  frames: %d\n",
                core.c_str(), rom.c_str(), pack.c_str(), kFrames);

    const RunResult first = run_once(core, rom, pack, registry);
    const RunResult second = run_once(core, rom, pack, registry);

    check(first.ok && second.ok, "both sessions ran the whole track");
    check(first.pack_loaded && second.pack_loaded,
          "the pack actually opened (a silent miss would still hash cleanly)");
    if (!first.ok || !second.ok) {
        std::printf("\n=== FAIL ===\n");
        return 1;
    }

    std::printf("\n  frames=%016llX  audio=%016llX  events=%016llX  ram=%016llX\n",
                static_cast<unsigned long long>(first.frames),
                static_cast<unsigned long long>(first.audio),
                static_cast<unsigned long long>(first.events),
                static_cast<unsigned long long>(first.ram));
    std::printf("  frame_count=%u  event_count=%u  core_frames=%llu\n",
                first.frame_count, first.event_count,
                static_cast<unsigned long long>(first.core_frames));

    // --- 1. Run to run ----------------------------------------------------
    check(first.frame_count == static_cast<uint32_t>(kFrames),
          "the run produced every frame it was asked for");
    check(first.frames == second.frames, "frames: two runs agree");
    check(first.audio == second.audio, "audio: two runs agree");
    check(first.events == second.events, "events: two runs agree");
    check(first.ram == second.ram, "work RAM: two runs agree");

    // --- Non-vacuity ------------------------------------------------------
    // A pipeline that produced nothing would agree with itself perfectly, so
    // the agreement above is only worth something if there was output.
    check(first.frames != kFnvOffset, "frames: something was actually hashed");
    check(first.ram != kFnvOffset, "work RAM: something was actually hashed");

    // --- 2. Run to golden -------------------------------------------------
    // Pinned from a Windows run. CI runs this on Linux too; a mismatch there is
    // the cross-platform difference this test exists to surface, and it should
    // be investigated rather than re-pinned.
#if defined(AYTHER_E2E_GOLDEN_FRAMES)
    const bool golden_frames = first.frames == AYTHER_E2E_GOLDEN_FRAMES;
    const bool golden_audio  = first.audio  == AYTHER_E2E_GOLDEN_AUDIO;
    const bool golden_events = first.events == AYTHER_E2E_GOLDEN_EVENTS;
    const bool golden_ram    = first.ram    == AYTHER_E2E_GOLDEN_RAM;
    if (!golden_frames || !golden_audio) {
        std::printf("\n  expected frames=%016llX audio=%016llX events=%016llX ram=%016llX\n",
                    static_cast<unsigned long long>(AYTHER_E2E_GOLDEN_FRAMES),
                    static_cast<unsigned long long>(AYTHER_E2E_GOLDEN_AUDIO),
                    static_cast<unsigned long long>(AYTHER_E2E_GOLDEN_EVENTS),
                    static_cast<unsigned long long>(AYTHER_E2E_GOLDEN_RAM));
    }
    check(golden_frames, "frames: matches the pinned cross-platform hash");
    check(golden_audio, "audio: matches the pinned cross-platform hash");
    // events and ram are reported, not asserted: see the note at the top.
    std::printf("  (not pinned) events golden %s, ram golden %s\n",
                golden_events ? "matches" : "differs",
                golden_ram ? "matches" : "differs");
#else
    std::printf("\n  [aviso] sin hashes fijados — solo se midio run-to-run.\n"
                "          Fijarlos con -DAYTHER_E2E_GOLDEN_*.\n");
#endif

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
