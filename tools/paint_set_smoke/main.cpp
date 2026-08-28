// ---------------------------------------------------------------------------
// paint_set_smoke — valida el matcher de plane SETS (Pintar Fase C): que un
// elemento multi-tile definido con define_plane_set() se sustituye por UN quad
// del asset (bbox completo) por aparición, en la lane correcta, consumiendo
// sus celdas (sin 1×1 dobles) y suprimiendo los tiles originales.
//
// Oráculo de POSICIÓN/SUPRESIÓN (mismo criterio que plane_sub_smoke): definir
// el set cambia el framebuffer EXACTAMENTE donde los tiles miembros estaban en
// pantalla (la supresión revela lo de atrás). Así:
//   1. elegir dos celdas ADYACENTES de Plano A del pick-list (plane_cells) →
//      set 2×1 con ancla en la izquierda.
//   2. define_plane_set → el matcher emite un quad w_tiles=2 en la posición
//      del ancla, en la lane de su prioridad (PlaneCellHit.flags bit2).
//   3. diff del fb vs baseline → las celdas de AMBOS miembros deben cambiar.
//   4. assign_plane(hash del ancla) NO agrega un 1×1 encima del quad (la celda
//      está consumida).
//   5. clear_plane_sets restaura el framebuffer baseline (determinista).
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target paint_set_smoke
//   Run:    bin/paint_set_smoke[.exe]   (toma rutas de tests/test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_components_toml.h"   // bake/parse plane_sets.toml
#include "ayther_core_ffi.h"          // ayther_pack_builder_*

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;
using ayther::PlaneCellHit;

static std::string cfg_quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

// Copia el framebuffer (BGRA/RGB565 — comparamos bytes crudos por celda).
static std::vector<uint8_t> snap_fb(const FrameView* fv) {
    if (!fv || !fv->fb_pixels || !fv->fb_height || !fv->fb_pitch) return {};
    const uint8_t* p = static_cast<const uint8_t*>(fv->fb_pixels);
    return std::vector<uint8_t>(p, p + (size_t)fv->fb_height * fv->fb_pitch);
}

int main() {
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    if (!cfg) { std::fprintf(stderr, "[skip] sin tests/test_config.toml\n"); return 0; }
    std::string core, rom, line; bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) in_rom = true;
        else if (line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = cfg_quoted(line);
        else if (in_rom && rom.empty() && line.find("path") != std::string::npos) rom = cfg_quoted(line);
    }
    core = resolve(core, root); rom = resolve(rom, root);
    if (core.empty() || rom.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }
    std::printf("=== paint_set_smoke ===\ncore: %s\nrom : %s\n\n", core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    constexpr uint16_t START = 1u << 3, RIGHT = 1u << 6;
    for (int i = 0; i < 900; ++i) { s->set_input(0, (i % 24 < 6) ? START : 0); s->step(); }
    s->record_start();
    for (int i = 0; i < 360; ++i) { s->set_input(0, RIGHT | ((i % 48 < 4) ? 1u : 0u)); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[ok] toma: %u frames\n", take.frame_count());

    int fail = 0;
    auto check = [&](bool ok, const char* m) { std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail; };
    // La supresión llega con 1 frame de latencia (canal produce-only): asentar.
    auto settle = [&](uint32_t f) {
        const FrameView* fv = nullptr;
        for (int k = 0; k < 4; ++k) { s->replay_invalidate(); fv = s->replay_seek(take, f); }
        return fv;
    };

    // Frame tardío con pick-list de Plano A poblado (scroll grande → sensible
    // al signo, mismo criterio que plane_sub_smoke).
    uint32_t F = UINT32_MAX;
    for (uint32_t f = take.frame_count() > 60 ? take.frame_count() - 60 : 0; f >= 30; f -= 10) {
        const FrameView* fv = settle(f);
        bool any_a = false;
        for (uint32_t i = 0; fv && i < fv->plane_cell_count; ++i)
            if (fv->plane_cells[i].plane == 0) { any_a = true; break; }
        if (any_a) { F = f; break; }
        if (f < 40) break;
    }
    check(F != UINT32_MAX, "hay un frame con celdas de Plano A en el pick-list");
    if (F == UINT32_MAX) { std::printf("\nFALLÓ\n"); return 1; }
    std::printf("[..] frame F=%u\n", F);

    const FrameView* fv0 = settle(F);
    const int pitch = (int)fv0->fb_pitch, W = (int)fv0->fb_width, H = (int)fv0->fb_height;
    const int bpp = (W > 0) ? pitch / W : 2;
    auto fb0 = snap_fb(fv0);

    // Candidatos: pares ADYACENTES horizontales de Plano A (ancla izquierda).
    // Copia local del pick-list (el FrameView se invalida en cada settle).
    struct Cell { uint64_t hash; int sx, sy; uint8_t flags; };
    std::vector<Cell> cells;
    for (uint32_t i = 0; i < fv0->plane_cell_count; ++i) {
        const PlaneCellHit& pc = fv0->plane_cells[i];
        if (pc.plane == 0) cells.push_back({pc.hash, pc.screen_x, pc.screen_y, pc.flags});
    }
    auto cell_at = [&](int sx, int sy) -> const Cell* {
        for (const Cell& cl : cells) if (cl.sx == sx && cl.sy == sy) return &cl;
        return nullptr;
    };

    // La celda 8×8 en (sx,sy) difiere entre fb0 y fb — la supresión reveló
    // lo de atrás. Un tile puede suprimirse SIN cambiar el fb (lo revelado es
    // idéntico, o un sprite lo tapa): la selección del par lo exige en AMBAS
    // celdas para que el oráculo sea concluyente (criterio plane_sub_smoke).
    auto cell_changed = [&](const std::vector<uint8_t>& fb, int sx, int sy) {
        for (int y = sy; y < sy + 8; ++y)
            for (int x = sx; x < sx + 8; ++x) {
                const size_t o = (size_t)y * pitch + (size_t)x * bpp;
                for (int bi = 0; bi < bpp; ++bi)
                    if (fb0[o + bi] != fb[o + bi]) return true;
            }
        return false;
    };

    // Probar pares hasta hallar uno cuya sustitución (a) emite el quad 2×1 en
    // el ancla y (b) cambia el framebuffer en las DOS celdas miembro (la
    // supresión revela lo de atrás).
    constexpr uint64_t SET_ID = 0xC0FFEE;
    const Cell* anchor = nullptr; const Cell* right = nullptr;
    int quad_idx = -1; uint32_t quad_hi = 0, quad_n = 0;
    for (const Cell& a : cells) {
        if (a.sx < 0 || a.sy < 0 || a.sx + 16 > W || a.sy + 8 > H) continue;
        const Cell* b = cell_at(a.sx + 8, a.sy);
        if (!b) continue;
        ayther::AytherSession::PlaneSetMember ms[2] = {{a.hash, 0, 0}, {b->hash, 1, 0}};
        s->define_plane_set(SET_ID, 0, 2, 1, ms, 2, "dummy_set.png");
        const FrameView* fv = settle(F);
        int qi = -1;
        for (uint32_t j = 0; fv && j < fv->plane_tile_sub_count; ++j) {
            const auto& sub = fv->plane_tile_subs[j];
            if (sub.w_tiles == 2 && sub.h_tiles == 1 &&
                sub.screen_x == (int16_t)a.sx && sub.screen_y == (int16_t)a.sy &&
                !std::strcmp(sub.asset_path, "dummy_set.png")) { qi = (int)j; break; }
        }
        // R-5 (#274): la supresión murió — la prueba de que el matcher
        // CONSUMIÓ las celdas es SceneElement.claimed (el compose no las
        // emite), no un cambio en el fb (que ahora queda completo siempre).
        std::vector<ayther::SceneElement> inv;
        s->scene_inventory(inv);
        auto claimed_at = [&](int sx, int sy) {
            for (const auto& e : inv)
                if (e.layer == 1 && e.x == sx && e.y == sy) return e.claimed != 0;
            return false;
        };
        if (qi >= 0 && claimed_at(a.sx, a.sy) && claimed_at(b->sx, b->sy)) {
            anchor = &a; right = b; quad_idx = qi;
            quad_hi = fv->plane_tile_sub_hi; quad_n = fv->plane_tile_sub_count;
            break;
        }
        s->clear_plane_sets();
        settle(F);
    }
    check(anchor != nullptr, "el matcher emitió el quad 2×1 del set en el ancla (y consumió las celdas)");
    if (!anchor) { std::printf("\nFALLÓ (%d)\n", fail); return 1; }
    std::printf("[..] ancla (%d,%d) hash 0x%016llx · vecino 0x%016llx · quad #%d de %u (hi=%u)\n",
                anchor->sx, anchor->sy, (unsigned long long)anchor->hash,
                (unsigned long long)right->hash, quad_idx, quad_n, quad_hi);

    // Lane según la prioridad VDP del ancla (PlaneCellHit.flags bit2, Fase C).
    const bool anchor_hi = (anchor->flags >> 2) & 1;
    check(anchor_hi ? ((uint32_t)quad_idx >= quad_hi) : ((uint32_t)quad_idx < quad_hi),
          "el quad cayó en la lane de la prioridad del ancla (lo/hi)");

    // R-5: AMBOS miembros están CLAIMED en el inventario (el compose no los
    // emite — el reemplazo de la supresión) y el fb quedó INTACTO (los
    // canales 0x104/0x105 están muertos: el fb es fuente de datos, no lienzo).
    {
        std::vector<ayther::SceneElement> inv;
        s->scene_inventory(inv);
        auto claimed_at = [&](int sx, int sy) {
            for (const auto& e : inv)
                if (e.layer == 1 && e.x == sx && e.y == sy) return e.claimed != 0;
            return false;
        };
        check(claimed_at(anchor->sx, anchor->sy), "la celda del ancla quedó claimed en el inventario");
        check(claimed_at(right->sx, right->sy),  "la celda del vecino quedó claimed en el inventario");
        const FrameView* fvq = settle(F);
        check(snap_fb(fvq) == fb0, "el fb quedó completo (los canales de supresión murieron)");
    }

    // Celda consumida: asignar un 1×1 al hash del ancla NO duplica overlay en
    // la posición del quad (sí puede aparecer en OTRAS apariciones del hash).
    s->assign_plane(anchor->hash, "dummy_1x1.png");
    {
        const FrameView* fv = settle(F);
        bool dup = false;
        for (uint32_t j = 0; fv && j < fv->plane_tile_sub_count; ++j) {
            const auto& sub = fv->plane_tile_subs[j];
            if (sub.w_tiles == 1 && sub.h_tiles == 1 &&
                sub.screen_x == (int16_t)anchor->sx && sub.screen_y == (int16_t)anchor->sy)
                { dup = true; break; }
        }
        check(!dup, "sin 1×1 sobre la celda consumida por el set (assign_plane del ancla)");
    }
    s->unassign_plane(anchor->hash);

    // clear_plane_sets restaura el frame baseline (determinista, sin residuo).
    s->clear_plane_sets();
    {
        const FrameView* fv = settle(F);
        bool quad_left = false;
        for (uint32_t j = 0; fv && j < fv->plane_tile_sub_count; ++j)
            if (fv->plane_tile_subs[j].w_tiles == 2) { quad_left = true; break; }
        check(!quad_left, "clear_plane_sets retiró el quad del set");
        check(snap_fb(fv) == fb0, "el framebuffer volvió al baseline (supresión limpia)");
    }

    // ── P3: tolerancia off-screen + ancla no-primera ─────────────────────
    // Set 3×1 cuyo member[0] es un FANTASMA (hash inexistente) que cae a la
    // IZQUIERDA del área visible (ex = sx-8 < 0): el matcher debe EXCUSARLO
    // (posición esperada fuera de pantalla), anclar en OTRO miembro (el
    // fantasma ni aparece en el frame → valida el orden de anclas por
    // frecuencia) y emitir el quad 3×1 con ORIGEN fuera de pantalla.
    {
        const Cell* ea = nullptr; const Cell* eb = nullptr;
        for (const Cell& a : cells) {
            if (a.sx >= 8) continue;                    // borde izquierdo
            const Cell* b = cell_at(a.sx + 8, a.sy);
            if (!b) continue;
            ea = &a; eb = b; break;
        }
        check(ea != nullptr, "hay un par en el borde izquierdo (caso off-screen)");
        if (ea) {
            ayther::AytherSession::PlaneSetMember ms[3] = {
                {0xFEEDFACEFEEDFACEull, 0, 0},   // fantasma, esperado fuera de pantalla
                {ea->hash, 1, 0},
                {eb->hash, 2, 0}};
            s->define_plane_set(0xBEEF, 0, 3, 1, ms, 3, "dummy_tol.png");
            const FrameView* fv = settle(F);
            bool found = false;
            for (uint32_t j = 0; fv && j < fv->plane_tile_sub_count; ++j) {
                const auto& sub = fv->plane_tile_subs[j];
                if (sub.w_tiles == 3 && sub.h_tiles == 1 &&
                    sub.screen_x == (int16_t)(ea->sx - 8) &&
                    sub.screen_y == (int16_t)ea->sy &&
                    !std::strcmp(sub.asset_path, "dummy_tol.png")) { found = true; break; }
            }
            check(found, "P3: miembro off-screen excusado + ancla no-primera "
                         "(quad 3x1 con origen fuera de pantalla)");
            s->clear_plane_sets();
        }
    }

    // ── El set viene del PACK, no de la API ──────────────────────────────
    // Es el hueco que cerraba plane_sets.toml: hasta acá el catálogo de Pintar
    // sólo existía en la sesión de autoría, así que el .ay entregado NO
    // reproducía ninguna sustitución multi-tile. Se hornea un pack con el MISMO
    // set 2×1 de arriba, se abre, y el quad tiene que aparecer sin que nadie
    // haya llamado define_plane_set.
    {
        s->clear_plane_sets();
        std::vector<ayther::PackPlaneSet> sets(1);
        sets[0].id      = 0xCAFE01;
        sets[0].name    = "Utileria del pack";
        sets[0].type    = "utileria";
        sets[0].plane   = 0;
        sets[0].w_cells = 2;
        sets[0].h_cells = 1;
        sets[0].asset   = "desde_el_pack.png";
        sets[0].members = { { anchor->hash, 0, 0 }, { right->hash, 1, 0 } };
        std::vector<ayther::PackPlaneFont> fonts(1);
        fonts[0] = { 0xF0, "HUD", 1, 1 };

        // Round-trip del formato antes de meterlo al ZIP.
        const std::string toml = ayther::bake_plane_sets_toml(sets, fonts);
        std::vector<ayther::PackPlaneSet>  rs;
        std::vector<ayther::PackPlaneFont> rf;
        const size_t n = ayther::parse_plane_sets_toml(toml, rs, rf);
        check(n == 1 && rs.size() == 1 && rs[0].id == 0xCAFE01 &&
              rs[0].members.size() == 2 &&
              rs[0].members[0].hash == anchor->hash &&
              rs[0].members[1].cx == 1 && rs[0].asset == "desde_el_pack.png" &&
              rf.size() == 1 && rf[0].id == 0xF0,
              "plane_sets.toml: round-trip bake→parse");

        const std::string pack_path =
            (std::filesystem::temp_directory_path() / "ayther_plane_sets_smoke.ay").string();
        AytherPackBuilder* b = ayther_pack_builder_new();
        const std::string man =
            "[pack]\nname = \"plane_sets_smoke\"\nversion = \"1.0.0\"\n"
            "game_id = \"smoke\"\nayther_min = \"0.8.0\"\n"
            "\n[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";
        ayther_pack_builder_add_bytes(b, "manifest.toml",
            reinterpret_cast<const uint8_t*>(man.data()), man.size());
        ayther_pack_builder_add_bytes(b, "plane_sets.toml",
            reinterpret_cast<const uint8_t*>(toml.data()), toml.size());
        char perr[256] = {};
        const bool built = ayther_pack_builder_finish(b, true, pack_path.c_str(),
                                                      perr, sizeof(perr));
        ayther_pack_builder_free(b);
        check(built, "se horneó un .ay con plane_sets.toml");

        if (built) {
            const auto pr = s->set_pack(pack_path);
            check(static_cast<bool>(pr), "la sesión abrió el pack");
            const FrameView* fv = settle(F);
            bool from_pack = false;
            for (uint32_t j = 0; fv && j < fv->plane_tile_sub_count; ++j) {
                const auto& sub = fv->plane_tile_subs[j];
                if (sub.w_tiles == 2 && sub.h_tiles == 1 &&
                    !std::strcmp(sub.asset_path, "desde_el_pack.png")) {
                    from_pack = true; break;
                }
            }
            check(from_pack,
                  "el quad del set aparece DESDE EL PACK (sin define_plane_set)");

            // El gate: en modo Original el matcher no corre (si corriera,
            // suprimiría los originales sin nada que los tape).
            s->set_hd_enabled(false);
            const FrameView* fo = settle(F);
            bool still = false;
            for (uint32_t j = 0; fo && j < fo->plane_tile_sub_count; ++j)
                if (!std::strcmp(fo->plane_tile_subs[j].asset_path, "desde_el_pack.png"))
                    { still = true; break; }
            check(!still, "set_hd_enabled(false) apaga el matcher (modo Original)");
            check(snap_fb(fo) == fb0,
                  "en Original el framebuffer vuelve al baseline (sin agujeros)");
            s->set_hd_enabled(true);
            std::error_code rec;
            std::filesystem::remove(pack_path, rec);
        }
    }

    // -- ANIMACIÓN (#374): el asset del quad sale del paso VIGENTE ------------
    //
    // Dos sets con los MISMOS miembros y assets distintos, más una secuencia
    // que los cicla (2 frames · 3 frames). Cuál de los dos matchea es
    // indistinto —los dos pertenecen a la Animación— y eso es justamente el
    // punto: lo que se dibuja NO es el asset del set que matcheó sino el del
    // paso que toca por reloj. Es lo que permite que el HD tenga más fases que
    // el original.
    if (anchor && right) {
        std::printf("\n-- Animación sobre plane sets --\n");
        s->clear_plane_sets();
        s->clear_plane_sequences();
        constexpr uint64_t A_ID = 0xA1, B_ID = 0xB2, SEQ_ID = 0x5E9;
        ayther::AytherSession::PlaneSetMember ms[2] = {
            { anchor->hash, 0, 0 }, { right->hash, 1, 0 } };
        s->define_plane_set(A_ID, 0, 2, 1, ms, 2, "paso_a.png");
        s->define_plane_set(B_ID, 0, 2, 1, ms, 2, "paso_b.png");
        ayther::AytherSession::PlaneSequenceStep qs[2] = {
            { A_ID, "paso_a.png", 2 },
            { B_ID, "paso_b.png", 3 },
        };
        s->define_plane_sequence(SEQ_ID, qs, 2);

        // El ancla del reloj es el PRIMER frame en que la secuencia se resuelve,
        // así que se mide desde ahí: fase esperada = (f - F) mod 5.
        const uint16_t durs[2] = { 2, 3 };
        int samples = 0, wrong = 0;
        std::string seen;
        for (uint32_t k = 0; k < 10; ++k) {
            const FrameView* fv = settle(F + k);
            const char* got = nullptr;
            for (uint32_t j = 0; fv && j < fv->plane_tile_sub_count; ++j) {
                const auto& sub = fv->plane_tile_subs[j];
                if (sub.screen_x == (int16_t)anchor->sx &&
                    sub.screen_y == (int16_t)anchor->sy && sub.w_tiles == 2)
                    { got = sub.asset_path; break; }
            }
            if (!got) { seen += '.'; continue;  }   // el set no matchea en ese frame
            const uint32_t want =
                ayther::plane_sequence_step_at(durs, 2, k % 5u, 8);
            const char* wanted = want == 0 ? "paso_a.png" : "paso_b.png";
            seen += (want == 0 ? 'a' : 'b');
            ++samples;
            if (std::strcmp(got, wanted) != 0) ++wrong;
        }
        std::printf("[..] fase por frame (a=paso 0 · b=paso 1 · .=sin match): %s\n",
                    seen.c_str());
        check(samples >= 5, "la secuencia matchea en al menos 5 de 10 frames");
        check(wrong == 0, "cada frame dibuja el asset del PASO VIGENTE, no el "
                          "del set que matcheó");

        // Sin la secuencia definida, vuelve a mandar el asset del set.
        s->undefine_plane_sequence(SEQ_ID);
        const FrameView* fv = settle(F);
        bool own = false;
        for (uint32_t j = 0; fv && j < fv->plane_tile_sub_count; ++j) {
            const auto& sub = fv->plane_tile_subs[j];
            if (sub.screen_x != (int16_t)anchor->sx || sub.w_tiles != 2) continue;
            own = !std::strcmp(sub.asset_path, "paso_a.png") ||
                  !std::strcmp(sub.asset_path, "paso_b.png");
            break;
        }
        check(own, "sin Animación, el quad vuelve al asset del propio set");
        s->clear_plane_sequences();
        s->clear_plane_sets();
    }

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
