// ---------------------------------------------------------------------------
// audio_live_resume_test — reanudar continúa, no reinicia (#385).
//
// POR QUÉ EXISTE. La pausa corta los streams HD (#384) pero el estado lógico
// del detector live decía «esta firma ya sonó»: al reanudar dentro del mismo
// evento no hay flanco nuevo y el reemplazo quedaba mudo hasta el próximo
// key-on — o, limpiando los flancos, se reiniciaba desde cero con desfase.
// El contrato nuevo: la instancia lógica sobrevive a la pausa y el stream se
// recrea DESDE EL OFFSET del reloj emulado; un loop conserva su fase tras
// cualquier cantidad de pausas; una instancia vencida se descarta.
//
// Dos capas: la DECISIÓN pura (audio_live_resume.h — sin SDL ni core) y el
// player real con driver dummy + pack en miniatura (el offset de
// play_event_hd crea/omite streams según el contrato #388).
// ---------------------------------------------------------------------------
#include "audio_live_resume.h"
#include "audio_player.h"
#include "ayther_core_ffi.h"

#include <SDL3/SDL.h>

#include <cmath>
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

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

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
    std::printf("=== audio_live_resume_test — reanudar continúa (#385) ===\n");

    using ayther::LiveResumeAction;
    using ayther::live_resume_decide;
    using ayther::live_instance_over;
    using ayther::live_resume_offset_bytes;
    constexpr uint64_t NOCUT = ayther::kLiveNoCut;

    // ---- One-shot de 5 s pausado en 1,25 s: mismo punto, no el comienzo ----
    {
        // Ventana [100, 400], sin tail autorado (legacy). El reloj emulado
        // quedó en 175 = 75 frames adentro = 1,25 s a 60 fps.
        const auto d = live_resume_decide(175, 100, 400, NOCUT,
                                          /*loop=*/false, 60.0, 5.0);
        check(d.action == LiveResumeAction::Restart,
              "one-shot pausado a mitad se reanuda");
        check(near(d.offset_seconds, 1.25),
              "el offset es el del reloj emulado (1,25 s), no cero");
        // El anclaje mismo (pausa en el frame del disparo) entra desde 0.
        const auto d0 = live_resume_decide(100, 100, 400, NOCUT, false, 60.0, 5.0);
        check(d0.action == LiveResumeAction::Restart && near(d0.offset_seconds, 0.0),
              "pausa en el frame del disparo reanuda desde 0");
    }

    // ---- Evento sostenido: la ventana abierta basta, sin key-on nuevo ------
    {
        const auto d = live_resume_decide(390, 100, 400, NOCUT, false, 60.0, 6.0);
        check(d.action == LiveResumeAction::Restart,
              "ventana todavía abierta = se reanuda sin flanco nuevo");
    }

    // ---- Instancia vencida: descartar, no reiniciar ------------------------
    {
        // El asset (5 s = 300 frames) ya drenó entero: nada que reanudar.
        const auto d = live_resume_decide(430, 100, 400, NOCUT, false, 60.0, 5.0);
        check(d.action == LiveResumeAction::Finished,
              "one-shot ya drenado (offset >= duración) se descarta");
        // Ventana con tail: pasado cut_frame la instancia quedó atrás.
        const auto dt = live_resume_decide(431, 100, 400, /*cut=*/430,
                                           false, 60.0, 30.0);
        check(dt.action == LiveResumeAction::Finished,
              "pasado end+tail (#386) se descarta aunque el asset sea largo");
        const auto din = live_resume_decide(430, 100, 400, 430, false, 60.0, 30.0);
        check(din.action == LiveResumeAction::Restart,
              "en el límite exacto del cut todavía se reanuda");
    }

    // ---- Loop: fase conservada tras tres ciclos de pausa -------------------
    {
        // Asset de 2 s (352 800 bytes S16 estéreo @44100). Tres reanudaciones
        // en 1,0 s / 2,5 s / 7,7 s de reloj emulado: el offset físico es
        // SIEMPRE el módulo del asset — misma fase que si nunca hubiera
        // pausado.
        const uint64_t pcm = 2ull * 44100ull * 4ull;
        const double elapsed[3] = {1.0, 2.5, 7.7};
        bool phase_ok = true;
        for (const double t : elapsed) {
            const auto d = live_resume_decide(
                1000 + static_cast<uint64_t>(t * 60.0), 1000, 2000, NOCUT,
                /*loop=*/true, 60.0, 0.0);
            if (d.action != LiveResumeAction::Restart) { phase_ok = false; break; }
            const uint64_t off = live_resume_offset_bytes(
                d.offset_seconds, 44100, 4, pcm, true);
            const uint64_t want = static_cast<uint64_t>(
                std::fmod(d.offset_seconds, 2.0) * 44100.0) * 4ull;
            if (off != want || off >= pcm || off % 4 != 0) { phase_ok = false; break; }
        }
        check(phase_ok, "loop: 3 reanudaciones caen en fase (módulo del asset)");
        // Un loop nunca queda libre: pasado end_frame (sin tail) muere.
        const auto d = live_resume_decide(2001, 1000, 2000, NOCUT, true, 60.0, 0.0);
        check(d.action == LiveResumeAction::Finished,
              "loop pasado su end_frame no se reanuda (contrato tick_events)");
        // Con tail (#386) drena hasta cut y ahí sí muere.
        check(live_resume_decide(2030, 1000, 2000, 2060, true, 60.0, 0.0).action
                  == LiveResumeAction::Restart &&
              live_resume_decide(2061, 1000, 2000, 2060, true, 60.0, 0.0).action
                  == LiveResumeAction::Finished,
              "loop con tail: vive hasta cut_frame y ni un frame más");
    }

    // ---- El offset respeta el timing real (PAL) y la alineación ------------
    {
        const auto d = live_resume_decide(575, 500, 900, NOCUT, false, 50.0, 9.0);
        check(near(d.offset_seconds, 1.5), "a 50 fps (PAL) 75 frames = 1,5 s");
        const uint64_t off = live_resume_offset_bytes(0.333, 44100, 4,
                                                      44100ull * 4ull, false);
        check(off % 4 == 0, "el offset físico cae en cuadro completo");
        check(live_resume_offset_bytes(0.0, 44100, 4, 1000, true) == 0 &&
              live_resume_offset_bytes(1.0, 0, 4, 1000, true) == 0,
              "offset 0 / spec rota = arranque desde el comienzo");
    }

    // ---- One-shot LIBRE (sin ventana): sólo lo poda su duración ------------
    {
        check(!live_instance_over(1u << 20, 0xFFFFFFFFFFFFFFFFull,
                                  0xFFFFFFFFFFFFFFFFull, false),
              "instancia libre no vence por frames (la poda su asset)");
        const auto d = live_resume_decide(100 + 240, 100, NOCUT, NOCUT,
                                          false, 60.0, 3.0);
        check(d.action == LiveResumeAction::Finished,
              "one-shot libre de 3 s con 4 s de reloj encima = vencido");
    }

    // ---- Player real: el offset crea/omite streams según el contrato -------
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::printf("[FAIL] SDL_Init(AUDIO) dummy: %s\n", SDL_GetError());
        return 1;
    }
    const auto wav = fixture_wav_bytes(44100);   // 1 s de audio
    char err[256] = "";
    AytherPackBuilder* b = ayther_pack_builder_new();
    const std::string manifest =
        "[pack]\nname = \"resume_test\"\nversion = \"0.0.1\"\n"
        "game_id = \"crc32:00000000\"\nayther_min = \"0.8.0\"\n"
        "\n[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";
    ayther_pack_builder_add_bytes(b, "manifest.toml",
        reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
    ayther_pack_builder_add_bytes(b, "tone.wav", wav.data(), wav.size());
    const bool baked = ayther_pack_builder_finish(b, /*sign=*/true,
                                                  "resume_test.ay",
                                                  err, sizeof(err));
    ayther_pack_builder_free(b);
    if (!baked) { std::printf("[FAIL] pack builder: %s\n", err); return 1; }
    AyArchive* pack = ayther_pack_open("resume_test.ay");
    if (!pack) { std::printf("[FAIL] no abre el pack de prueba\n"); return 1; }
    AudioPlayer p;
    if (!p.init()) { std::printf("[FAIL] AudioPlayer::init() dummy\n"); return 1; }

    // Reanudar a mitad del asset del pack: stream vivo.
    check(p.play_event_hd(pack, "tone.wav", false, 0x10, /*end=*/600,
                          /*cut=*/NOCUT, /*offset=*/0.5),
          "reanudación del pack a mitad del asset arranca");
    check(p.event_count() == 1, "stream del evento vivo tras reanudar");
    p.stop_all_events();

    // Non-loop con el offset pasado el final: éxito SIN stream (#388 — el
    // original de esa ventana también pasó; no es un fallo del asset).
    check(p.play_event_hd(pack, "tone.wav", false, 0x11, 600, NOCUT, 2.0),
          "offset pasado el final devuelve éxito (no es fallo)");
    check(p.event_count() == 0, "…pero no crea stream (nada que sonar)");

    // Loop con offset mayor que el asset: entra por el módulo (fase).
    check(p.play_event_hd(pack, "tone.wav", true, 0x12, 600, NOCUT, 2.5),
          "loop reanudado con 2,5 s sobre un asset de 1 s arranca");
    check(p.event_count() == 1, "…con stream vivo (offset = módulo)");
    p.stop_all_events();

    // El one-shot de DISCO ya tenía offset (#220/#388): pasado el final es
    // éxito sin stream — mismo contrato que el pack.
    {
        FILE* f = std::fopen("resume_tone.wav", "wb");
        check(f && std::fwrite(wav.data(), 1, wav.size(), f) == wav.size(),
              "tono suelto de disco escrito");
        if (f) std::fclose(f);
        check(p.play_oneshot_asset_file("resume_tone.wav", 0x13, 5.0),
              "one-shot de disco pasado el final = éxito");
        check(p.hd_voice_count() == 0, "…sin voz");
        check(p.play_oneshot_asset_file("resume_tone.wav", 0x14, 0.25) &&
                  p.hd_voice_count() == 1,
              "one-shot de disco a mitad = voz viva");
        p.stop_all_sfx();
    }

    p.shutdown();
    ayther_pack_close(pack);
    SDL_Quit();
    std::remove("resume_test.ay");
    std::remove("resume_tone.wav");

    if (g_fail) { std::printf("--- %d FALLAS ---\n", g_fail); return 1; }
    std::printf("--- todo ok ---\n");
    return 0;
}
