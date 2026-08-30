// ---------------------------------------------------------------------------
// audio_player_pause_test — pausar corta TODO el gameplay, y solo el gameplay
// (#384). Corre sobre el driver "dummy" de SDL3: device real, sin hardware.
//
// POR QUÉ EXISTE. La pausa cortaba los SFX en vuelo pero no el PCM ya
// encolado en los streams continuos: el colchón DRC (~70 ms) seguía sonando
// tras el botón, y un asset HD largo sobrevivía entero. Además el corte viejo
// (stop_all_sfx) mataba también el preview explícito de Mezclar — pausar el
// gameplay no puede callar un Reproducir que el autor pidió a mano.
//
// Observables sin oído: pending_frames(), sfx_count(), los contadores de
// pausa y el retorno de cut_transport_audio() (cuadros descartados).
// ---------------------------------------------------------------------------
#include "audio_player.h"
#include "ayther_file.h"

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

// WAV S16 estéreo 44100 con un tono no nulo — el asset HD de los one-shot.
bool write_tone_wav(const std::string& path, uint32_t frames) {
    FILE* f = ayther::file_open(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t data_sz = frames * 4;
    const uint32_t riff_sz = 36u + data_sz;
    uint8_t hdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xAC,0,0,             // 44100 Hz
        0x10,0xB1,0x02,0,          // byte rate 44100*4
        4,0, 16,0,
        'd','a','t','a', 0,0,0,0 };
    std::memcpy(hdr + 4,  &riff_sz, 4);
    std::memcpy(hdr + 40, &data_sz, 4);
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::vector<int16_t> pcm(frames * 2);
    for (uint32_t i = 0; i < frames * 2; ++i)
        pcm[i] = static_cast<int16_t>(((i / 64) % 2) ? 8000 : -8000);
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace

int main() {
    std::printf("=== audio_player_pause_test — corte de pausa (#384) ===\n");

    // Driver dummy ANTES del init: device real sin hardware (CI-safe).
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::printf("[FAIL] SDL_Init(AUDIO) con driver dummy: %s\n", SDL_GetError());
        return 1;
    }

    AudioPlayer p;
    if (!p.init()) {
        std::printf("[FAIL] AudioPlayer::init() sobre el driver dummy\n");
        return 1;
    }

    const std::string tone = "audio_player_pause_tone.wav";
    if (!write_tone_wav(tone, 44100)) {   // 1 s de tono
        std::printf("[FAIL] no pude escribir el WAV de prueba\n");
        return 1;
    }

    // ---- El corte alcanza el backlog encolado Y el staging -----------------
    std::vector<int16_t> pcm(4410 * 2, 5000);   // 100 ms de PCM no nulo
    p.buffer_emulator(0x1111, pcm.data(), 4410);
    p.flush_emulator();                          // → encolado en emu_stream_
    p.buffer_emulator(0x2222, pcm.data(), 4410); // → staging sin flushear
    p.feed_synth(std::vector<float>(4410 * 2, 0.25f).data(), 4410);  // synth
    check(p.pending_frames() == 4410, "hay staging antes del corte");
    const uint64_t cut1 = p.cut_transport_audio();
    check(cut1 > 0, "el corte descartó cuadros (staging + emu + synth)");
    check(p.pending_frames() == 0, "staging vacío tras el corte");
    check(p.synth_queued_frames() == 0, "synth vacío tras el corte");
    check(p.drc_ratio() == 1.0f, "DRC neutro tras el corte");
    check(p.cut_transport_audio() == 0, "repetir el corte es idempotente (0 cuadros)");

    // ---- El corte respeta los previews explícitos de autoría ---------------
    p.play_oneshot_asset_file(tone, /*key=*/1);                          // gameplay
    p.play_oneshot_asset_file(tone, /*key=*/2, 0.0, 1.0f, /*preview=*/true);
    check(p.hd_voice_count() == 1 && p.sfx_count() == 1,
          "gameplay (voz) + preview (stream) sonando antes de pausar");
    p.cut_transport_audio();
    check(p.hd_voice_count() == 0 && p.sfx_count() == 1,
          "pausar corta el gameplay y respeta el preview");
    p.stop_all_sfx();
    check(p.sfx_count() == 0 && p.hd_voice_count() == 0,
          "stop_all_sfx (shutdown) sí corta todo");

    // ---- 100 ciclos pause/play: idempotente, sin fugas de streams ----------
    const uint64_t cuts_before = p.pause_cuts();
    for (int i = 0; i < 100; ++i) {
        p.buffer_emulator(0x3333, pcm.data(), 4410);
        p.flush_emulator();
        p.play_oneshot_asset_file(tone, /*key=*/10 + (i % 3));
        p.cut_transport_audio();
    }
    check(p.sfx_count() == 0, "100 ciclos pause/play: sin streams vivos");
    check(p.pending_frames() == 0, "100 ciclos: sin staging residual");
    check(p.pause_cuts() >= cuts_before + 100,
          "cada ciclo con audio contó como corte efectivo");
    check(p.pause_cut_frames() > 0, "telemetría acumulada de cuadros cortados");

    p.shutdown();
    SDL_Quit();
    std::remove(tone.c_str());

    if (g_fail) {
        std::printf("--- %d FALLAS ---\n", g_fail);
        return 1;
    }
    std::printf("--- todo ok ---\n");
    return 0;
}
