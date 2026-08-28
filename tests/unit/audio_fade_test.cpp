// ---------------------------------------------------------------------------
// audio_fade_test — la política de fin FADE_OUT (#396), en el dominio de
// MUESTRAS.
//
// POR QUÉ EXISTE. #386 entregó dos políticas de fin: `hard_cut` (corte exacto
// en el límite) y `tail` (cola acotada que drena). La tercera, `fade_out`,
// quedó sin implementar, y su requisito duro no es «que baje el volumen» sino
// DÓNDE ocurre: si el fade se aplicara por frame o por reloj de pared, su
// duración cambiaría con el catch-up — el mismo motivo por el que #387 movió
// la colocación a muestras. Acá se verifica sobre el mixer, que es donde la
// rampa vive.
//
// Se prueba contra `HdMixer` directo y no contra el device: la mezcla es una
// función pura sobre un buffer, así que la rampa se puede medir muestra a
// muestra sin SDL y sin oído.
// ---------------------------------------------------------------------------
#include "audio_hd_mixer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

/// PCM constante: cada muestra vale `v`, así la ganancia aplicada se lee
/// directo del buffer de salida (out / v = g) sin despejar nada.
HdMixPcm flat_pcm(size_t frames, int16_t v) {
    auto pcm = std::make_shared<std::vector<int16_t>>(frames * 2, v);
    return pcm;
}

/// Mezcla `frames` cuadros desde `block_start` sobre un buffer en cero.
std::vector<int16_t> mix_block(HdMixer& m, size_t frames, uint64_t block_start) {
    std::vector<int16_t> out(frames * 2, 0);
    m.mix_into(out.data(), frames, block_start);
    return out;
}

constexpr int16_t kAmp = 10000;

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== audio_fade_test — política fade_out (#396) ===\n");

    // ---- 1. Sin fade autorado, el contrato de #386 no cambia --------------
    {
        HdMixer m;
        m.start(1, flat_pcm(44100, kAmp), 0, 0, 1.0f, /*looping=*/false,
                /*event=*/true, /*end=*/10, /*cut=*/10, /*fade_frames=*/0);
        m.tick_frame(11);   // pasó el corte
        check(m.event_voice_count() == 0,
              "sin fade, pasar el corte mata la voz (hard_cut de #386 intacto)");
    }

    // ---- 2. La rampa: duración exacta y monotonía --------------------------
    // El fade arranca al pasar end_frame y llega a silencio `fade_frames`
    // cuadros después — que es lo que la issue pide poder prometerle al autor.
    {
        constexpr uint32_t kFade = 1000;
        HdMixer m;
        m.start(2, flat_pcm(44100, kAmp), 0, 0, 1.0f, false, true,
                /*end=*/10, /*cut=*/UINT64_MAX, kFade);
        m.tick_frame(11);   // dispara la rampa
        check(m.event_voice_count() == 1, "el fade NO mata la voz de golpe");

        const std::vector<int16_t> out = mix_block(m, kFade + 10, 0);
        bool monotonic = true, starts_high = false, ends_in_silence = false;
        int16_t prev = 32767;
        for (uint32_t i = 0; i < kFade; ++i) {
            const int16_t s = out[i * 2];
            if (s > prev) monotonic = false;
            prev = s;
        }
        starts_high = out[0] > int16_t(kAmp * 0.95f);
        ends_in_silence = out[(kFade - 1) * 2] < int16_t(kAmp * 0.05f);
        check(starts_high, "la rampa arranca en ~ganancia plena");
        check(monotonic, "la rampa es monotónicamente decreciente (sin escalones)");
        check(ends_in_silence, "…y llega a ~silencio al final del fade");
        check(out[(kFade + 5) * 2] == 0,
              "pasado el fade no suena nada más (la voz murió sola)");
        check(m.event_voice_count() == 0, "…y la voz se recogió");
    }

    // ---- 3. Cruzar un borde de bloque no rompe la rampa --------------------
    // El fade que cae a caballo de dos `mix_into` es el caso que un contador
    // por bloque arruinaría: la segunda mitad tiene que seguir bajando desde
    // donde quedó la primera, no reiniciarse.
    {
        constexpr uint32_t kFade = 800;
        HdMixer m;
        m.start(3, flat_pcm(44100, kAmp), 0, 0, 1.0f, false, true,
                10, UINT64_MAX, kFade);
        m.tick_frame(11);
        const std::vector<int16_t> a = mix_block(m, 300, 0);
        const std::vector<int16_t> b = mix_block(m, 300, 300);
        const int16_t last_a = a[299 * 2];
        const int16_t first_b = b[0];
        check(first_b <= last_a,
              "cruzando el borde de bloque la rampa NO repunta");
        check(first_b > 0, "…y todavía suena (no se cortó en el borde)");
        std::printf("       último del bloque 1 = %d · primero del 2 = %d\n",
                    last_a, first_b);
    }

    // ---- 4. 1×1 vs catch-up: la duración no depende del tamaño del bloque --
    // El oráculo de #387 aplicado al fade. Si la rampa se calculara por bloque
    // o por reloj, estas dos corridas darían distinto.
    {
        constexpr uint32_t kFade = 512;
        auto run = [](size_t block_size) {
            HdMixer m;
            m.start(4, flat_pcm(44100, kAmp), 0, 0, 1.0f, false, true,
                    10, UINT64_MAX, kFade);
            m.tick_frame(11);
            std::vector<int16_t> output;
            for (size_t done = 0; done < kFade + 64; done += block_size) {
                const std::vector<int16_t> blk = mix_block(m, block_size, done);
                output.insert(output.end(), blk.begin(), blk.end());
            }
            return output;
        };
        const std::vector<int16_t> one_at_a_time = run(1);      // 1 cuadro por bloque
        const std::vector<int16_t> catch_up = run(64);  // catch-up
        const size_t n = std::min(one_at_a_time.size(), catch_up.size());
        bool identical = true;
        for (size_t i = 0; i < n && identical; ++i) identical = one_at_a_time[i] == catch_up[i];
        check(identical, "1×1 vs catch-up 64: la rampa es BYTE-IDÉNTICA");
    }

    // ---- 5. El fade manda sobre el corte de la ventana ---------------------
    // Son políticas ALTERNATIVAS: con fade autorado, el cut_frame de #386 no
    // debe interrumpir la rampa a mitad (si no, el «click» vuelve por la
    // ventana de al lado).
    {
        HdMixer m;
        m.start(5, flat_pcm(44100, kAmp), 0, 0, 1.0f, false, true,
                /*end=*/10, /*cut=*/11, /*fade=*/2000);
        m.tick_frame(11);
        m.tick_frame(12);   // pasado el cut: sin fade esto la mataría
        check(m.event_voice_count() == 1,
              "con fade autorado, el corte de la ventana no interrumpe la rampa");
    }

    // ---- 6. Un LOOP con fade deja de re-alimentarse y se apaga -------------
    {
        constexpr uint32_t kFade = 600;
        HdMixer m;
        m.start(6, flat_pcm(1000, kAmp), 0, 0, 1.0f, /*looping=*/true, true,
                10, UINT64_MAX, kFade);
        m.tick_frame(11);
        const std::vector<int16_t> out = mix_block(m, kFade + 32, 0);
        check(out[(kFade - 1) * 2] < int16_t(kAmp * 0.05f),
              "el loop con fade llega a silencio en su duración");
        check(m.event_voice_count() == 0, "…y se apaga (no queda loopeando)");
    }

    std::printf("\n%s (%d fallos)\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
