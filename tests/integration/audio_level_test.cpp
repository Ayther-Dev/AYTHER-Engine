// ---------------------------------------------------------------------------
// audio_level_test.cpp — #291: medir el nivel de un asset de audio.
//
// EL ESTÍMULO ES SINTÉTICO Y CONOCIDO, que es lo que hace que este test valga:
// se escriben WAVs cuya amplitud sabemos de antemano y se compara la medición
// contra la cuenta hecha a mano. Con un asset real sólo se podría verificar que
// «devuelve algo», que es lo que no queremos.
//
// Los tres casos que importan:
//
//   · media escala      → pico ≈ -6 dBFS, sin clipping;
//   · tope de escala    → clipping DETECTADO (la meseta se oye);
//   · muy bajo          → «too_quiet», porque se va a perder debajo del juego.
//
// Y el control que evita el falso positivo más fácil: una señal que TOCA el
// tope una sola vez por ciclo NO es clipping. Contar muestras al máximo a secas
// marcaría todo master comercial.
//
// No abre device de audio: medir es decodificar y recorrer muestras.
// ---------------------------------------------------------------------------
#include "audio_player.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

/// WAV S16 estéreo 44100 con una onda cuadrada de amplitud `amp`.
/// `flat_run` = cuántas muestras seguidas se quedan EN el tope (para fabricar
/// una meseta de verdad, que es lo que el detector tiene que ver).
bool write_wav(const std::filesystem::path& p, int16_t amp, uint32_t frames,
               int flat_run = 0) {
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const uint32_t data = frames * 4, riff = 36 + data;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(riff); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t v;
        if (flat_run > 0 && (i % 100) < static_cast<uint32_t>(flat_run)) {
            v = 32767;                       // meseta en el tope
        } else {
            v = (i / 50) % 2 ? amp : static_cast<int16_t>(-amp);
        }
        u16(static_cast<uint16_t>(v)); u16(static_cast<uint16_t>(v));
    }
    std::fclose(f);
    return true;
}

}  // namespace

int main() {
    std::printf("=== audio_level_test — #291: nivel de un asset ===\n");
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ayther_level_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    AudioPlayer player;   // sin init(): medir no necesita device

    // -- media escala: -6 dBFS, limpio ---------------------------------------
    {
        const fs::path p = dir / "half.wav";
        write_wav(p, 16384, 44100);          // 1 s a media escala
        const auto& lv = player.asset_level(p.string());
        check(lv.ok, "media escala: se pudo medir");
        check(std::abs(lv.duration_s - 1.0) < 0.01, "…duración 1 s");
        check(lv.sample_rate == 44100 && lv.channels == 2,
              "…sample rate y canales del contenedor");
        // 16384/32767 = 0,5 → -6,02 dBFS. Medio dB de tolerancia: la conversión
        // a la tasa de mezcla puede mover el pico un pelo.
        check(std::abs(lv.peak_db + 6.02f) < 0.5f, "…pico ≈ -6 dBFS");
        check(!lv.clipping, "…y NO clipea");
        check(!lv.too_hot, "…ni está al límite");
        // Una cuadrada de media escala tiene RMS = pico → también ≈ -6 dBFS.
        check(std::abs(lv.rms_db + 6.02f) < 1.0f, "…RMS ≈ -6 dBFS (onda cuadrada)");
        // Para llevar el pico a -1 dBFS hay que subir ~5 dB.
        check(std::abs(lv.suggested_gain_db - 5.02f) < 0.6f,
              "…y sugiere +5 dB para llegar a -1 dBFS");
    }

    // -- meseta en el tope: clipping -----------------------------------------
    {
        const fs::path p = dir / "clipped.wav";
        write_wav(p, 16384, 44100, /*flat_run=*/10);
        const auto& lv = player.asset_level(p.string());
        check(lv.ok && lv.clipping, "una MESETA en el tope se detecta como clipping");
        check(lv.clipped_runs > 0, "…y se cuenta cuántas rachas hay");
        check(lv.too_hot, "…y queda marcado como demasiado fuerte");
    }

    // -- el control del falso positivo ---------------------------------------
    // Una cuadrada a escala COMPLETA toca el tope todo el tiempo… así que ésta
    // es la que tiene que dar clipping. La que NO tiene que darlo es la de
    // media escala de arriba, que ya se verificó. Acá se fija el otro extremo:
    // un pico único al tope, sin racha, no alcanza.
    {
        const fs::path p = dir / "one_sample.wav";
        // flat_run = 1: una sola muestra al tope cada 100 → nunca hay 3 seguidas.
        write_wav(p, 16384, 44100, /*flat_run=*/1);
        const auto& lv = player.asset_level(p.string());
        check(lv.ok, "pico único al tope: se mide");
        check(!lv.clipping,
              "…y NO es clipping (hace falta una RACHA, no una muestra suelta)");
    }

    // -- demasiado bajo ------------------------------------------------------
    {
        const fs::path p = dir / "quiet.wav";
        write_wav(p, 300, 44100);            // ≈ -40 dBFS
        const auto& lv = player.asset_level(p.string());
        check(lv.ok && lv.too_quiet,
              "un asset muy bajo se marca: se perdería debajo del juego");
        check(!lv.too_hot && !lv.clipping, "…y no se confunde con clipping");
    }

    // -- lo que no se puede medir --------------------------------------------
    {
        const auto& lv = player.asset_level((dir / "no_existe.wav").string());
        check(!lv.ok, "un asset ausente devuelve ok=false, no ceros que parezcan datos");
    }

    fs::remove_all(dir, ec);
    std::printf("\n=== %d/%d ===\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
