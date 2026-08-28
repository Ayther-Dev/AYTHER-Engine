// ---------------------------------------------------------------------------
// ayther_recording_test.cpp — round-trip de .arp (save/load) + slice().
//
// Cubre la CAPA DE DATOS de la que dependen los features recording-céntricos:
//   · v7: historia de hashes de AUDIO por frame (CSR) → audio-por-sonido +
//         la vista previa de audio (preview_audio escanea audio_hashes/offsets).
//   · v3: historia de hashes de SPRITE por frame (CSR).
//   · v5/v6: cobertura de planos A/B/Window en los FrameStat.
//   · slice(): que la sub-toma rebase ambos CSR correctamente.
//
// No depende de SDL/Vulkan/ROM: compila ayther_recording.cpp + zstd. Hand-rolled
// (mismo estilo que ayther_core_ffi_test.cpp). ctest -R ayther_recording.
// ---------------------------------------------------------------------------
#include "ayther_recording.h"

#include <cstdio>
#include <filesystem>
#include <vector>

using ayther::AytherRecording;
using ayther::FrameStat;

static int g_fail = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", (msg)); ++g_fail; } } while (0)

// Una toma de 3 frames con stats de planos + ambos CSR (sprite v3 + audio v7).
static AytherRecording make_rec() {
    AytherRecording r;
    r.game_id       = "crc32:test";
    r.name          = "take_test";
    r.initial_state = { 0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04 };
    r.inputs        = { 0x0001, 0x0002, 0x0003 };           // frame_count = 3
    r.stats = {
        { 2, 5, 1, 10, 20, 3 },   // frame 0: sprites,tiles,audio,plane_a,plane_b,plane_w
        { 1, 4, 0, 11, 21, 3 },   // frame 1
        { 0, 4, 2, 12, 22, 3 },   // frame 2
    };
    // Sprite CSR (v3): frame0=2 hashes, frame1=1, frame2=1 → offsets {0,2,3,4}.
    r.sprite_hashes = { 0x1111, 0x2222, 0x3333, 0x4444 };
    r.hash_offsets  = { 0, 2, 3, 4 };
    // Audio CSR (v7): frame0=1, frame1=0, frame2=2 → offsets {0,1,1,3}.
    r.audio_hashes  = { 0xA1A1, 0xA2A2, 0xA3A3 };
    r.audio_offsets = { 0, 1, 1, 3 };
    r.trim_in  = 0;
    r.trim_out = 3;
    return r;
}

int main() {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "ayther_rec_test.arp";

    const AytherRecording a = make_rec();
    CHECK(a.save(path.string()), "save() debe tener éxito");

    const auto loaded = AytherRecording::load(path.string());
    CHECK(loaded.has_value(), "load() debe devolver una toma");

    if (loaded) {
        const AytherRecording& b = *loaded;
        CHECK(b.game_id == a.game_id, "game_id preservado");
        CHECK(b.inputs == a.inputs, "inputs preservados");
        CHECK(b.frame_count() == 3, "frame_count == 3");

        // Stats de planos (v5/v6).
        CHECK(b.stats.size() == 3, "stats: 3 frames");
        if (b.stats.size() == 3) {
            CHECK(b.stats[0].audio == 1 && b.stats[2].audio == 2, "stats.audio preservado");
            CHECK(b.stats[0].plane_a == 10 && b.stats[2].plane_a == 12, "stats.plane_a (v5)");
            CHECK(b.stats[0].plane_b == 20 && b.stats[1].plane_b == 21, "stats.plane_b (v5)");
            CHECK(b.stats[0].plane_w == 3 && b.stats[2].plane_w == 3,  "stats.plane_w (v6)");
        }

        // Sprite CSR (v3).
        CHECK(b.sprite_hashes == a.sprite_hashes, "sprite_hashes preservados");
        CHECK(b.hash_offsets == a.hash_offsets,   "hash_offsets (CSR sprite) preservados");
        CHECK(b.present(0, 0x1111) && b.present(0, 0x2222), "present(): sprites del frame 0");
        CHECK(!b.present(1, 0x1111), "present(): 0x1111 no está en el frame 1");
        CHECK(b.present(2, 0x4444),  "present(): 0x4444 en el frame 2");

        // Audio CSR (v7) — lo que escanea preview_audio para ubicar el sonido.
        CHECK(b.audio_hashes == a.audio_hashes,   "audio_hashes (v7) preservados");
        CHECK(b.audio_offsets == a.audio_offsets, "audio_offsets (CSR audio) preservados");
        CHECK(b.audio_offsets.size() == b.frame_count() + 1, "audio_offsets: frame_count+1");
        CHECK(b.audio_offsets.back() == b.audio_hashes.size(), "audio CSR cierra en total_hashes");
    }

    // slice([1,3)) — la sub-toma rebasa inputs + ambos CSR a 0.
    {
        const AytherRecording s = a.slice(1, 3, a.initial_state);
        CHECK(s.frame_count() == 2, "slice: 2 frames");
        CHECK((s.inputs == std::vector<uint16_t>{ 0x0002, 0x0003 }), "slice: inputs rebasados");
        // Audio del rango [1,3): frame1 vacío, frame2 = {0xA2A2,0xA3A3}.
        CHECK((s.audio_hashes == std::vector<uint64_t>{ 0xA2A2, 0xA3A3 }), "slice: audio_hashes del rango");
        CHECK((s.audio_offsets == std::vector<uint32_t>{ 0, 0, 2 }), "slice: audio CSR rebasado");
        CHECK((s.sprite_hashes == std::vector<uint64_t>{ 0x3333, 0x4444 }), "slice: sprite_hashes del rango");
        CHECK((s.hash_offsets == std::vector<uint32_t>{ 0, 1, 2 }), "slice: sprite CSR rebasado");
    }

    // Toma sin audio CSR (formato viejo): load no debe romperse, audio vacío.
    {
        AytherRecording noaud = make_rec();
        noaud.audio_hashes.clear();
        noaud.audio_offsets.clear();
        const fs::path p2 = fs::temp_directory_path() / "ayther_rec_noaudio.arp";
        CHECK(noaud.save(p2.string()), "save() sin audio CSR");
        const auto l2 = AytherRecording::load(p2.string());
        CHECK(l2.has_value(), "load() sin audio CSR");
        // Sin audio → no hay audio_hashes (los offsets pueden quedar en ceros,
        // misma convención que el CSR de sprites; lo que importa: sin datos).
        if (l2) CHECK(l2->audio_hashes.empty(), "sin audio CSR → audio_hashes vacío tras load");
        std::error_code ec; fs::remove(p2, ec);
    }

    // patch_name(): renombrar la toma en disco sin recomprimir — el nombre
    // visible (UTF-8, acentos incluidos) cambia y TODO el resto queda intacto.
    {
        CHECK(AytherRecording::patch_name(path.string(), "Men\xC3\xBA nuevo"),
              "patch_name: nombre más largo (con acento)");
        const auto p1 = AytherRecording::load(path.string());
        CHECK(p1.has_value(), "load() tras patch_name");
        if (p1) {
            CHECK(p1->name == "Men\xC3\xBA nuevo", "patch_name: name UTF-8 preservado");
            CHECK(p1->game_id == a.game_id, "patch_name: game_id intacto");
            CHECK(p1->inputs == a.inputs, "patch_name: inputs intactos");
            CHECK(p1->audio_hashes == a.audio_hashes, "patch_name: audio CSR intacto");
            CHECK(p1->sprite_hashes == a.sprite_hashes, "patch_name: sprite CSR intacto");
        }
        CHECK(AytherRecording::patch_name(path.string(), "x"),
              "patch_name: nombre más corto");
        const auto p2 = AytherRecording::load(path.string());
        CHECK(p2 && p2->name == "x" && p2->inputs == a.inputs,
              "patch_name: acortar no corrompe el resto");
        CHECK(AytherRecording::patch_name(path.string(), "x"),
              "patch_name: mismo nombre = no-op exitoso");
        CHECK(!AytherRecording::patch_name(
                  (fs::temp_directory_path() / "no_existe.arp").string(), "y"),
              "patch_name: archivo inexistente → false");
    }

    std::error_code ec; fs::remove(path, ec);

    if (g_fail) { std::fprintf(stderr, "%d checks fallaron\n", g_fail); return 1; }
    std::printf("ayther_recording_test: OK\n");
    return 0;
}
