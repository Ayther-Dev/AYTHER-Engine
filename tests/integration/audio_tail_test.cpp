// ---------------------------------------------------------------------------
// audio_tail_test — [start,end] + tail gobiernan la vida audible del HD (#386).
//
// POR QUÉ EXISTE. La ventana de una sustitución tenía end_frame pero el
// non-loop drenaba el asset ENTERO después — el original volvía (el mute es
// por rango) con el HD todavía sonando, los dos a la vez. El contrato nuevo:
// `cut_frame` (end + tail de la política) destruye el stream aunque tenga PCM
// encolado, loops incluidos (un loop nunca queda libre). tail AUSENTE en el
// TOML = ilimitado, el legacy explícito — cambiarle el sonido a los packs ya
// horneados sería una regresión audible.
//
// Player con driver dummy + un pack REAL en miniatura (builder FFI del core):
// play_event_hd decodifica del pack, así que el pack es parte del arnés.
// ---------------------------------------------------------------------------
#include "audio_player.h"
#include "ayther_components_toml.h"
#include "ayther_core_ffi.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// WAV S16 estéreo 44100 con `frames` cuadros de audio determinista, en memoria.
// La señal varía para que el fixture no parezca una bomba ZIP ante el límite
// de relación de compresión del pack.
std::vector<uint8_t> fixture_wav_bytes(uint32_t frames) {
    const uint32_t data_sz = frames * 4;
    const uint32_t riff_sz = 36u + data_sz;
    uint8_t hdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xAC,0,0, 0x10,0xB1,0x02,0, 4,0, 16,0,
        'd','a','t','a', 0,0,0,0 };
    std::memcpy(hdr + 4,  &riff_sz, 4);
    std::memcpy(hdr + 40, &data_sz, 4);
    std::vector<uint8_t> out(sizeof(hdr) + data_sz);
    std::memcpy(out.data(), hdr, sizeof(hdr));
    std::vector<int16_t> pcm(frames * 2);
    uint32_t state = 0x6d2b79f5u;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        state = state * 1664525u + 1013904223u;
        const auto sample = static_cast<int16_t>(
            static_cast<int32_t>(state >> 17) - 16384);
        pcm[frame * 2] = sample;
        pcm[frame * 2 + 1] = sample;
    }
    std::memcpy(out.data() + sizeof(hdr), pcm.data(), data_sz);
    return out;
}

}  // namespace

int main() {
    std::printf("=== audio_tail_test — la ventana corta el HD (#386) ===\n");

    // ---- Parser de tail_frames (transporte tipo `members`) -----------------
    {
        const std::string legacy =
            "[[event]]\nsignature = \"0x00000000000000aa\"\nasset = \"a.wav\"\n";
        check(ayther::parse_audio_event_tails(legacy).empty(),
              "TOML legacy sin tail_frames = mapa vacío (ilimitado)");
        const std::string updated_toml =
            "[[event]]\nsignature = \"0x00000000000000aa\"\nasset = \"a.wav\"\n"
            "tail_frames = 0\n"
            "[[event]]\nsignature = \"0x00000000000000bb\"\nasset = \"b.wav\"\n"
            "tail_frames = 30\n"
            "[[event]]\nsignature = \"0x00000000000000cc\"\nasset = \"c.wav\"\n"
            "tail_frames = -5\n";
        const auto tails = ayther::parse_audio_event_tails(updated_toml);
        check(tails.size() == 2, "tail_frames parsea (y el negativo se ignora)");
        check(tails.count(0xaa) && tails.at(0xaa) == 0,  "tail 0 = corte exacto");
        check(tails.count(0xbb) && tails.at(0xbb) == 30, "tail 30 = cola acotada");
        check(!tails.count(0xcc), "tail invalido queda ilimitado (legacy)");
    }

    // ---- #511: parser de `head` (mismo transporte que `members`) ----------
    {
        const std::string t =
            "[[event]]\nsignature = \"0x00000000000000aa\"\nasset = \"a.wav\"\n"
            "duration = 576\nloop = true\n"
            "members = [\"0x00000000000000aa\", \"0x00000000000000bb\"]\n"
            "head = [\"0x00000000000000aa\", \"0x00000000000000bb\", \"0x00000000000000cc\"]\n"
            "[[event]]\nsignature = \"0x00000000000000dd\"\nasset = \"d.wav\"\n"
            "duration = 10\nmembers = [\"0x00000000000000dd\"]\n";
        const auto head = ayther::parse_audio_event_sig_list(t, "head");
        check(head.size() == 1 && head.count(0xaa) && head.at(0xaa).size() == 3 &&
                  head.at(0xaa)[2] == 0xcc,
              "#511: `head` parsea por disparador (3 firmas) y falta en la entrada sin cabeza");
        const auto members = ayther::parse_audio_event_sig_list(t, "members");
        check(members.size() == 2 && members.at(0xdd).size() == 1,
              "#511: el mismo parser lee `members` (2 entradas)");
    }

    // ---- Pack en miniatura con un tono de 1 s ------------------------------
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::printf("[FAIL] SDL_Init(AUDIO) dummy: %s\n", SDL_GetError());
        return 1;
    }
    const auto wav = fixture_wav_bytes(44100);
    char err[256] = "";
    AytherPackBuilder* b = ayther_pack_builder_new();
    // El builder exige un manifest — el mínimo que AyArchive acepta.
    const std::string manifest =
        "[pack]\nname = \"tail_test\"\nversion = \"0.0.1\"\n"
        "game_id = \"crc32:00000000\"\nayther_min = \"0.8.0\"\n"
        "\n[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";
    ayther_pack_builder_add_bytes(b, "manifest.toml",
        reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
    ayther_pack_builder_add_bytes(b, "tone.wav", wav.data(), wav.size());
    const bool baked = ayther_pack_builder_finish(b, /*sign=*/true,
                                                  "tail_test.ay",
                                                  err, sizeof(err));
    ayther_pack_builder_free(b);
    if (!baked) {
        std::printf("[FAIL] pack builder: %s\n", err);
        return 1;
    }
    AyArchive* pack = ayther_pack_open("tail_test.ay");
    if (!pack) {
        std::printf("[FAIL] no abre el pack de prueba\n");
        return 1;
    }
    AudioPlayer p;
    if (!p.init()) {
        std::printf("[FAIL] AudioPlayer::init() dummy\n");
        return 1;
    }

    // ---- Non-loop MÁS LARGO que la ventana, tail 0: corte exacto -----------
    check(p.play_event_hd(pack, "tone.wav", false, 0x1, /*end=*/10, /*cut=*/10),
          "arranca el non-loop con ventana [.,10] y tail 0");
    check(p.event_count() == 1, "stream vivo dentro de la ventana");
    p.tick_events(10);
    check(p.event_count() == 1, "en el límite exacto sigue vivo");
    p.tick_events(11);
    check(p.event_count() == 0,
          "pasado cut_frame el stream MUERE aunque tenga PCM encolado");

    // ---- Non-loop LEGACY (sin tail): drena entero como siempre -------------
    check(p.play_event_hd(pack, "tone.wav", false, 0x2, 10),
          "arranca el non-loop legacy (cut ilimitado)");
    p.tick_events(10000);
    check(p.event_count() == 1,
          "legacy: sigue drenando su asset después de end_frame");
    p.stop_all_events();

    // ---- Loop con corte a mitad de vuelta ----------------------------------
    check(p.play_event_hd(pack, "tone.wav", true, 0x3, /*end=*/5, /*cut=*/5),
          "arranca el loop [.,5] tail 0");
    p.tick_events(6);
    check(p.event_count() == 0,
          "el loop termina en su límite aun con PCM encolado");

    // ---- Loop con tail finito: deja de re-alimentar y drena hasta cut ------
    check(p.play_event_hd(pack, "tone.wav", true, 0x4, /*end=*/5, /*cut=*/8),
          "arranca el loop [.,5] con tail 3");
    p.tick_events(6);
    check(p.event_count() == 1, "en el tail sigue drenando (sin re-alimentar)");
    p.tick_events(9);
    check(p.event_count() == 0, "el tail nunca supera su máximo");

    // ---- Loop LEGACY (cut ilimitado): nunca queda libre --------------------
    check(p.play_event_hd(pack, "tone.wav", true, 0x5, /*end=*/5),
          "arranca el loop legacy");
    p.tick_events(6);
    check(p.event_count() == 0, "un loop legacy muere en end_frame, como siempre");

    check(p.event_count() == 0 && p.sfx_count() == 0,
          "sin streams/handles tras expirar todas las ventanas");

    p.shutdown();
    ayther_pack_close(pack);
    SDL_Quit();
    std::remove("tail_test.ay");

    if (g_fail) { std::printf("--- %d FALLAS ---\n", g_fail); return 1; }
    std::printf("--- todo ok ---\n");
    return 0;
}
