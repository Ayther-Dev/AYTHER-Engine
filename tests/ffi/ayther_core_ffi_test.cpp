// ---------------------------------------------------------------------------
// ayther_core_ffi_test.cpp — Integration tests for the ayther_core C FFI.
//
// Tests the header + static lib boundary: catches ABI layout drift, missing
// symbols, and lifecycle regressions that Rust unit tests cannot detect
// (they never cross the extern "C" boundary from the C++ side).
//
// Build:   cmake --build <build_dir> --target ayther_core_ffi_tests
// Run:     ctest -C RelWithDebInfo --output-on-failure
//          or directly: bin/ayther_core_ffi_tests.exe
//
// No SDL3 or Vulkan dependency — links only against ayther_core.lib.
// ---------------------------------------------------------------------------
#include <ayther/ayther_core_ffi.h>
#include "trusted_pack_fixture.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time ABI checks.
//
// A static_assert failure means the C++ header drifted from the Rust
// #[repr(C)] struct definitions in lib.rs.  Fix: update both sides together.
//
// Size derivations (x86-64, System V / Windows x64 ABI):
//   AytherTileOccurrence  : hash(8)  + tile_x(4)    + tile_y(4)    = 16
//   AytherTileSub         : path[256]+ tile_x(4)    + tile_y(4)    = 264
//   AytherSpriteOccurrence: hash(8)  + anim_group_id(8)            = 16
//                         + w(1)+h(1)+sx(2)+sy(2)+link(1)+palette(1)+priority(1)
//                         + slot(1)+hflip(1)+vflip(1)
//                         = 28 → pad→32 (align 8)
//   AytherSpriteSub       : path[256]+ sx(2)+sy(2)+w(1)+h(1)
//                         + w_px(2)+h_px(2)+mirror(1)+palette(1)
//                         + synth_pal(1)+ref_rgb(3)
//                         = 272 (align 2, sin pad)
//   AytherAudioOccurrence : hash(8)  + frame_count(4) + hits(4)    = 16
//   AytherAudioSub        : hash(8)  + path[256] + frame_count(4)  = 268 → pad→272 (align 8)
//   AytherShaderParams    : crt(4)   + scan(4)  + vignette(4)      = 12
// ---------------------------------------------------------------------------

static_assert(sizeof(AytherTileOccurrence)   == 16,
    "ABI: AytherTileOccurrence size mismatch with Rust repr(C)");

static_assert(sizeof(AytherTileSub)          == 264,
    "ABI: AytherTileSub size mismatch with Rust repr(C)");

static_assert(sizeof(AytherSpriteOccurrence) == 32,
    "ABI: AytherSpriteOccurrence size mismatch with Rust repr(C)");

// : + sub-rect UV (4×f32, align 4) tras ref_rgb → 272 (offset alineado) + 16
// = 288; in-betweens: + pose_key (u64 al FINAL, alineado a 288) → 296;
// Vestuario: + mask_path[256] al FINAL → 552 (296 + 256, align 8 intacto).
static_assert(sizeof(AytherSpriteSub)        == 552,
    "ABI: AytherSpriteSub size mismatch with Rust repr(C)");
static_assert(offsetof(AytherSpriteSub, mask_path) == 296,
    "ABI: AytherSpriteSub.mask_path offset mismatch with Rust repr(C)");
static_assert(offsetof(AytherSpriteSub, palette) == 267,
    "ABI: AytherSpriteSub.palette offset mismatch with Rust repr(C)");
static_assert(offsetof(AytherSpriteSub, synth_pal) == 268,
    "ABI: AytherSpriteSub.synth_pal offset mismatch with Rust repr(C)");
static_assert(offsetof(AytherSpriteSub, ref_rgb) == 269,
    "ABI: AytherSpriteSub.ref_rgb offset mismatch with Rust repr(C)");
static_assert(offsetof(AytherSpriteSub, u0) == 272,
    "ABI: AytherSpriteSub.u0 offset mismatch with Rust repr(C)");

static_assert(sizeof(AytherAudioOccurrence)  == 16,
    "ABI: AytherAudioOccurrence size mismatch with Rust repr(C)");

static_assert(sizeof(AytherAudioSub)         == 272,
    "ABI: AytherAudioSub size mismatch with Rust repr(C)");

static_assert(sizeof(AytherShaderParams)     == 12,
    "ABI: AytherShaderParams size mismatch with Rust repr(C)");

static_assert(sizeof(AytherAnimFrame)        == 16,
    "ABI: AytherAnimFrame size mismatch with Rust repr(C)");   // u64(8)+u16(2)→pad→16
static_assert(offsetof(AytherAnimFrame, duration) == 8,
    "ABI: AytherAnimFrame::duration offset mismatch (C-S1)");

static_assert(sizeof(AytherTransform)        == 16,
    "ABI: AytherTransform size mismatch with Rust repr(C)");        // 4×f32

static_assert(sizeof(AytherAudioEvent)       == 32,
    "ABI: AytherAudioEvent size mismatch with Rust repr(C)");  // 2×u64+2×u32+4×u8→32
static_assert(offsetof(AytherAudioEvent, instrument) == 8,
    "ABI: AytherAudioEvent::instrument offset mismatch");
static_assert(offsetof(AytherAudioEvent, start_frame) == 16,
    "ABI: AytherAudioEvent::start_frame offset mismatch");
static_assert(offsetof(AytherAudioEvent, chip) == 24,
    "ABI: AytherAudioEvent::chip offset mismatch");

static_assert(sizeof(AytherAudioEventSub)    == 288,
    "ABI: AytherAudioEventSub size mismatch with Rust repr(C)");  // 3×u64+char[256]+u8→pad→288
static_assert(offsetof(AytherAudioEventSub, asset_path) == 24,
    "ABI: AytherAudioEventSub::asset_path offset mismatch (C-A2)");
static_assert(offsetof(AytherAudioEventSub, looping) == 280,
    "ABI: AytherAudioEventSub::looping offset mismatch (C-A2)");

// Field offset spot-checks (the most error-prone: structs with mixed types).
static_assert(offsetof(AytherSpriteOccurrence, anim_group_id) == 8,
    "ABI: AytherSpriteOccurrence::anim_group_id offset mismatch");
static_assert(offsetof(AytherSpriteOccurrence, screen_x) == 18,
    "ABI: AytherSpriteOccurrence::screen_x offset mismatch");
static_assert(offsetof(AytherSpriteOccurrence, screen_y) == 20,
    "ABI: AytherSpriteOccurrence::screen_y offset mismatch");
static_assert(offsetof(AytherSpriteOccurrence, link) == 22,
    "ABI: AytherSpriteOccurrence::link offset mismatch (R1.5)");
static_assert(offsetof(AytherSpriteOccurrence, palette) == 23,
    "ABI: AytherSpriteOccurrence::palette offset mismatch (R1.5)");
static_assert(offsetof(AytherSpriteOccurrence, priority) == 24,
    "ABI: AytherSpriteOccurrence::priority offset mismatch");
static_assert(offsetof(AytherSpriteOccurrence, slot) == 25,
    "ABI: AytherSpriteOccurrence::slot offset mismatch");
static_assert(offsetof(AytherSpriteOccurrence, hflip) == 26,
    "ABI: AytherSpriteOccurrence::hflip offset mismatch (C-B0)");
static_assert(offsetof(AytherSpriteOccurrence, vflip) == 27,
    "ABI: AytherSpriteOccurrence::vflip offset mismatch (C-B0)");
static_assert(offsetof(AytherSpriteSub, screen_x) == 256,
    "ABI: AytherSpriteSub::screen_x offset mismatch");
static_assert(offsetof(AytherAudioSub, asset_path) == 8,
    "ABI: AytherAudioSub::asset_path offset mismatch");
static_assert(offsetof(AytherAudioSub, frame_count) == 264,
    "ABI: AytherAudioSub::frame_count offset mismatch");

// ---------------------------------------------------------------------------
// Minimal test harness — no external dependencies.
// ---------------------------------------------------------------------------
static int  g_pass     = 0;
static int  g_fail     = 0;
static const char* g_test = "?";

#define BEGIN_TEST(name)  do { g_test = (name); } while (0)

#define EXPECT(expr) \
    do { \
        if (expr) { \
            ++g_pass; \
        } else { \
            std::fprintf(stderr, "  FAIL  %-50s  [%s:%d]\n", \
                         g_test, __FILE__, __LINE__); \
            ++g_fail; \
        } \
    } while (0)

#define EXPECT_EQ(a, b)  EXPECT(static_cast<decltype(b)>(a) == (b))
#define EXPECT_NULL(p)   EXPECT((p) == nullptr)
#define EXPECT_NNULL(p)  EXPECT((p) != nullptr)

// ---------------------------------------------------------------------------
// version
// ---------------------------------------------------------------------------
static void test_version() {
    BEGIN_TEST("version/release_contract");
    EXPECT(std::strcmp(ayther_engine_version(), AYTHER_VERSION_STRING) == 0);
    EXPECT(std::strcmp(ayther_engine_version(), AYTHER_CMAKE_PROJECT_VERSION) == 0);

    BEGIN_TEST("version/independent_protocol_revisions");
    EXPECT_EQ(ayther_core_version(), AYTHER_CORE_C_ABI_REVISION);
    EXPECT_EQ(ayther_manifest_schema_supported(), AYTHER_PACK_MANIFEST_SCHEMA);
    EXPECT_EQ(ayther_pack_format_supported(), AYTHER_PACK_FORMAT);
}

// ---------------------------------------------------------------------------
// TileHasher
// ---------------------------------------------------------------------------
static void test_tile_hasher() {
    BEGIN_TEST("tile_hasher/new");
    AytherTileHasher* h = ayther_tile_hasher_new();
    EXPECT_NNULL(h);
    if (!h) return;

    BEGIN_TEST("tile_hasher/unique_count_starts_zero");
    EXPECT_EQ(ayther_tile_hasher_unique_count(h), 0u);

    // Solid-black 320×240 RGB565: all 1200 tiles identical → at most 1 unique.
    BEGIN_TEST("tile_hasher/process_black_frame");
    static uint8_t pixels[320 * 240 * 2];   // zero-init via static
    uint32_t new_tiles = ayther_tile_hasher_process_frame(
        h, pixels, 320, 240, 320 * 2, 2 /*RETRO_PIXEL_FORMAT_RGB565*/);
    EXPECT(new_tiles <= 1u);

    BEGIN_TEST("tile_hasher/unique_count_le_one_after_black_frame");
    EXPECT(ayther_tile_hasher_unique_count(h) <= 1u);

    // Second identical frame → 0 new tiles.
    BEGIN_TEST("tile_hasher/second_identical_frame_no_new_tiles");
    EXPECT_EQ(ayther_tile_hasher_process_frame(
        h, pixels, 320, 240, 320 * 2, 2), 0u);

    BEGIN_TEST("tile_hasher/free");
    ayther_tile_hasher_free(h);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// TileSubstitutor
// ---------------------------------------------------------------------------
static void test_tile_substitutor() {
    BEGIN_TEST("tile_sub/new");
    AytherTileSubstitutor* s = ayther_tile_sub_new();
    EXPECT_NNULL(s);
    if (!s) return;

    BEGIN_TEST("tile_sub/resolve_empty_occurrences");
    AytherTileSub out[8];
    EXPECT_EQ(ayther_tile_sub_resolve(s, nullptr, 0, out, 8), 0u);

    BEGIN_TEST("tile_sub/add_override_does_not_crash");
    ayther_tile_sub_add_override(s, 0xDEADBEEFCAFEBABEULL, "graphics/tile.png");
    EXPECT(true);

    BEGIN_TEST("tile_sub/clear_overrides_does_not_crash");
    ayther_tile_sub_clear_overrides(s);
    EXPECT(true);

    BEGIN_TEST("tile_sub/free");
    ayther_tile_sub_free(s);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// SpriteHasher
// ---------------------------------------------------------------------------
static void test_sprite_hasher() {
    BEGIN_TEST("sprite_hasher/new");
    AytherSpriteHasher* h = ayther_sprite_hasher_new();
    EXPECT_NNULL(h);
    if (!h) return;

    BEGIN_TEST("sprite_hasher/unique_count_starts_zero");
    EXPECT_EQ(ayther_sprite_hasher_unique_count(h), 0u);

    // All-zero VRAM: no meaningful sprite patterns.
    BEGIN_TEST("sprite_hasher/process_empty_vram");
    static uint8_t vram[64 * 1024];
    ayther_sprite_hasher_process_vram(h, vram, sizeof(vram), 0xD800);
    EXPECT(true);

    BEGIN_TEST("sprite_hasher/get_occurrences_does_not_overflow");
    AytherSpriteOccurrence occs[256];
    uint32_t n = ayther_sprite_hasher_get_occurrences(h, occs, 256);
    EXPECT(n <= 80u);   // Mega Drive SAT max = 80 sprites

    BEGIN_TEST("sprite_hasher/free");
    ayther_sprite_hasher_free(h);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// SpriteSubstitutor
// ---------------------------------------------------------------------------
static void test_sprite_substitutor() {
    BEGIN_TEST("sprite_sub/new");
    AytherSpriteSubstitutor* s = ayther_sprite_sub_new();
    EXPECT_NNULL(s);
    if (!s) return;

    BEGIN_TEST("sprite_sub/resolve_empty_occurrences");
    AytherSpriteSub out[8];
    EXPECT_EQ(ayther_sprite_sub_resolve(s, nullptr, 0, out, 8), 0u);

    BEGIN_TEST("sprite_sub/free");
    ayther_sprite_sub_free(s);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// AudioHasher
// ---------------------------------------------------------------------------
static void test_audio_hasher() {
    BEGIN_TEST("audio_hasher/new");
    AytherAudioHasher* h = ayther_audio_hasher_new();
    EXPECT_NNULL(h);
    if (!h) return;

    BEGIN_TEST("audio_hasher/unique_count_starts_zero");
    EXPECT_EQ(ayther_audio_hasher_unique_count(h), 0u);

    // Silent batch → hash must be 0 (skipped by design).
    BEGIN_TEST("audio_hasher/silent_batch_returns_zero");
    static int16_t silence[512];   // zero-init via static
    EXPECT_EQ(ayther_audio_hasher_process_batch(h, silence, 256), 0ULL);

    // Non-silent batch → non-zero hash.
    BEGIN_TEST("audio_hasher/nonsilent_batch_returns_nonzero");
    int16_t noise[16];
    for (int i = 0; i < 16; ++i) noise[i] = static_cast<int16_t>(i * 1000);
    EXPECT(ayther_audio_hasher_process_batch(h, noise, 8) != 0ULL);

    BEGIN_TEST("audio_hasher/end_tick");
    ayther_audio_hasher_end_tick(h);
    EXPECT(true);

    BEGIN_TEST("audio_hasher/free");
    ayther_audio_hasher_free(h);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// AudioSubstitutor
// ---------------------------------------------------------------------------
static void test_audio_substitutor() {
    BEGIN_TEST("audio_sub/new");
    AytherAudioSubstitutor* s = ayther_audio_sub_new();
    EXPECT_NNULL(s);
    if (!s) return;

    BEGIN_TEST("audio_sub/catalog_len_starts_zero");
    EXPECT_EQ(ayther_audio_sub_catalog_len(s), 0u);

    BEGIN_TEST("audio_sub/free");
    ayther_audio_sub_free(s);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// ScriptEnv
// ---------------------------------------------------------------------------
static void test_script_env() {
    BEGIN_TEST("script/new");
    AytherScriptEnv* env = ayther_script_new();
    EXPECT_NNULL(env);
    if (!env) return;

    BEGIN_TEST("script/set_pack_null_safe");
    ayther_script_set_pack(env, nullptr);
    EXPECT(true);

    BEGIN_TEST("script/load_valid_lua");
    EXPECT(ayther_script_load_string(env, "x = 1 + 1", "test_chunk"));

    BEGIN_TEST("script/load_invalid_lua_returns_false");
    EXPECT(!ayther_script_load_string(env, ")(not valid lua", "bad_chunk"));

    BEGIN_TEST("script/on_frame_no_callbacks_returns_zero");
    static uint8_t ram[0x10000];
    EXPECT_EQ(ayther_script_on_frame(env, ram, sizeof(ram)), 0u);

    // Defaults: crt_strength=0.0 (off), scan_strength=0.5, vignette=0.2
    // Keep in sync with script_env.rs shader._params initialisation.
    BEGIN_TEST("script/get_shader_params_defaults");
    AytherShaderParams sp{};
    ayther_script_get_shader_params(env, &sp);
    EXPECT(sp.crt_strength  == 0.0f);
    EXPECT(sp.scan_strength == 0.5f);
    EXPECT(sp.vignette      == 0.2f);

    BEGIN_TEST("script/update_tiles_empty_does_not_crash");
    ayther_script_update_tiles(env, nullptr, 0);
    EXPECT(true);

    BEGIN_TEST("script/update_sprites_empty_does_not_crash");
    ayther_script_update_sprites(env, nullptr, 0);
    EXPECT(true);

    BEGIN_TEST("script/update_audio_empty_does_not_crash");
    ayther_script_update_audio(env, nullptr, 0);
    EXPECT(true);

    BEGIN_TEST("script/free");
    ayther_script_free(env);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// PackWatcher  (v0.9.5)
// ---------------------------------------------------------------------------
static void test_pack_watcher() {
    BEGIN_TEST("watcher/poll_null_returns_false");
    EXPECT(!ayther_pack_watcher_poll(nullptr));

    BEGIN_TEST("watcher/nonexistent_parent_no_crash");
    // Deep nonexistent path — most platforms return null, some succeed lazily.
    AytherPackWatcher* w = ayther_pack_watcher_new(
        "/nonexistent_ayther_test_dir_xyz/pack.ay");
    if (w) ayther_pack_watcher_free(w);
    EXPECT(true);   // reaching here = no crash / UB

    BEGIN_TEST("watcher/free_null_safe");
    ayther_pack_watcher_free(nullptr);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// pack builder (R8) — build + sign a .ay, then open + read it back
// ---------------------------------------------------------------------------
static void test_pack_builder() {
    ayther::test::TrustedPackFixture fixture{"ffi"};

    BEGIN_TEST("pack_builder/new");
    AytherPackBuilder* b = fixture.builder();
    EXPECT_NNULL(b);

    BEGIN_TEST("pack_builder/reject_signature_entry");
    const uint8_t one[1] = { 1 };
    EXPECT(!ayther_pack_builder_add_bytes(b, "signature.bin", one, 1));

    BEGIN_TEST("pack_builder/add_manifest_and_sub");
    const char* manifest =
        "[pack]\nname = \"FFI Pack\"\nversion = \"1.0.0\"\n"
        "game_id = \"sonic2\"\nayther_min = \"0.8.0\"\n"
        "[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";
    EXPECT(ayther_pack_builder_add_bytes(b, "manifest.toml",
        reinterpret_cast<const uint8_t*>(manifest), std::strlen(manifest)));
    const char* sub =
        "[[sub]]\nhash = \"0x00000000000000ab\"\nasset = \"hero.png\"\n";
    EXPECT(ayther_pack_builder_add_bytes(b, "sprite_substitutions.toml",
        reinterpret_cast<const uint8_t*>(sub), std::strlen(sub)));

    BEGIN_TEST("pack_builder/add_asset_bytes");
    const uint8_t png[8] = { 0x89, 'P', 'N', 'G', 1, 2, 3, 4 };
    EXPECT(ayther_pack_builder_add_bytes(b, "hero.png", png, sizeof(png)));

    // : un `.ivf` de 70000 bytes — pasa los 64 KiB del trozo minimo, asi
    // que ejercita VARIOS trozos y el ultimo corto. No es un video valido y no
    // hace falta que lo sea: aca se prueba el transporte por rango, no el codec.
    BEGIN_TEST("pack_builder/add_video_bytes");
    std::vector<uint8_t> clip(70000);
    for (size_t i = 0; i < clip.size(); ++i) clip[i] = uint8_t(i % 251);
    EXPECT(ayther_pack_builder_add_bytes(b, "video/clip.ivf",
                                        clip.data(), clip.size()));

    BEGIN_TEST("pack_builder/count");
    EXPECT_EQ(ayther_pack_builder_count(b), 4u);   // manifest + sub + png + ivf

    BEGIN_TEST("pack_builder/finish_signed");
    char err[256] = {};
    const bool ok = fixture.finish(err, sizeof(err));
    if (!ok) std::fprintf(stderr, "    finish error: %s\n", err);
    EXPECT(ok);

    BEGIN_TEST("pack_builder/opens_and_verifies");
    AyArchive* pack = fixture.open();
    EXPECT_NNULL(pack);

    if (pack) {
        BEGIN_TEST("pack_builder/game_id_round_trips");
        EXPECT(std::strcmp(ayther_pack_game_id(pack), "sonic2") == 0);

        BEGIN_TEST("pack_builder/asset_round_trips");
        uint8_t buf[8] = {};
        const int64_t n = ayther_pack_read(pack, "hero.png", buf, sizeof(buf));
        EXPECT_EQ(n, (int64_t)sizeof(png));
        EXPECT(std::memcmp(buf, png, sizeof(png)) == 0);

        // ---- : lectura por rango --------------------------------------
        BEGIN_TEST("pack_range/streamable_only_for_stored");
        EXPECT(ayther_pack_entry_streamable(pack, "video/clip.ivf"));
        EXPECT(!ayther_pack_entry_streamable(pack, "hero.png"));   // deflateado
        EXPECT(!ayther_pack_entry_streamable(pack, "no/existe.ivf"));
        EXPECT(!ayther_pack_entry_streamable(nullptr, "video/clip.ivf"));

        BEGIN_TEST("pack_range/reads_match_the_content");
        // Incluye el borde de trozo (65536), que es donde se veria un off-by-one.
        const uint64_t offs[] = { 0, 12, 65530, 65536, 69999 };
        const size_t   lens[] = { 32, 187, 12,    64,    1     };
        for (size_t k = 0; k < 5; ++k) {
            std::vector<uint8_t> got(lens[k], 0);
            const int64_t r = ayther_pack_read_range(pack, "video/clip.ivf",
                                                    offs[k], got.data(), lens[k]);
            EXPECT_EQ(r, (int64_t)lens[k]);
            EXPECT(std::memcmp(got.data(), clip.data() + offs[k], lens[k]) == 0);
        }

        BEGIN_TEST("pack_range/tail_is_clipped_not_refused");
        // Un pedido que se pasa del final devuelve lo que hay: asi un barrido
        // secuencial no necesita saber el tamano exacto de antemano.
        std::vector<uint8_t> tail(4096, 0);
        EXPECT_EQ(ayther_pack_read_range(pack, "video/clip.ivf", 69990,
                                         tail.data(), tail.size()), (int64_t)10);

        BEGIN_TEST("pack_range/refuses_what_it_cannot_verify");
        uint8_t one_byte = 0;
        // Fuera de rango, entrada deflateada, y pack nulo: -1, no datos.
        EXPECT_EQ(ayther_pack_read_range(pack, "video/clip.ivf", 70000,
                                         &one_byte, 1), (int64_t)-1);
        EXPECT_EQ(ayther_pack_read_range(pack, "hero.png", 0,
                                         &one_byte, 1), (int64_t)-1);
        EXPECT_EQ(ayther_pack_read_range(nullptr, "video/clip.ivf", 0,
                                         &one_byte, 1), (int64_t)-1);

    }

    BEGIN_TEST("pack_builder/free_null_safe");
    ayther_pack_builder_free(nullptr);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// Audio event substitution (C-A2) — signature → asset over event ranges
// ---------------------------------------------------------------------------
static void test_audio_event_sub() {
    AytherAudioSubstitutor* s = ayther_audio_sub_new();

    BEGIN_TEST("audio_event_sub/resolve_by_signature");
    EXPECT_NNULL(s);
    // add_event_override → the runtime override map (event_catalog_len counts the
    // separate TOML catalog, so it stays 0); resolve checks overrides first.
    ayther_audio_sub_add_event_override(s, 0xABCDull, "audio/music/theme.ogg", 1);

    // Two detected events: one assigned (0xABCD), one not (0x9999).
    AytherAudioEvent evs[2] = {
        { /*sig*/ 0xABCDull, /*inst*/ 0, /*start*/ 100, /*end*/ 220, /*chip*/ 0,
          /*ch*/ 0, /*pitch*/ 255, 0 },
        { /*sig*/ 0x9999ull, /*inst*/ 0, /*start*/ 300, /*end*/ 305, /*chip*/ 0,
          /*ch*/ 1, /*pitch*/ 255, 0 },
    };
    AytherAudioEventSub out[4];
    const uint32_t n = ayther_audio_sub_resolve_events(s, evs, 2, out, 4);
    EXPECT_EQ(n, 1u);                                   // only the assigned event
    if (n == 1) {
        EXPECT_EQ(out[0].signature, 0xABCDull);
        EXPECT_EQ(out[0].start_frame, 100ull);
        EXPECT_EQ(out[0].end_frame, 220ull);
        EXPECT_EQ(out[0].looping, (uint8_t)1);
        EXPECT(std::strcmp(out[0].asset_path, "audio/music/theme.ogg") == 0);
    }

    BEGIN_TEST("audio_event_sub/clear");
    ayther_audio_sub_clear_event_overrides(s);
    EXPECT_EQ(ayther_audio_sub_resolve_events(s, evs, 2, out, 4), 0u);   // nothing resolves now

    ayther_audio_sub_free(s);
}

// ---------------------------------------------------------------------------
// Game profile — Mode 3 anchor plumbing (kinds)
// ---------------------------------------------------------------------------
static void test_game_profile() {
    namespace fs = std::filesystem;
    const fs::path toml = fs::temp_directory_path() / "ayther_test_profile.toml";
    {
        std::ofstream f(toml, std::ios::binary);
        BEGIN_TEST("game_profile/write_temp_toml");
        EXPECT(f.good());
        f << "word_swap = true\n"
          << "[[anchor]]\nname=\"player\"\nbase=0xB000\nx_off=0x08\ny_off=0x0C\n"
          << "count=2\nstride=0x40\npivot=[0,0]\nhalf=[40,40]\n";
    }

    BEGIN_TEST("game_profile/load_null_on_bad_path");
    EXPECT_NULL(ayther_game_profile_load("/no/such/profile.toml"));

    BEGIN_TEST("game_profile/kinds");
    AytherGameProfile* p = ayther_game_profile_load(toml.string().c_str());
    EXPECT_NNULL(p);
    if (p) {
        EXPECT_EQ(ayther_game_profile_kind_count(p), (size_t)1);
        EXPECT_EQ(ayther_game_profile_kind_of_id(p, 0xB000), 0);   // Sonic (base)
        EXPECT_EQ(ayther_game_profile_kind_of_id(p, 0xB040), 0);   // Tails (base+stride)
        EXPECT_EQ(ayther_game_profile_kind_of_id(p, 0xB020), -1);  // between → none
        char buf[64] = {0};
        const size_t len = ayther_game_profile_kind_name(p, 0, buf, sizeof(buf));
        EXPECT_EQ(len, (size_t)6);
        EXPECT(std::strcmp(buf, "player") == 0);
        ayther_game_profile_free(p);
    }

    BEGIN_TEST("game_profile/free_null_safe");
    ayther_game_profile_free(nullptr);
    EXPECT(true);

    std::error_code ec;
    fs::remove(toml, ec);
}

// ---------------------------------------------------------------------------
// Geometric tween (C-S1 Level 1)
// ---------------------------------------------------------------------------
static void test_geo_tween() {
    // Two keyframes: x=0 held 4 ticks, x=8 held 1; one-shot.
    AytherTransform tf[2] = { { 0, 0, 10, 10 }, { 8, 0, 10, 10 } };
    uint16_t durs[2] = { 4, 1 };

    BEGIN_TEST("geo_tween/new_null_args");
    EXPECT_NULL(ayther_geo_tween_new(nullptr, durs, 2, false));
    EXPECT_NULL(ayther_geo_tween_new(tf, durs, 0, false));

    BEGIN_TEST("geo_tween/sample_interpolates");
    AytherGeometricTween* t = ayther_geo_tween_new(tf, durs, 2, false);
    EXPECT_NNULL(t);
    EXPECT_EQ(ayther_geo_tween_duration(t), 5u);
    EXPECT_EQ(ayther_geo_tween_sample(t, 0).x, 0.0f);
    EXPECT_EQ(ayther_geo_tween_sample(t, 2).x, 4.0f);   // half of the 4-tick hold
    EXPECT_EQ(ayther_geo_tween_sample(t, 99).x, 8.0f);  // one-shot clamps to last
    ayther_geo_tween_free(t);

    BEGIN_TEST("geo_tween/free_null_safe");
    ayther_geo_tween_free(nullptr);
    EXPECT(true);
}

// ---------------------------------------------------------------------------
// SoundFont: normalización .sfz → SF2 a través del FFI ( ampliado)
//
// El camino que el Lab usa de verdad: un .sfz con su sample al lado entra por
// ayther_soundfont_normalize_file y el resultado tiene que LISTAR un preset,
// CARGAR en el sintetizador y SONAR — cargar sin sonar es el modo de fallo
// silencioso de todo lo que arma SF2 a mano. (El decode Vorbis del SF3 se
// cubre en los tests Rust del core; acá se cruza la frontera C.)
// ---------------------------------------------------------------------------
static void test_soundfont_normalize() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ayther_ffi_sfz";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // WAV S16 mono 44100 con 0,25 s de tono.
    {
        const uint32_t frames = 11025, data_sz = frames * 2;
        const uint32_t riff_sz = 36u + data_sz;
        uint8_t hdr[44] = {
            'R','I','F','F', 0,0,0,0, 'W','A','V','E',
            'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
            0x44,0xAC,0,0, 0x88,0x58,0x01,0, 2,0, 16,0,
            'd','a','t','a', 0,0,0,0 };
        std::memcpy(hdr + 4,  &riff_sz, 4);
        std::memcpy(hdr + 40, &data_sz, 4);
        std::ofstream w(dir / "tono.wav", std::ios::binary);
        w.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
        for (uint32_t i = 0; i < frames; ++i) {
            const int16_t v = static_cast<int16_t>(((i / 50) % 2) ? 12000 : -12000);
            w.write(reinterpret_cast<const char*>(&v), 2);
        }
    }
    const fs::path sfz = dir / "ffi_inst.sfz";
    {
        std::ofstream s(sfz);
        s << "<region> sample=tono.wav lokey=0 hikey=127 pitch_keycenter=60\n";
    }
    const std::string sfz_utf8 = sfz.string();

    BEGIN_TEST("soundfont/normalize_two_call");
    const size_t need =
        ayther_soundfont_normalize_file(sfz_utf8.c_str(), nullptr, 0);
    EXPECT(need > 0);
    std::vector<uint8_t> sf2(need ? need : 1);
    EXPECT_EQ(ayther_soundfont_normalize_file(sfz_utf8.c_str(), sf2.data(),
                                              sf2.size()), need);

    BEGIN_TEST("soundfont/normalized_lists_one_preset");
    uint16_t bank = 99, preset = 99;
    EXPECT_EQ(ayther_sf2_list_presets(sf2.data(), sf2.size(), &bank, &preset, 1), 1u);
    EXPECT(bank == 0 && preset == 0);

    BEGIN_TEST("soundfont/preset_list_by_path");
    uint8_t names[256] = {};
    const size_t nb = ayther_sf2_preset_list(sfz_utf8.c_str(), names, sizeof(names));
    EXPECT(nb > 0 && std::strstr(reinterpret_cast<char*>(names),
                                 "0:0|ffi_inst") != nullptr);

    BEGIN_TEST("soundfont/normalized_synth_produces_signal");
    AytherSf2* sy = ayther_sf2_new(sf2.data(), sf2.size(), 44100);
    EXPECT_NNULL(sy);
    if (sy) {
        ayther_sf2_note_on(sy, 0, 60, 100);
        std::vector<float> buf(4096 * 2, 0.0f);
        ayther_sf2_render(sy, buf.data(), 4096);
        float peak = 0.0f;
        for (float v : buf) { const float a = v < 0.0f ? -v : v; if (a > peak) peak = a; }
        EXPECT(peak > 1e-3f);
        ayther_sf2_free(sy);
    }

    BEGIN_TEST("soundfont/normalize_missing_file_is_zero");
    EXPECT_EQ(ayther_soundfont_normalize_file("no_existe.sfz", nullptr, 0),
              size_t{0});

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// / — la lista de subsistemas, que existe DOS veces
// ---------------------------------------------------------------------------
//
// El core la publica por FFI (es la que viaja en el `[systems]` del manifest) y
// el Engine tiene el enum `ayther::Subsystem`, que es con lo que se enciende y
// se apaga cada sustitución. Los índices son el contrato entre las dos.
//
// Este test es lo único que ata las dos listas. Sin él se desincronizan en
// silencio —agregar un subsistema en el medio de una y no de la otra corre
// todos los índices siguientes— y el síntoma no sería un error sino algo peor:
// apagar «música» y que se vaya la interfaz.
//
// No se puede incluir ayther_session.h acá (este test enlaza sólo contra
// ayther_core, sin SDL ni Vulkan), así que la lista del Engine se replica como
// literales. Es a propósito: si alguien cambia el enum y no toca esto, el test
// falla — que es exactamente lo que se quiere.
static void test_subsystem_list_parity() {
    static const char* kEngineOrder[] = {
        "sprites", "metasprites", "tiles", "planes",
        "ui", "music", "sfx", "shaders",
    };
    constexpr uint32_t kEngineCount =
        static_cast<uint32_t>(sizeof(kEngineOrder) / sizeof(kEngineOrder[0]));

    BEGIN_TEST("subsystems/count matches the Engine enum");
    EXPECT_EQ(ayther_subsystem_count(), kEngineCount);

    for (uint32_t i = 0; i < kEngineCount; ++i) {
        BEGIN_TEST("subsystems/name matches at the same index");
        const char* n = ayther_subsystem_name(i);
        EXPECT_NNULL(n);
        if (n) EXPECT(std::strcmp(n, kEngineOrder[i]) == 0);
    }

    BEGIN_TEST("subsystems/out of range is NULL, not garbage");
    EXPECT_NULL(ayther_subsystem_name(kEngineCount));

    BEGIN_TEST("manifest/schema supported is >= 2 ()");
    EXPECT(ayther_manifest_schema_supported() >= 2u);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::fprintf(stdout, "=== ayther_core FFI integration tests ===\n\n");
    std::fprintf(stdout, "  (ABI static_asserts: passed at compile time)\n\n");

    test_version();
    test_tile_hasher();
    test_tile_substitutor();
    test_sprite_hasher();
    test_sprite_substitutor();
    test_audio_hasher();
    test_audio_substitutor();
    test_script_env();
    test_pack_watcher();
    test_pack_builder();
    test_audio_event_sub();
    test_game_profile();
    test_geo_tween();
    test_soundfont_normalize();
    test_subsystem_list_parity();

    const int total = g_pass + g_fail;
    std::fprintf(stdout,
        "\n=== Results: %d/%d passed%s ===\n",
        g_pass, total,
        g_fail > 0 ? " — FAILURES ABOVE" : " — all OK");

    return g_fail > 0 ? 1 : 0;
}
