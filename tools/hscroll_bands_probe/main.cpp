// ---------------------------------------------------------------------------
// hscroll_bands_probe (#421) — ¿el plano tiene BANDAS con scroll horizontal
// independiente, o se desplaza entero?
//
// La Panorámica modela una tira RÍGIDA con UNA cámara: `panorama_cam_x/y` es una
// sola posición votada por las celdas visibles. Si dentro del mismo plano hay
// bandas que se mueven a distinto ritmo (line-scroll), no existe una posición
// que las explique a todas y el voto se parte. Antes de diseñar nada hay que
// saber si el caso ocurre de verdad y en qué plano — eso mide esta sonda.
//
// Lee la tabla HSCROLL de VRAM (reg $D · 4 bytes por entrada: 2 del plano A y 2
// del B) con el modo del reg $B, y cuenta por frame:
//   · valores DISTINTOS de scroll dentro del frame, por plano;
//   · BANDAS = corridas contiguas de líneas con el mismo valor.
// Una banda por plano = el plano se desplaza entero (la Panorámica lo cubre).
// Dos o más = line-scroll real.
//
// Necesita el compose de escena para tener VRAM: correr con
// AYTHER_SCENE_COMPOSE=1.
//
//   Run: bin/hscroll_bands_probe <core.dll> <rom> <toma.arp>
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using ayther::FrameView;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "uso: hscroll_bands_probe <core.dll> <rom> <toma.arp>\n");
        return 2;
    }
    const std::string core = argv[1], rom = argv[2], arp = argv[3];
    std::printf("=== hscroll_bands_probe (#421) ===\ncore: %s\nrom : %s\n"
                "toma: %s\n\n", core.c_str(), rom.c_str(), arp.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) {
        std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str());
        return 1;
    }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // La toma puede venir de un .arp del proyecto o grabarse desde el arranque
    // ("-"): para la pantalla de TÍTULO alcanza con dejar correr el boot, que
    // es más reproducible que depender de un archivo del proyecto del usuario.
    ayther::AytherRecording take;
    if (arp != "-") {
        auto take_opt = ayther::AytherRecording::load(arp);
        if (!take_opt) {
            std::fprintf(stderr, "[FAIL] no se pudo abrir la toma\n");
            return 1;
        }
        take = std::move(*take_opt);
    } else {
        // Frames a grabar: por defecto alcanza para logo + título + demo.
        const int kBootFrames = (argc >= 5) ? std::atoi(argv[4]) : 3600;
        s->record_start();
        for (int i = 0; i < kBootFrames; ++i) { s->set_input(0, 0); s->step(); }
        s->record_stop();
        take = s->take_recording();
    }
    const uint32_t N = take.frame_count();
    std::printf("toma: %u frames\n\n", N);

    // Resumen por plano: cuántos frames tuvieron N bandas.
    std::map<int, uint32_t> bands_hist[2];
    uint32_t frames_ok = 0, frames_no_vram = 0, frames_mode_off = 0;
    uint32_t dirty_bit2 = 0;
    int      max_bands[2] = { 0, 0 };
    uint32_t first_multi[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };

    // CONTROL DE VALIDEZ: si la toma se quedó en un logo estático, "no hay
    // line-scroll" no dice nada. Se registra cuánto se movió cada plano y
    // cuántas PANTALLAS distintas se vieron (por firma de las celdas), para que
    // el veredicto venga con la prueba de que hubo algo que medir.
    int hs_min[2] = { 32767, 32767 }, hs_max[2] = { -32768, -32768 };
    std::set<uint64_t> screens;

    for (uint32_t f = 0; f < N; ++f) {
        const FrameView* fv = s->replay_seek(take, f);
        if (!fv) continue;
        if (fv->scene_dirty & 4) ++dirty_bit2;
        for (int p = 0; p < 2; ++p) {
            const int h = fv->plane_hscroll[p];
            if (h < hs_min[p]) hs_min[p] = h;
            if (h > hs_max[p]) hs_max[p] = h;
        }
        screens.insert(fv->screen_signature(0x03));   // planos A|B
        if (!fv->scene_vram || fv->scene_vram_size < 0x10000) { ++frames_no_vram; continue; }
        size_t rsz = 0;
        const uint8_t* regs = s->vdp_regs(&rsz);
        if (!regs || rsz < 0x20) { ++frames_no_vram; continue; }

        const uint8_t mode = regs[11] & 3;
        if (!mode) { ++frames_mode_off; continue; }   // scroll de pantalla completa
        ++frames_ok;

        const uint32_t hscb  = (uint32_t)(regs[13] & 0x3F) << 10;
        const uint32_t hmask = mode == 3 ? 0xFF : mode == 2 ? 0xF8 : 0x07;
        const uint32_t lines = fv->fb_height ? fv->fb_height : 224;

        for (int plane = 0; plane < 2; ++plane) {
            std::set<int> distinct;
            int bands = 0, prev = -100000;
            for (uint32_t l = 0; l < lines; ++l) {
                const uint32_t off = hscb + ((l & hmask) << 2) + (plane ? 2 : 0);
                if (off + 1 >= fv->scene_vram_size) break;
                const int v = (int)((uint32_t)fv->scene_vram[off]
                                    | ((uint32_t)fv->scene_vram[off + 1] << 8));
                distinct.insert(v);
                if (v != prev) { ++bands; prev = v; }
            }
            ++bands_hist[plane][bands];
            if (bands > max_bands[plane]) max_bands[plane] = bands;
            if (bands > 1 && first_multi[plane] == 0xFFFFFFFFu) first_multi[plane] = f;
            (void)distinct;
        }
    }

    std::printf("frames medidos      : %u\n", frames_ok);
    std::printf("frames sin VRAM     : %u  (¿falta AYTHER_SCENE_COMPOSE=1?)\n",
                frames_no_vram);
    std::printf("frames scroll ENTERO: %u  (reg $B modo 0 — sin tabla por línea)\n",
                frames_mode_off);
    std::printf("frames con scene_dirty bit2 (cizalla detectada por el motor): %u\n",
                dirty_bit2);
    std::printf("CONTROL — pantallas distintas vistas: %zu · "
                "recorrido hscroll A: %d..%d · B: %d..%d\n\n",
                screens.size(), hs_min[0], hs_max[0], hs_min[1], hs_max[1]);

    for (int plane = 0; plane < 2; ++plane) {
        std::printf("── Plano %c ──\n", plane ? 'B' : 'A');
        std::printf("   bandas máximas en un frame: %d\n", max_bands[plane]);
        if (first_multi[plane] != 0xFFFFFFFFu)
            std::printf("   primer frame con >1 banda : %u\n", first_multi[plane]);
        std::printf("   distribución (bandas → frames): ");
        int shown = 0;
        for (const auto& [b, n] : bands_hist[plane]) {
            std::printf("%d→%u  ", b, n);
            if (++shown >= 12) { std::printf("…"); break; }
        }
        std::printf("\n\n");
    }

    const bool line_scroll = max_bands[0] > 1 || max_bands[1] > 1;
    std::printf("VEREDICTO: %s\n", line_scroll
        ? "HAY bandas con scroll independiente — una tira rígida no las modela"
        : "cada plano se desplaza ENTERO — la Panorámica lo cubre tal cual");
    return 0;
}
