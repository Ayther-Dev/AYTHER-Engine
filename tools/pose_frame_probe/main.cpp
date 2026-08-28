// ---------------------------------------------------------------------------
// pose_frame_probe — sonda de diagnóstico (NO es un oráculo): carga una grabación
// real y vuelca los sprites de un frame, marcando cuáles son de la pose. Sirve
// para entender el "reloj" (¿sprite o plano? ¿prioridad?) y los rastros del Genio.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target pose_frame_probe
//   Args:  <recording.arp> <frame> [pose_hash1 pose_hash2 ...]
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include <stb_image.h>          // impl linkeada desde el engine

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif
using ayther::FrameView;

static std::string quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    std::string core, rom, line; bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) { in_rom = true; continue; }
        if (line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = quoted(line);
        else if (in_rom && rom.empty() && line.find("path") != std::string::npos) rom = quoted(line);
    }
    core = resolve(core, root); rom = resolve(rom, root);
    // Sondar tomas de OTRO juego: la ROM del test_config es fija (Aladdin);
    // AYTHER_PROBE_ROM la reemplaza sin tocar el config.
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) rom = er;

    const std::string rec_path = argc > 1 ? argv[1]
        : "C:/Users/david/Documents/AytherProjects/Aladdin/recordings/000 - Inicio.arp";
    const uint32_t frame = argc > 2 ? (uint32_t)std::strtoul(argv[2], nullptr, 10) : 119;
    std::unordered_set<uint64_t> pose;
    for (int i = 3; i < argc; ++i) pose.insert(std::strtoull(argv[i], nullptr, 16));

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }
    std::printf("=== pose_frame_probe ===\nrec : %s (%u frames)\nframe: %u\npose hashes: %zu\n\n",
                rec_path.c_str(), rec->frame_count(), frame, pose.size());

    // AYTHER_PROBE_REBAKE=1: re-hornear la historia de hashes EN MEMORIA con el
    // hasher vigente (migración v8) antes del barrido — valida present()/marks
    // post-migración sin re-guardar el .arp.
    if (ayther::env_get("AYTHER_PROBE_REBAKE") && rec->hash_algo == 0) {
        std::printf("[..] re-horneando historia (hash_algo 0 → %u)…\n",
                    ayther::AytherRecording::kSpriteHashAlgo);
        ayther::AytherSession::SeekStep st;
        do { st = s->replay_rebake_history_step(*rec, 240); } while (!st.done);
        std::printf("[ok] historia re-horneada (%zu hashes, %zu keyframes)\n",
                    rec->sprite_hashes.size(), rec->keyframes.size());
    }

    // Barrido de la HISTORIA de hashes del .arp (present(), la fuente de los
    // marks del timeline): en cuántos frames aparecen TODOS los hashes dados.
    if (!pose.empty()) {
        uint32_t n_all = 0, n_any = 0, first_all = UINT32_MAX;
        for (uint32_t f = 0; f < rec->frame_count(); ++f) {
            bool all = true, any = false;
            for (uint64_t h : pose)
                if (rec->present(f, h)) any = true; else all = false;
            if (any) ++n_any;
            if (all) { ++n_all; if (first_all == UINT32_MAX) first_all = f; }
        }
        std::printf("historia .arp: TODOS los hashes en %u frames (primero=%d) · "
                    "alguno en %u frames\n\n", n_all, (int)first_all, n_any);
    }

    const FrameView* fv = s->replay_seek(*rec, frame);
    if (!fv) { std::fprintf(stderr, "[FAIL] replay_seek(%u) null\n", frame); return 1; }

    std::printf("sprite_occ_count = %u | plane_a=%u plane_b=%u plane_w=%u | plane_tile_occ=%u\n\n",
                fv->sprite_occ_count, fv->plane_a_count, fv->plane_b_count, fv->plane_w_count,
                fv->plane_tile_occ_count);
    std::printf("%-4s %-18s %-4s %-4s %-4s %-4s %-4s %-4s %-3s %-3s %s\n",
                "idx", "hash", "x", "y", "w", "h", "pri", "slot", "hf", "vf", "pose?");
    uint32_t n_pose = 0, n_other = 0;
    for (uint32_t i = 0; i < fv->sprite_occ_count; ++i) {
        const auto& o = fv->sprite_occs[i];
        const bool is_pose = pose.count(o.hash) > 0;
        if (is_pose) ++n_pose; else ++n_other;
        std::printf("%-4u %016llx %-4d %-4d %-4u %-4u %-4u %-4u %-3u %-3u %s\n",
                    i, (unsigned long long)o.hash, o.screen_x, o.screen_y,
                    o.w_tiles, o.h_tiles, o.priority, o.slot, o.hflip, o.vflip,
                    is_pose ? "SÍ" : "-- (no-pose)");
    }
    std::printf("\nResumen: %u de la pose, %u NO-pose. bbox pose 0x1 = x[246..310] y[43..147].\n",
                n_pose, n_other);

    // Volcar A (normal) y C (TODOS los sprites off) como PPM → ver si el 'reloj'
    // sobrevive sin sprites (= plano/HUD) o desaparece (= sprite).
    auto dump = [&](const char* path) {
        const FrameView* f = s->replay_seek(*rec, frame);
        if (!f || !f->fb_pixels) return;
        const unsigned W = f->fb_width, H = f->fb_height; const size_t pitch = f->fb_pitch;
        std::ofstream o(path, std::ios::binary);
        o << "P6\n" << W << " " << H << "\n255\n";
        for (unsigned y = 0; y < H; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(f->fb_pixels) + (size_t)y * pitch);
            for (unsigned x = 0; x < W; ++x) {
                const uint16_t p = row[x];
                const uint8_t r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
                char rgb[3] = { (char)((r5 << 3) | (r5 >> 2)), (char)((g6 << 2) | (g6 >> 4)),
                                (char)((b5 << 3) | (b5 >> 2)) };
                o.write(rgb, 3);
            }
        }
    };
    s->replay_invalidate(); dump("probe_A.ppm");
    uint8_t allb[16]; for (int i = 0; i < 16; ++i) allb[i] = 0xFF;
    s->set_sprite_suppress(allb, 16);
    s->replay_invalidate();
    if (const FrameView* fs = s->replay_seek(*rec, frame))
        std::printf("occs con TODOS los slots suprimidos: %u (¿la supresión "
                    "conserva las occurrences?)\n", fs->sprite_occ_count);
    s->replay_invalidate(); dump("probe_C.ppm");
    s->set_sprite_suppress(nullptr, 0);

    // Hipótesis D: suprimir SOLO los sprites NO-pose → donde A≠D, el no-pose era
    // visible AL FRENTE en A (restaurable con A); donde estaba ocluido, A==D.
    if (!pose.empty()) {
        s->replay_invalidate();
        const FrameView* f0 = s->replay_seek(*rec, frame);
        uint8_t dbits[16] = {0};
        int n_np = 0;
        for (uint32_t i = 0; i < f0->sprite_occ_count; ++i) {
            const auto& o = f0->sprite_occs[i];
            if (!pose.count(o.hash)) { dbits[(o.slot >> 3) & 0x0F] |= 1u << (o.slot & 7); ++n_np; }
        }
        std::printf("\n[D] %d sprites no-pose suprimidos\n", n_np);
        s->set_sprite_suppress(dbits, 16);
        s->replay_invalidate(); s->replay_seek(*rec, frame); s->replay_invalidate();
        const FrameView* fd = s->replay_seek(*rec, frame);
        std::vector<uint8_t> D;
        if (fd && fd->fb_pixels) {
            const uint8_t* p = static_cast<const uint8_t*>(fd->fb_pixels);
            D.assign(p, p + (size_t)fd->fb_height * fd->fb_pitch);
        }
        s->set_sprite_suppress(nullptr, 0);
        s->replay_invalidate();
        const FrameView* fA2 = s->replay_seek(*rec, frame);
        if (fA2 && !D.empty()) {
            const size_t pit = fA2->fb_pitch;
            const uint8_t* ap8 = static_cast<const uint8_t*>(fA2->fb_pixels);
            // diff A vs D por fila dentro del bbox de la pose
            std::printf("[D] A!=D dentro del bbox (246,43..310,147) por fila:\n");
            size_t total = 0;
            for (int y = 43; y < 147; ++y) {
                const uint16_t* ap = reinterpret_cast<const uint16_t*>(ap8 + (size_t)y * pit);
                const uint16_t* dp = reinterpret_cast<const uint16_t*>(D.data() + (size_t)y * pit);
                int xmin = -1, xmax = -1, cnt = 0;
                for (int x = 246; x < 310; ++x)
                    if (ap[x] != dp[x]) { if (xmin < 0) xmin = x; xmax = x; ++cnt; }
                if (cnt) { std::printf("  y=%3d  x[%d..%d]  n=%d\n", y, xmin, xmax, cnt); total += cnt; }
            }
            std::printf("[D] total=%zu (esperado: SOLO la zona del reloj ~(289..297,94..102))\n", total);
        }
    }

    // Preview ACTIVO: alimentar el pose-set con la firma de la pose (los hashes que
    // se pasaron por args) y volcar la BASE mostrada (B, = fb_pixels) + inspeccionar
    // el overlay. Así se ve si el reloj (no-pose) queda en B y si el overlay lo
    // restaura (alpha=255 en su posición).
    if (!pose.empty()) {
        // Reproducir la condición del Lab: el proyecto Aladdin tiene un override de
        // PLANO cargado (planes.toml) — B lo suprime; si nos no aplicara la misma
        // máscara, B≠nos en toda la extensión del tile → fragmentos espurios de A.
        s->assign_plane(0x46ed2bcfad951f57ull,
            "C:/Users/david/Documents/AytherProjects/Aladdin/graphics/hd_plane_test.png");
        std::vector<uint64_t> hv(pose.begin(), pose.end());
        // Escenario real: el Genio (pose con HD) + el reloj "05:30" (pose SIN HD →
        // snapshot). El motor debe: región del Genio limpia, reloj dibujado en la
        // posición de su occ, ENCIMA (slot C8 por área: 1 < 104).
        ayther::AytherSession::PosePreview genie, clock;
        genie.hashes = hv;
        genie.asset  = "C:/Users/david/Documents/AytherProjects/Aladdin/graphics/frame_00.png";
        genie.hd     = true;    // HD autorado → limpia los sueltos de su región
        clock.hashes = { 0xaac7e4127c507432ull };
        clock.asset  = "C:/Users/david/Documents/AytherProjects/Aladdin/graphics/_pose_snap/0000000d.png";
        clock.hd     = false;   // pose sin HD → snapshot (identidad, no limpia vecinos)
        s->set_pose_preview({ genie, clock });
        const FrameView* pf = nullptr;
        for (int k = 0; k < 4; ++k) { s->replay_invalidate(); pf = s->replay_seek(*rec, frame); }
        s->replay_invalidate(); s->replay_seek(*rec, frame); dump("probe_B_base.ppm");
        // #345: acá se volcaba el overlay del compose por supresión
        // (compose_pixels). Esa maquinaria se borró con la issue; el resto de
        // la sonda —los sprites del frame y quién es de la pose— sigue igual.
        s->set_pose_preview({});
    }
    std::printf("Volcados: probe_A.ppm (normal), probe_C.ppm (sin sprites), probe_B_base.ppm (base del preview).\n");

    // ── Lista CRUDA de sprites parseados por el VDP (ids 0x10B/0x10C del fork) ──
    // La fuente autoritativa: cada entrada es lo que parse_satb parseó EN SUS LÍNEAS
    // (Y de la caché interna, X/attr de VRAM al momento). Comparar contra las occs
    // detecta pérdidas del pipeline (p.ej. el filtro tile==0 del hasher Rust).
    {
        s->replay_invalidate(); s->replay_seek(*rec, frame);
        uint8_t pn = 0;
        const uint8_t* ps = s->parsed_sprites_raw(&pn);
        std::printf("\nSprites PARSEADOS por el VDP este frame: %u\n", pn);
        if (ps && pn) {
            std::printf("%-4s %-5s %-5s %-4s %-4s %-6s %-4s %-4s %-6s %-5s %s\n",
                        "i", "sx", "sy", "w", "h", "tile", "pal", "pri", "flips", "sat", "chain");
            for (uint8_t i = 0; i < pn; ++i) {
                const uint8_t* e = ps + (size_t)i * 10;   // AytherSpr = 10 bytes
                const uint16_t yr   = (uint16_t)(e[0] | (e[1] << 8));
                const uint16_t xr   = (uint16_t)(e[2] | (e[3] << 8));
                const uint16_t attr = (uint16_t)(e[4] | (e[5] << 8));
                const int sx2 = (int)(xr & 0x1FF) - 128, sy2 = (int)(yr & 0x1FF) - 128;
                std::printf("%-4u %-5d %-5d %-4u %-4u 0x%03x  %-4u %-4u %c%c     %-5u %u\n",
                            i, sx2, sy2, e[6], e[7], attr & 0x7FF,
                            (attr >> 13) & 3, (attr >> 15) & 1,
                            (attr & 0x0800) ? 'H' : '-', (attr & 0x1000) ? 'V' : '-',
                            e[8], e[9]);
            }
        }
    }

    // ── SAT CRUDA del frame: 80 entradas (y, size, link, tile, x) decodificadas ──
    // Para comparar la TABLA (lo que parsea el hasher) contra el RENDER (lo que el
    // VDP dibuja siguiendo la cadena de links) y detectar desalineaciones de índice.
    {
        s->replay_invalidate(); s->replay_seek(*rec, frame);
        size_t vsz = 0;
        const uint8_t* vram = s->video_ram(&vsz);
        if (vram && vsz >= 0x10000) {
            // Base del SAT: registro 5 del VDP (sat = (reg5 & 0x7F) << 9 en H40:
            // bit0 ignorado → &0x7E). El fork expone los regs (id 0x101).
            size_t rsz = 0;
            const uint8_t* regs = s->vdp_regs(&rsz);
            size_t sat_base = 0xF800;
            if (regs && rsz > 5) sat_base = ((size_t)(regs[5] & 0x7F)) << 9;
            std::printf("\n[SAT] reg5=0x%02x → base=0x%zx\n",
                        (regs && rsz > 5) ? regs[5] : 0, sat_base);
            {
                const size_t base = sat_base;
                if (base + 80 * 8 <= vsz) {
                std::printf("SAT @0x%zx (frame %u) — recorrido POR LINKS desde la entrada 0:\n", base, frame);
                std::printf("%-4s %-4s %-5s %-5s %-4s %-4s %-5s %s\n", "ord", "idx", "x", "y", "w", "h", "link", "tile");
                int idx = 0;
                for (int ord = 0; ord < 80; ++ord) {
                    const uint8_t* e = vram + base + (size_t)idx * 8;
                    const int y     = (((e[0] & 0x03) << 8) | e[1]) - 128;
                    const int hs    = ((e[2] >> 2) & 3) + 1, vs = (e[2] & 3) + 1;
                    const int link  = e[3] & 0x7F;
                    const int tile  = ((e[4] & 0x07) << 8) | e[5];
                    const int x     = (((e[6] & 0x03) << 8) | e[7]) - 128;
                    std::printf("%-4d %-4d %-5d %-5d %-4d %-4d %-5d 0x%03x\n", ord, idx, x, y, hs, vs, link, tile);
                    if (link == 0 || link >= 80) break;   // fin de cadena
                    idx = link;
                }
                }
            }
        } else {
            std::printf("\n[SAT] sin acceso a VRAM (%zu bytes)\n", vsz);
        }
    }
    return 0;
}
