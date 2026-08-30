// ---------------------------------------------------------------------------
// audio_unified_mix_test — la mezcla unificada coloca por MUESTRA (#387).
//
// POR QUÉ EXISTE. Los HD corrían en streams SDL propios: arrancaban respecto
// del reloj de pared mientras el original viajaba con ~70 ms de colchón DRC —
// la fase entre los dos dependía del backlog, de los stalls y del tamaño del
// catch-up. El contrato nuevo: un disparo en el frame N cae en el MISMO
// sample ejecutando 1×1 o catch-up 16, y todo cruza el mismo DRC.
//
// EL ORÁCULO CENTRAL es de identidad: mezclar en bloques de a un frame y
// mezclar el mismo período en un solo bloque acumulado tiene que dar PCM
// byte-idéntico. Sin device, sin oído — HdMixer es puro a propósito.
// La segunda mitad cruza el AudioPlayer real (driver dummy): el switch
// unificado convierte los play_* en voces, los stops las alcanzan (#384,
// #329) y el contrato de vida por frame se conserva (#386).
// ---------------------------------------------------------------------------
#include "audio_hd_mixer.h"
#include "audio_player.h"
#include "ayther_file.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

/// PCM mix-ready sintético: rampa determinista (nada de ceros — un error de
/// colocación con silencio no se vería).
HdMixPcm ramp_pcm(size_t frames) {
    auto v = std::make_shared<std::vector<int16_t>>(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        (*v)[i * 2]     = static_cast<int16_t>((i * 37) % 8000);
        (*v)[i * 2 + 1] = static_cast<int16_t>(-(static_cast<int>((i * 53) % 8000)));
    }
    return v;
}

// WAV S16 estéreo 44100 con un tono no nulo — para el player real.
bool write_tone_wav(const std::string& path, uint32_t frames) {
    FILE* f = ayther::file_open(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t data_sz = frames * 4;
    const uint32_t riff_sz = 36u + data_sz;
    uint8_t hdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xAC,0,0, 0x10,0xB1,0x02,0, 4,0, 16,0,
        'd','a','t','a', 0,0,0,0 };
    std::memcpy(hdr + 4,  &riff_sz, 4);
    std::memcpy(hdr + 40, &data_sz, 4);
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::vector<int16_t> pcm(frames * 2, 9000);
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace

int main() {
    std::printf("=== audio_unified_mix_test — un solo reloj (#387) ===\n");
    constexpr size_t kFrame = 735;   // cuadros de un frame NTSC a 44100

    // ---- EL oráculo: 1×1 y catch-up dan PCM idéntico -----------------------
    {
        // Voz disparada «en el frame 2» de un período de 4 frames.
        auto run = [&](const std::vector<size_t>& blocks) {
            HdMixer m;
            std::vector<int16_t> out(4 * kFrame * 2, 0);
            uint64_t cursor = 0;
            size_t   at     = 0;
            bool started = false;
            for (size_t b : blocks) {
                // El disparo del frame 2 llega cuando su bloque está staged.
                if (!started && cursor + b > 2 * kFrame) {
                    m.start(0x11, ramp_pcm(3 * kFrame), /*start=*/2 * kFrame,
                            0, 1.0f, false, true, UINT64_MAX, UINT64_MAX);
                    started = true;
                }
                m.mix_into(out.data() + at * 2, b, cursor);
                cursor += b;
                at     += b;
            }
            return out;
        };
    const auto one_frame_at_a_time = run({kFrame, kFrame, kFrame, kFrame});
        const auto catch_up  = run({4 * kFrame});
    check(one_frame_at_a_time == catch_up,
              "1x1 y catch-up 4 producen PCM BYTE-IDENTICO");
        // Y el disparo cayó donde debía: silencio hasta el sample 2*kFrame,
        // señal exactamente desde ahí.
    check(one_frame_at_a_time[2 * kFrame * 2 - 2] == 0 &&
          one_frame_at_a_time[(2 * kFrame + 1) * 2] != 0,
              "la voz entra EXACTAMENTE en el sample de su frame");
        // El issue pide la matriz 2/4/16: el tamaño del bloque no puede
        // cambiar NADA. Con 2 el disparo cae en el borde de bloque; con 4 cae
        // adentro; el de 16 es el catch-up que el loop de la app puede hacer
        // tras un stall — el caso donde el camino de streams se desfasaba.
    check(one_frame_at_a_time == run({2 * kFrame, 2 * kFrame}),
              "1x1 y catch-up 2 producen PCM BYTE-IDENTICO");
    check(one_frame_at_a_time == run({kFrame, 3 * kFrame}),
              "un bloque que ENGLOBA el disparo lo coloca igual");
    }

    // ---- Catch-up 16: el bloque más grande que el loop de la app arma ------
    {
        // Mismo oráculo sobre un período de 16 frames, con el disparo en el
        // frame 9 — adentro del bloque único, no en su borde.
        auto run16 = [&](const std::vector<size_t>& blocks) {
            HdMixer m;
            std::vector<int16_t> out(16 * kFrame * 2, 0);
            uint64_t cursor = 0;
            size_t   at     = 0;
            bool started = false;
            for (size_t b : blocks) {
                if (!started && cursor + b > 9 * kFrame) {
                    m.start(0x99, ramp_pcm(4 * kFrame), /*start=*/9 * kFrame,
                            0, 1.0f, false, true, UINT64_MAX, UINT64_MAX);
                    started = true;
                }
                m.mix_into(out.data() + at * 2, b, cursor);
                cursor += b;
                at     += b;
            }
            return out;
        };
    std::vector<size_t> one_frame_chunks(16, kFrame);
    const auto ref = run16(one_frame_chunks);
        check(ref == run16({16 * kFrame}),
              "1x1 y catch-up 16 producen PCM BYTE-IDENTICO");
        check(ref[9 * kFrame * 2 - 2] == 0 && ref[(9 * kFrame + 1) * 2] != 0,
              "…y en el catch-up de 16 la voz sigue entrando en SU frame");
    }

    // ---- Loop: fase continua entre bloques y módulo en el offset -----------
    {
        HdMixer m;
        const size_t loop_frames = 100;
        m.start(0x22, ramp_pcm(loop_frames), 0, /*offset=*/250, 1.0f,
                /*loop=*/true, true, /*end=*/UINT64_MAX, UINT64_MAX);
        std::vector<int16_t> a(kFrame * 2, 0), b(kFrame * 2, 0);
        m.mix_into(a.data(), kFrame, 0);
        m.mix_into(b.data(), kFrame, kFrame);
        // offset 250 sobre un loop de 100 = fase 50; a[0] = pcm[50].
        const auto pcm = ramp_pcm(loop_frames);
        check(a[0] == (*pcm)[50 * 2],
              "el offset del loop entra por el módulo (fase 50)");
        // El primer sample del segundo bloque continúa la fase del primero:
        // (735 + 50) % 100 = 85.
        check(b[0] == (*pcm)[85 * 2],
              "la fase del loop CONTINÚA entre bloques");
    }

    // ---- Contrato #386 por tick_frame: end + tail, loop nunca libre --------
    {
        HdMixer m;
        m.start(0x33, ramp_pcm(50), 0, 0, 1.0f, true, true,
                /*end=*/10, /*cut=*/13);
        m.tick_frame(10);
        check(m.voice_count() == 1, "loop dentro de su ventana sigue vivo");
        m.tick_frame(11);
        check(m.voice_count() == 1, "en el tail deja de loopear pero drena");
        m.tick_frame(14);
        check(m.voice_count() == 0, "pasado cut_frame la voz MUERE");

        m.start(0x34, ramp_pcm(50), 0, 0, 1.0f, true, true,
                /*end=*/10, /*cut=*/UINT64_MAX);
        m.tick_frame(11);
        check(m.voice_count() == 0, "loop legacy (sin tail) muere en end_frame");
    }

    // ---- stop = fade a silencio; retrigger no superpone --------------------
    {
        HdMixer m;
        m.start(0x44, ramp_pcm(44100), 0, 0, 1.0f, false, false,
                UINT64_MAX, UINT64_MAX);
        check(m.stop(0x44), "stop alcanza la voz (contrato #329)");
        check(!m.stop(0x44), "el segundo stop no re-corta (ya en fade)");
        // El fade termina dentro de kFadeFrames: después, silencio.
        std::vector<int16_t> out(HdMixer::kFadeFrames * 2 + 512, 0);
        m.mix_into(out.data(), HdMixer::kFadeFrames + 256, 0);
        check(m.voice_count() == 0, "la voz muere al terminar su fade");
        check(out[(HdMixer::kFadeFrames + 100) * 2] == 0,
              "tras el fade hay SILENCIO");
    }

    // ---- Suma con saturación (dos voces fuertes no dan wrap) ---------------
    {
        HdMixer m;
        auto loud = std::make_shared<std::vector<int16_t>>(512 * 2, 30000);
        m.start(0x55, loud, 0, 0, 1.0f, false, false, UINT64_MAX, UINT64_MAX);
        m.start(0x56, loud, 0, 0, 1.0f, false, true, UINT64_MAX, UINT64_MAX);
        std::vector<int16_t> out(512 * 2, 0);
        m.mix_into(out.data(), 512, 0);
        check(out[0] == 32767, "la suma SATURA en vez de dar la vuelta");
    }

    // ---- Player real (driver dummy): el switch integra los contratos -------
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::printf("[FAIL] SDL_Init(AUDIO) dummy: %s\n", SDL_GetError());
        return 1;
    }
    AudioPlayer p;
    if (!p.init()) { std::printf("[FAIL] AudioPlayer::init() dummy\n"); return 1; }
    // #395: ya no hay `set_unified(true)` — el mixer es el único camino, y
    // este test lo ejercita tal como lo hace el Lab.
    const std::string tone = "unified_tone.wav";
    if (!write_tone_wav(tone, 44100)) {
        std::printf("[FAIL] no pude escribir el WAV\n");
        return 1;
    }

    // Un one-shot del gameplay es una VOZ (no un stream); el preview sigue
    // siendo stream (no pertenece al transporte, #384).
    check(p.play_oneshot_asset_file(tone, 0x1), "one-shot unificado arranca");
    check(p.hd_voice_count() == 1 && p.sfx_count() == 0,
          "…como voz del mixer, sin stream SDL");
    check(p.play_oneshot_asset_file(tone, 0x2, 0.0, 1.0f, /*preview=*/true),
          "el preview explícito arranca");
    check(p.sfx_count() == 1, "…y ÉSE sí es un stream aparte");

    // La pausa (#384) corta las voces del gameplay y respeta el preview.
    p.cut_transport_audio();
    check(p.hd_voice_count() == 0, "pausar corta TODAS las voces del mixer");
    check(p.sfx_count() == 1, "…y el preview sigue sonando");
    p.stop_all_sfx();

    // El flush mueve la línea de tiempo con el PCM staged (el reloj es el
    // bloque, no la pared) y el mute de un batch NO acorta el bloque.
    const uint64_t t0 = p.timeline_samples();
    std::vector<int16_t> pcm(kFrame * 2, 5000);
    p.mark_frame_boundary();
    p.buffer_emulator(0xAAA, pcm.data(), kFrame);
    p.set_user_mute_hashes(nullptr, 0);
    p.flush_emulator();
    check(p.timeline_samples() == t0 + kFrame,
          "la línea de tiempo avanza con el bloque flusheado");
    const uint64_t hash_muted = 0xBBB;
    p.mark_frame_boundary();
    p.buffer_emulator(hash_muted, pcm.data(), kFrame);
    p.set_user_mute_hashes(&hash_muted, 1);
    p.flush_emulator();
    check(p.timeline_samples() == t0 + 2 * kFrame,
          "un batch muteado se SILENCIA en su lugar (el eje no se corre)");
    p.set_user_mute_hashes(nullptr, 0);

    // flush(true) = range-mute unificado: el original calla, el bloque viaja.
    p.mark_frame_boundary();
    p.buffer_emulator(0xCCC, pcm.data(), kFrame);
    p.flush_emulator(true);
    check(p.timeline_samples() == t0 + 3 * kFrame,
          "suppress_original conserva el eje de tiempo del bloque");

    // ---- Stall del host: el re-cebado NO corre la fase relativa ------------
    // Tras >250 ms sin flushes con datos (#146) el player antepone silencio
    // hasta llenar el colchón del device. Ese silencio es del DEVICE: la línea
    // de tiempo lógica —donde se colocan las voces HD— no se mueve, así que la
    // fase entre original y HD sobrevive al stall. Es el criterio de #387 que
    // el camino de streams no podía cumplir: ahí el HD arrancaba contra el
    // reloj de pared y el colchón del original lo desfasaba.
    {
        const uint64_t t_stall = p.timeline_samples();
        SDL_Delay(300);                       // > 250 ms: dispara el re-cebado
        p.mark_frame_boundary();
        p.buffer_emulator(0xDDD, pcm.data(), kFrame);
        p.flush_emulator();
        // No vacuo: el prime deja el EMA clavado en el target (3072 frames),
        // muy por encima del backlog de un flush normal de 735.
        check(p.drc_queue_avg() >= 3000.0f,
              "el stall se detectó y re-cebó el colchón del device");
        check(p.timeline_samples() == t_stall + kFrame,
              "…pero la línea de tiempo avanzó SOLO el bloque (fase intacta)");
        // Y el disparo siguiente entra normalmente, sin arrastre del stall.
        p.mark_frame_boundary();
        check(p.play_oneshot_asset_file(tone, 0x7) && p.hd_voice_count() == 1,
              "tras el stall, un disparo nuevo nace en el frame actual");
        p.stop_all_sfx();
    }

    // play_event_hd unificado respeta el contrato #388 (pasado el final =
    // éxito sin voz) y el retrigger no duplica.
    check(p.play_event_hd(nullptr, "no.wav", false, 0x9, 10) == false,
          "sin pack no hay voz (fallo limpio)");
    check(p.play_oneshot_asset_file(tone, 0x3, /*offset=*/5.0),
          "offset pasado el final = éxito (#388)");
    check(p.hd_voice_count() == 0, "…sin voz (nada que sonar)");
    check(p.play_oneshot_asset_file(tone, 0x4, 0.25) && p.hd_voice_count() == 1,
          "offset a mitad = voz viva");
    check(p.play_oneshot_asset_file(tone, 0x4, 0.25),
          "retrigger de la misma key");
    check(p.hd_voice_count() == 2,
          "…la vieja queda en fade y la nueva suena (sin superposición eterna)");
    p.stop_all_sfx();
    check(p.hd_voice_count() == 0, "stop_all_sfx limpia las voces one-shot");

    p.shutdown();
    SDL_Quit();
    std::remove(tone.c_str());

    if (g_fail) { std::printf("--- %d FALLAS ---\n", g_fail); return 1; }
    std::printf("--- todo ok ---\n");
    return 0;
}
