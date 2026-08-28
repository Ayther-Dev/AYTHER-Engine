// ---------------------------------------------------------------------------
// pose_replay_scan — sonda de diagnóstico (NO es un oráculo): reproduce un rango
// de frames de una grabación con el pose-set del proyecto alimentado al motor
// (set_pose_preview, igual que el Lab) y reporta por frame:
//   · subs aplicados (asset, bbox, espejo) — lo que el viewport muestra
//   · poses que CASI matchean: mejor arreglo, miembros faltantes y dónde quedó
//     su hash en pantalla (delta) — la causa de un "no aplica el gráfico"
//   · sprites sueltos (no reclamados) cuyo centro cae en el bbox de un sub HD
//     → el produce los SUPRIME (la causa de "desaparecen sprites ajenos")
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target pose_replay_scan
//   Args:  <poses.toml> <recording.arp> <frame0> <frame1>
//   Env:   AYTHER_PROBE_ROM = ROM del juego (el core sale de test_config.toml)
//
// ⚠ RIESGO DE DRIFT (#148): `match_pose` de este tool es una RÉPLICA del
// matcher del motor (pose_resolve/produce_frame) para poder diagnosticar los
// near-misses — el motor no expone por qué NO matcheó. Si el matcher del motor
// cambia (arreglos espejados, tolerancia off-screen, claims) y esta réplica no
// lo sigue, el diagnóstico MIENTE en silencio. Guardia barata: por frame se
// cross-chequean réplica↔motor (pose CON asset y dims que la réplica completa
// debe tener [sub] del motor, y viceversa) y toda discrepancia se imprime como
// [DRIFT] — si aparece, actualizar match_pose antes de confiar en el reporte.
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include <stb_image.h>          // impl linkeada desde el engine (dump AYTHER_SCAN_DUMP)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif
using ayther::FrameView;

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}
static std::string basename_of(const std::string& p) {
    const auto s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

struct PoseDef {
    std::string name, asset;
    std::vector<uint64_t> hashes;
    std::vector<std::pair<int16_t, int16_t>> rel;
    std::vector<std::pair<int16_t, int16_t>> dims;   ///< px por miembro ("" = sin dims)
    std::vector<uint8_t> flips;                       ///< #221: flips SAT por miembro
    bool flip_h = false, flip_v = false;             ///< flip de presentación (preview/export)
    /// Cara del ASSET (latcheada al asignar; ausente = hereda flip_h/v — la
    /// MISMA migración que catalog_io, reporte 2026-07-24).
    bool asset_flip_h = false, asset_flip_v = false;
    bool has_asset_flip = false;
    uint8_t ref_rgb[3] = { 0, 0, 0 };                ///< referencia tinte E1 ({0,0,0} = sin ref)
};

// Parser mínimo de poses.toml (mismo formato que escribe catalog_io.cpp).
static std::vector<PoseDef> load_poses(const std::string& path) {
    std::vector<PoseDef> out;
    std::ifstream f(path);
    std::string line;
    PoseDef cur;
    auto flush = [&]() {
        // Migración asset_flip ausente → hereda el flip de presentación
        // (mismo criterio que catalog_io.cpp).
        if (!cur.has_asset_flip) {
            cur.asset_flip_h = cur.flip_h;
            cur.asset_flip_v = cur.flip_v;
        }
        if (!cur.hashes.empty()) out.push_back(cur);
        cur = PoseDef{};
    };
    while (std::getline(f, line)) {
        if (line.find("[[pose]]") != std::string::npos) { flush(); continue; }
        auto starts = [&](const char* k) {
            return line.rfind(k, 0) == 0;
        };
        if (starts("name")) cur.name = toml_quoted(line);
        else if (starts("default_asset")) cur.asset = toml_quoted(line);
        else if (starts("hashes")) {
            const std::string v = toml_quoted(line);
            size_t p = 0;
            while (p < v.size()) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                cur.hashes.push_back(std::strtoull(v.substr(p, e - p).c_str(), nullptr, 16));
                p = e + 1;
            }
        } else if (starts("flips")) {
            const std::string v = toml_quoted(line);
            size_t p = 0;
            while (p < v.size()) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                cur.flips.push_back(static_cast<uint8_t>(
                    std::atoi(v.substr(p, e - p).c_str()) & 3));
                p = e + 1;
            }
        } else if (starts("rel") || starts("dims")) {
            auto& dst = starts("rel") ? cur.rel : cur.dims;
            const std::string v = toml_quoted(line);
            size_t p = 0;
            while (p < v.size()) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                const std::string it = v.substr(p, e - p);
                const size_t c = it.find(',');
                if (c != std::string::npos)
                    dst.push_back({ (int16_t)std::atoi(it.substr(0, c).c_str()),
                                    (int16_t)std::atoi(it.substr(c + 1).c_str()) });
                p = e + 1;
            }
        } else if (starts("asset_flip")) {
            const std::string v = toml_quoted(line);
            cur.asset_flip_h   = v.find('h') != std::string::npos;
            cur.asset_flip_v   = v.find('v') != std::string::npos;
            cur.has_asset_flip = true;
        } else if (starts("flip")) {
            const std::string v = toml_quoted(line);
            cur.flip_h = v.find('h') != std::string::npos;
            cur.flip_v = v.find('v') != std::string::npos;
        } else if (starts("ref")) {
            const std::string v = toml_quoted(line);   // "r,g,b" (0-255)
            const size_t c1 = v.find(',');
            const size_t c2 = c1 == std::string::npos ? std::string::npos
                                                      : v.find(',', c1 + 1);
            if (c1 != std::string::npos && c2 != std::string::npos) {
                cur.ref_rgb[0] = (uint8_t)std::atoi(v.c_str());
                cur.ref_rgb[1] = (uint8_t)std::atoi(v.c_str() + c1 + 1);
                cur.ref_rgb[2] = (uint8_t)std::atoi(v.c_str() + c2 + 1);
            }
        }
    }
    flush();
    return out;
}

// Dims reales por hash desde los thumbs 1:1 del proyecto (_sprite_thumb/
// <hash>_pN.png) — valida la hipótesis "dims fallback": con las dims REALES
// del miembro ausente, ¿la tolerancia off-screen habría aplicado?
#include <filesystem>
#include <unordered_map>
static std::unordered_map<uint64_t, std::pair<int, int>> load_thumb_dims(const std::string& dir) {
    std::unordered_map<uint64_t, std::pair<int, int>> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        const std::string stem = it->path().stem().string();   // <hash>_pN
        const size_t us = stem.find('_');
        if (us == std::string::npos) continue;
        const uint64_t h = std::strtoull(stem.substr(0, us).c_str(), nullptr, 16);
        std::ifstream f(it->path(), std::ios::binary);
        uint8_t hdr[24];
        if (!f.read(reinterpret_cast<char*>(hdr), 24)) continue;
        const int w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
        const int hh = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
        if (h && w > 0 && hh > 0) out.emplace(h, std::make_pair(w, hh));
    }
    return out;
}
static std::unordered_map<uint64_t, std::pair<int, int>> g_thumb_dims;

// AYTHER_SCAN_DUMP=<dir>: composición final por SOFTWARE imitando al renderer
// (base fb_pixels → subs HD por slot DESC con flips → overlay alpha=255) → PPM
// por frame. Sirve para VER lo que el viewport mostraría (p.ej. si el wipe de
// una transición tapa el HD como tapa al original).
// AYTHER_SCAN_DUMP_SCALE=<S> (default 3): supersampleo — como el viewport dibuja
// a resolución de pantalla, el HD se muestrea a S× (bilinear al MINIFICAR,
// nearest al ampliar — la misma regla por-quad de VkSprite); base y overlay van
// nearest ×S (pixel-art del emu).
struct HdImage { stbi_uc* px = nullptr; int w = 0, h = 0; };
static std::unordered_map<std::string, HdImage> g_hd_cache;
static const HdImage& hd_image(const char* path) {
    auto it = g_hd_cache.find(path);
    if (it != g_hd_cache.end()) return it->second;
    HdImage img;
    int n = 0;
    img.px = stbi_load(path, &img.w, &img.h, &n, 4);
    return g_hd_cache.emplace(path, img).first->second;
}
static void dump_composite(const FrameView* fv, const std::string& path, int S) {
    if (!fv || !fv->fb_pixels) return;
    if (S < 1) S = 1;
    const unsigned W = fv->fb_width, H = fv->fb_height;
    const unsigned OW = W * S, OH = H * S;
    const size_t pitch = fv->fb_pitch;
    std::vector<uint8_t> out((size_t)OW * OH * 3);
    for (unsigned y = 0; y < OH; ++y) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(
            static_cast<const uint8_t*>(fv->fb_pixels) + (size_t)(y / S) * pitch);
        for (unsigned x = 0; x < OW; ++x) {
            const uint16_t p = row[x / S];
            const uint8_t r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
            out[((size_t)y * OW + x) * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            out[((size_t)y * OW + x) * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            out[((size_t)y * OW + x) * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
    }
    // Subs HD con el orden del renderer: slot DESCENDENTE (menor = al frente).
    std::vector<uint32_t> order(fv->sprite_sub_count);
    for (uint32_t i = 0; i < fv->sprite_sub_count; ++i) order[i] = i;
    if (fv->sprite_sub_slot)
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return fv->sprite_sub_slot[a] > fv->sprite_sub_slot[b];
        });
    for (uint32_t oi : order) {
        const auto& sub = fv->sprite_subs[oi];
        const HdImage& hd = hd_image(sub.asset_path);
        if (!hd.px) continue;
        const uint8_t flip = fv->sprite_sub_flips ? fv->sprite_sub_flips[oi] : 0;
        // E1 cromático: el renderer tinta el HD por canal (Q2.6, 64 = 1.0).
        float tint[3] = { 1.0f, 1.0f, 1.0f };
        if (fv->sprite_sub_tint)
            for (int c = 0; c < 3; ++c)
                tint[c] = fv->sprite_sub_tint[oi * 3 + c] / 64.0f;
        const int dx = sub.screen_x * S, dy = sub.screen_y * S;
        const int dw = (sub.w_px ? sub.w_px : sub.w_tiles * 8) * S;
        const int dh = (sub.h_px ? sub.h_px : sub.h_tiles * 8) * S;
        const bool minified = hd.w > dw || hd.h > dh;   // regla por-quad de VkSprite
        for (int yy = 0; yy < dh; ++yy) for (int xx = 0; xx < dw; ++xx) {
            const int px = dx + xx, py = dy + yy;
            if (px < 0 || px >= (int)OW || py < 0 || py >= (int)OH) continue;
            const int fx = (flip & 1) ? dw - 1 - xx : xx;
            const int fy = (flip & 2) ? dh - 1 - yy : yy;
            float rgba[4];
            if (minified) {
                // Bilinear (como el sampler LINEAR del renderer al minificar).
                const float sxf = (fx + 0.5f) * hd.w / dw - 0.5f;
                const float syf = (fy + 0.5f) * hd.h / dh - 0.5f;
                const int x0 = sxf < 0 ? 0 : (int)sxf, y0 = syf < 0 ? 0 : (int)syf;
                const int x1 = x0 + 1 < hd.w ? x0 + 1 : x0;
                const int y1 = y0 + 1 < hd.h ? y0 + 1 : y0;
                const float tx = sxf - x0 < 0 ? 0 : sxf - x0;
                const float ty = syf - y0 < 0 ? 0 : syf - y0;
                const stbi_uc* p00 = hd.px + ((size_t)y0 * hd.w + x0) * 4;
                const stbi_uc* p10 = hd.px + ((size_t)y0 * hd.w + x1) * 4;
                const stbi_uc* p01 = hd.px + ((size_t)y1 * hd.w + x0) * 4;
                const stbi_uc* p11 = hd.px + ((size_t)y1 * hd.w + x1) * 4;
                for (int k = 0; k < 4; ++k)
                    rgba[k] = (p00[k] * (1 - tx) + p10[k] * tx) * (1 - ty)
                            + (p01[k] * (1 - tx) + p11[k] * tx) * ty;
            } else {
                const int sx = fx * hd.w / dw, sy = fy * hd.h / dh;   // nearest
                const stbi_uc* sp = hd.px + ((size_t)sy * hd.w + sx) * 4;
                for (int k = 0; k < 4; ++k) rgba[k] = sp[k];
            }
            const float a = rgba[3] / 255.0f;
            for (int k = 0; k < 3; ++k) {
                uint8_t& d = out[((size_t)py * OW + px) * 3 + k];
                const float v = rgba[k] * tint[k];   // >255 satura (flash)
                d = (uint8_t)((v > 255.0f ? 255.0f : v) * a + d * (1 - a));
            }
        }
    }
    // #345: el overlay del compose por supresión se borró con su maquinaria.
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << OW << " " << OH << "\n255\n";
    f.write(reinterpret_cast<const char*>(out.data()), out.size());
}

// -- Réplica del matcher del motor (PoseSetSubstitutor::resolve, instanciado) --
// para poder DIAGNOSTICAR: el motor sólo publica los subs que SÍ matchearon;
// esta réplica además reporta el mejor arreglo parcial de las que no.
// Two-phase (2026-07-19, defecto #8): match_pose ENUMERA candidatas completas
// SIN reclamar; el driver hace la asignación greedy por EVIDENCIA (hits reales
// DESC, desempate estable = orden por cantidad de miembros) — igual que el
// motor tras el fix del robo de ancla en el borde.
struct InstMatch {
    bool complete = false;
    uint8_t mirror = 0;
    int ox = 0, oy = 0;
    std::vector<int> member_occ;                 // -1 = ausente (tolerado offscreen)
    size_t hits = 0;                             // del mejor arreglo parcial
    std::vector<std::string> missing_notes;      // diagnóstico por miembro faltante
};

static bool member_invisible(int x, int y, int wpx, int hpx, int W, int H) {
    return x >= W || y >= H || x + wpx <= 0 || y + hpx <= 0;
}

// Devuelve todas las instancias COMPLETAS (SIN reclamar — el driver reclama en
// la fase greedy), y si no hubo ninguna, el mejor parcial con notas de
// diagnóstico. `claimed` solo se LEE (pre-reclamos de la fase actual).
static std::vector<InstMatch> match_pose(const PoseDef& p, const FrameView& fv,
                                         const std::vector<char>& claimed, int W, int H) {
    std::vector<InstMatch> out;
    const size_t nm = p.hashes.size();
    if (nm == 0 || p.rel.size() != nm) return out;

    // Dims por hash del frame vivo (fallback: primer miembro visto).
    std::vector<std::pair<int, int>> mpx(nm, { 8, 8 });
    {
        auto dims_of = [&](uint64_t h, int& w, int& hh) {
            for (uint32_t i = 0; i < fv.sprite_occ_count; ++i)
                if (fv.sprite_occs[i].hash == h) {
                    w = fv.sprite_occs[i].w_tiles * 8;
                    hh = fv.sprite_occs[i].h_tiles * 8;
                    return true;
                }
            return false;
        };
        int fw = 8, fh = 8;
        for (size_t i = 0; i < nm && !dims_of(p.hashes[i], fw, fh); ++i) {}
        for (size_t i = 0; i < nm; ++i) {
            int w = fw, hh = fh;
            if (!dims_of(p.hashes[i], w, hh)) {
                // Miembro sin occ este frame: dims REALES desde el thumb si las
                // hay (la mejora bajo prueba); si no, el fallback del motor.
                auto td = g_thumb_dims.find(p.hashes[i]);
                if (td != g_thumb_dims.end()) { w = td->second.first; hh = td->second.second; }
            }
            mpx[i] = { w, hh };
        }
    }
    int Wpx = 0, Hpx = 0;
    for (size_t i = 0; i < nm; ++i) {
        Wpx = std::max(Wpx, p.rel[i].first + mpx[i].first);
        Hpx = std::max(Hpx, p.rel[i].second + mpx[i].second);
    }
    using Arr = std::vector<std::pair<int, int>>;
    Arr arrs[4] = { Arr(nm), Arr(nm), Arr(nm), Arr(nm) };
    for (size_t i = 0; i < nm; ++i) {
        const int x = p.rel[i].first, y = p.rel[i].second;
        arrs[0][i] = { x, y };
        arrs[1][i] = { Wpx - x - mpx[i].first, y };
        arrs[2][i] = { x, Hpx - y - mpx[i].second };
        arrs[3][i] = { Wpx - x - mpx[i].first, Hpx - y - mpx[i].second };
    }
    InstMatch best_partial;
    for (int ai = 0; ai < 4; ++ai) {
        bool dup = false;
        for (int bi = 0; bi < ai && !dup; ++bi) dup = arrs[ai] == arrs[bi];
        if (dup) continue;
        const Arr& rel = arrs[ai];
        std::vector<std::pair<int, int>> tried;
        for (uint32_t a = 0; a < fv.sprite_occ_count; ++a) {
            if (claimed[a]) continue;
            size_t k = nm;
            for (size_t i = 0; i < nm; ++i)
                if (p.hashes[i] == fv.sprite_occs[a].hash) { k = i; break; }
            if (k == nm) continue;
            const int ox = fv.sprite_occs[a].screen_x - rel[k].first;
            const int oy = fv.sprite_occs[a].screen_y - rel[k].second;
            bool seen = false;
            for (auto& t : tried) if (t.first == ox && t.second == oy) { seen = true; break; }
            if (seen) continue;
            tried.push_back({ ox, oy });
            std::vector<int> cand(nm, -1);
            std::vector<std::string> notes;
            size_t hits = 0;
            bool ok = true;
            for (size_t i = 0; i < nm; ++i) {
                const int wx = ox + rel[i].first, wy = oy + rel[i].second;
                int got = -1;
                for (uint32_t q = 0; q < fv.sprite_occ_count; ++q) {
                    const auto& o = fv.sprite_occs[q];
                    if (claimed[q] || o.hash != p.hashes[i]) continue;
                    bool taken = false;
                    for (int c : cand) if (c == (int)q) { taken = true; break; }
                    if (taken) continue;
                    if (o.screen_x == wx && o.screen_y == wy) { got = (int)q; break; }
                }
                if (got >= 0) { cand[i] = got; ++hits; continue; }
                if (member_invisible(wx, wy, mpx[i].first, mpx[i].second, W, H)) {
                    char n[128];
                    std::snprintf(n, sizeof(n), "m%zu %016llx esperado en (%d,%d) FUERA de pantalla (tolerado)",
                                  i, (unsigned long long)p.hashes[i], wx, wy);
                    notes.push_back(n);
                    continue;   // tolerado
                }
                // Diagnóstico: ¿el hash está en pantalla en otra posición?
                char n[160];
                int found_dx = 0, found_dy = 0, found_n = 0;
                for (uint32_t q = 0; q < fv.sprite_occ_count; ++q)
                    if (fv.sprite_occs[q].hash == p.hashes[i]) {
                        if (!found_n) { found_dx = fv.sprite_occs[q].screen_x - wx;
                                        found_dy = fv.sprite_occs[q].screen_y - wy; }
                        ++found_n;
                    }
                if (found_n)
                    std::snprintf(n, sizeof(n), "m%zu %016llx esperado (%d,%d): hash presente x%d, delta (%+d,%+d)%s",
                                  i, (unsigned long long)p.hashes[i], wx, wy, found_n,
                                  found_dx, found_dy,
                                  claimed.empty() ? "" : "");
                else
                    std::snprintf(n, sizeof(n), "m%zu %016llx esperado (%d,%d): hash AUSENTE del frame",
                                  i, (unsigned long long)p.hashes[i], wx, wy);
                notes.push_back(n);
                ok = false;
            }
            if (ok && hits > 0) {
                InstMatch m;
                m.complete = true;
                m.mirror = (uint8_t)ai;
                m.ox = ox; m.oy = oy;
                m.member_occ = cand;
                m.hits = hits;
                out.push_back(std::move(m));
            } else if (hits > best_partial.hits) {
                best_partial.hits = hits;
                best_partial.mirror = (uint8_t)ai;
                best_partial.ox = ox; best_partial.oy = oy;
                best_partial.member_occ = cand;
                best_partial.missing_notes = notes;
            }
        }
    }
    if (out.empty() && best_partial.hits > 0) out.push_back(std::move(best_partial));
    return out;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "uso: pose_replay_scan <poses.toml> <rec.arp> <f0> <f1>\n");
        return 2;
    }
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    std::string core, rom, line;
    while (std::getline(cfg, line))
        if (line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = toml_quoted(line);
    core = resolve(core, root);
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) rom = er;
    if (rom.empty()) { std::fprintf(stderr, "[FAIL] falta AYTHER_PROBE_ROM\n"); return 2; }

    const std::string poses_path = argv[1], rec_path = argv[2];
    const uint32_t f0 = (uint32_t)std::strtoul(argv[3], nullptr, 10);
    const uint32_t f1 = (uint32_t)std::strtoul(argv[4], nullptr, 10);

    const std::vector<PoseDef> poses = load_poses(poses_path);
    {
        // graphics/_sprite_thumb junto al poses.toml (dims 1:1 por hash).
        const std::filesystem::path tp =
            std::filesystem::path(poses_path).parent_path() / "graphics" / "_sprite_thumb";
        g_thumb_dims = load_thumb_dims(tp.string());
    }
    std::printf("=== pose_replay_scan ===\nposes: %zu de %s\nthumbs: %zu hashes con dims\nrec  : %s\nrango: %u..%u\n\n",
                poses.size(), poses_path.c_str(), g_thumb_dims.size(), rec_path.c_str(), f0, f1);

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    // Pose-set como lo alimenta el Lab (todas con default_asset → hd=true).
    // Dims por miembro: las del poses.toml; si faltan (pose pre-migración), las
    // reales de los thumbs — simula el backfill del Lab para ejercitar el motor.
    std::vector<ayther::AytherSession::PosePreview> pvs;
    for (const auto& p : poses) {
        ayther::AytherSession::PosePreview pv;
        pv.hashes = p.hashes;
        pv.asset = p.asset;
        pv.hd = !p.asset.empty();
        pv.flip_h = p.asset_flip_h;   // cara del ASSET (no el Flip de presentación)
        pv.flip_v = p.asset_flip_v;
        pv.ref_rgb[0] = p.ref_rgb[0];   // referencia autorada del tinte E1
        pv.ref_rgb[1] = p.ref_rgb[1];
        pv.ref_rgb[2] = p.ref_rgb[2];
        for (auto& rl : p.rel) { pv.rel_x.push_back(rl.first); pv.rel_y.push_back(rl.second); }
        if (p.flips.size() == p.hashes.size()) pv.mem_flips = p.flips;   // #221
        if (p.dims.size() == p.hashes.size()) {
            for (auto& d : p.dims) { pv.dim_w.push_back(d.first); pv.dim_h.push_back(d.second); }
        } else {
            bool all = true;
            std::vector<int16_t> dw, dh;
            for (uint64_t h : p.hashes) {
                auto it = g_thumb_dims.find(h);
                if (it == g_thumb_dims.end()) { all = false; break; }
                dw.push_back((int16_t)it->second.first);
                dh.push_back((int16_t)it->second.second);
            }
            if (all) { pv.dim_w = std::move(dw); pv.dim_h = std::move(dh); }
        }
        pvs.push_back(std::move(pv));
    }
    s->set_pose_preview(pvs);
    // Poses con dims completas en el preview: solo esas participan del check
    // [DRIFT] (sin dims el motor legítimamente no completa donde la réplica
    // con thumbs sí — ese caso ya lo reporta [fix-dims]).
    std::vector<char> pose_has_dims(poses.size(), 0);
    for (size_t i = 0; i < poses.size(); ++i)
        pose_has_dims[i] = !pvs[i].dim_w.empty();

    // Orden de reclamo del motor: más miembros primero. Poses SIN asset no
    // entran: el motor no las recibe (set_pose_preview filtra asset vacío) —
    // sin esto, una captura sin nombre matcheaba EXACTO en su frame de origen
    // y le robaba las occs a las poses reales solo en la réplica (falso DRIFT).
    std::vector<const PoseDef*> order;
    for (const auto& p : poses) if (!p.asset.empty()) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(),
                     [](const PoseDef* a, const PoseDef* b) {
                         return a->hashes.size() > b->hashes.size();
                     });

    for (uint32_t f = f0; f <= f1 && f < rec->frame_count(); ++f) {
        const FrameView* fv = s->replay_seek(*rec, f);
        if (!fv) { std::printf("f%-5u [FAIL seek]\n", f); continue; }
        const int W = fv->fb_width > 0 ? (int)fv->fb_width : 320;
        const int H = fv->fb_height > 0 ? (int)fv->fb_height : 240;

        // Luma CRAM por línea de paleta (mismo cálculo que E1, sin peak-hold):
        // referencia para leer el fundido/cambio de paleta del frame.
        char pal_line[224] = "";
        {
            size_t csz = 0;
            const uint8_t* cram = s->color_ram(&csz);
            if (cram && csz >= 128) {
                float pl[4];
                int   rgb[4][3];   // promedio por canal ×255 (== ref autorada)
                for (int p = 0; p < 4; ++p) {
                    double r = 0.0, g = 0.0, b = 0.0;
                    for (int c = 1; c < 16; ++c) {
                        const size_t ce = (size_t)p * 32 + (size_t)c * 2;
                        const uint16_t v = (uint16_t)(cram[ce] | (cram[ce + 1] << 8));
                        r += v & 7; g += (v >> 3) & 7; b += (v >> 6) & 7;
                    }
                    rgb[p][0] = (int)(r / (15.0 * 7.0) * 255.0 + 0.5);
                    rgb[p][1] = (int)(g / (15.0 * 7.0) * 255.0 + 0.5);
                    rgb[p][2] = (int)(b / (15.0 * 7.0) * 255.0 + 0.5);
                    pl[p] = (float)((0.299 * rgb[p][0] + 0.587 * rgb[p][1]
                                   + 0.114 * rgb[p][2]) / 255.0);
                }
                std::snprintf(pal_line, sizeof(pal_line),
                              "  cram-luma p0=%.2f p1=%.2f p2=%.2f p3=%.2f"
                              "  rgb p0=%d,%d,%d p1=%d,%d,%d p2=%d,%d,%d p3=%d,%d,%d\n",
                              pl[0], pl[1], pl[2], pl[3],
                              rgb[0][0], rgb[0][1], rgb[0][2], rgb[1][0], rgb[1][1], rgb[1][2],
                              rgb[2][0], rgb[2][1], rgb[2][2], rgb[3][0], rgb[3][1], rgb[3][2]);
            }
        }

        // Subs REALES publicados por el motor este frame.
        std::string subs_line = pal_line;
        for (uint32_t i = 0; i < fv->sprite_sub_count; ++i) {
            const auto& sb = fv->sprite_subs[i];
            // Paletas de las occs cuyo centro cae en el BBOX del sub — ⚠ INCLUYE
            // AJENOS solapados (Tyris montada cuenta las partes del Dragón y
            // viceversa; un flash mete a los enemigos). NO es la distribución de
            // paletas de los MIEMBROS — leerla así fabricó el falso "Tyris
            // p0+p1" del issue #149 (verificado 2026-07-15: Tyris es toda p0).
            // Para miembros reales: con #149, una pose mixta emite un [sub] por
            // grupo de línea — la cantidad de [sub] del mismo asset ES la señal.
            const int sx1 = sb.screen_x + (sb.w_px ? sb.w_px : sb.w_tiles * 8);
            const int sy1 = sb.screen_y + (sb.h_px ? sb.h_px : sb.h_tiles * 8);
            int pal_seen[4] = { 0, 0, 0, 0 };
            for (uint32_t o = 0; o < fv->sprite_occ_count; ++o) {
                const auto& oc = fv->sprite_occs[o];
                const int cx = oc.screen_x + oc.w_tiles * 4, cy = oc.screen_y + oc.h_tiles * 4;
                if (cx >= sb.screen_x && cx < sx1 && cy >= sb.screen_y && cy < sy1)
                    ++pal_seen[oc.palette & 3];
            }
            // Tinte E1 cromático (Q2.6, 64 = 1.0) — se imprime ×100 (100 = neutro).
            int tp[3] = { 100, 100, 100 };
            if (fv->sprite_sub_tint)
                for (int c = 0; c < 3; ++c)
                    tp[c] = fv->sprite_sub_tint[i * 3 + c] * 100 / 64;
            char t[256];
            std::snprintf(t, sizeof(t),
                          "  [sub] %s @(%d,%d) %ux%upx mirror=%u slot=%u pal=%d tint=%d/%d/%d bbox-pals=%d/%d/%d/%d\n",
                          basename_of(sb.asset_path).c_str(), sb.screen_x, sb.screen_y,
                          sb.w_px, sb.h_px,
                          fv->sprite_sub_flips ? fv->sprite_sub_flips[i] : 0,
                          fv->sprite_sub_slot ? fv->sprite_sub_slot[i] : 255,
                          sb.palette == 0xFF ? -1 : (int)sb.palette,
                          tp[0], tp[1], tp[2],
                          pal_seen[0], pal_seen[1], pal_seen[2], pal_seen[3]);
            subs_line += t;
        }

        // Réplica del matcher para diagnóstico (claims compartidos entre poses).
        std::vector<char> claimed(fv->sprite_occ_count, 0);
        std::string diag;
        // [DRIFT] réplica↔motor: assets publicados por el motor este frame vs
        // completados por la réplica (solo poses con asset Y dims — ver header).
        std::vector<std::string> engine_assets, replica_assets;
        for (uint32_t i = 0; i < fv->sprite_sub_count; ++i)
            engine_assets.push_back(basename_of(fv->sprite_subs[i].asset_path));
        auto has = [](const std::vector<std::string>& v, const std::string& x) {
            return std::find(v.begin(), v.end(), x) != v.end();
        };
        // FASE 1: candidatas completas de TODAS las poses, sin reclamar.
        struct PoseCand { const PoseDef* pose; InstMatch m; };
        std::vector<PoseCand> cands;
        for (const PoseDef* p : order)
            for (auto& m : match_pose(*p, *fv, claimed, W, H))
                if (m.complete) cands.push_back({ p, std::move(m) });
        // FASE 2: greedy por evidencia — hits reales DESC; sort estable → a
        // igual hits queda el orden de fase 1 (más miembros primero). Espejo
        // del motor (vram_sprite.rs resolve()).
        std::stable_sort(cands.begin(), cands.end(),
                         [](const PoseCand& a, const PoseCand& b) { return a.m.hits > b.m.hits; });
        std::vector<const PoseDef*> matched;
        for (const auto& c : cands) {
            bool free_members = true;
            for (int q : c.m.member_occ)
                if (q >= 0 && claimed[q]) { free_members = false; break; }
            if (!free_members) continue;
            for (int q : c.m.member_occ) if (q >= 0) claimed[q] = 1;
            matched.push_back(c.pose);
            if (!c.pose->asset.empty() && pose_has_dims[(size_t)(c.pose - &poses[0])])
                replica_assets.push_back(basename_of(c.pose->asset));
            // La réplica (con dims reales de thumbs) completó: si el motor
            // NO publicó ningún sub, el fix de dims habría aplicado acá.
            if (fv->sprite_sub_count == 0) {
                char t[160];
                std::snprintf(t, sizeof(t),
                              "  [fix-dims] %-24s completaría: arr=%u origen(%d,%d)\n",
                              c.pose->name.c_str(), c.m.mirror, c.m.ox, c.m.oy);
                diag += t;
            }
        }
        // Diagnóstico [casi]: poses SIN instancia aceptada, evaluadas con el
        // claimed FINAL (así "hash presente delta (+0,+0)" delata al ladrón).
        for (const PoseDef* p : order) {
            if (std::find(matched.begin(), matched.end(), p) != matched.end()) continue;
            for (const auto& m : match_pose(*p, *fv, claimed, W, H)) {
                if (m.complete) continue;                      // anomalía rara: la ignora
                if (m.hits * 2 < p->hashes.size()) continue;   // parcial débil: ruido
                char t[160];
                std::snprintf(t, sizeof(t), "  [casi] %-28s arr=%u origen(%d,%d) %zu/%zu miembros:\n",
                              p->name.c_str(), m.mirror, m.ox, m.oy, m.hits, p->hashes.size());
                diag += t;
                for (const auto& n : m.missing_notes) { diag += "         "; diag += n; diag += "\n"; }
            }
        }
        // [DRIFT]: discrepancia réplica↔motor — el matcher del motor cambió y
        // esta réplica quedó atrás (o al revés). Ver el header del tool.
        for (const auto& a : replica_assets)
            if (!has(engine_assets, a))
                diag += "  [DRIFT] la réplica completa \"" + a
                      + "\" pero el motor NO publicó su sub\n";
        for (const auto& a : engine_assets)
            if (!has(replica_assets, a)) {
                // Solo si ALGUNA pose chequeable (asset+dims) usa ese asset —
                // sin dims la réplica no participa y el silencio es esperado.
                bool checkable = false;
                for (size_t i = 0; i < poses.size(); ++i)
                    if (pose_has_dims[i] && !poses[i].asset.empty()
                        && basename_of(poses[i].asset) == a) { checkable = true; break; }
                if (checkable)
                    diag += "  [DRIFT] el motor publica sub de \"" + a
                          + "\" pero la réplica no la completa\n";
            }

        // Sueltos suprimidos por la limpieza de región HD (regla del produce:
        // centro dentro del bbox del sub Y hash MIEMBRO de la pose del sub).
        // Los ajenos (enemigos) ya no caen; si un [SUPR] aparece, es un
        // fragmento mid-frame del mismo personaje (el propósito original).
        std::string supr;
        for (uint32_t i = 0; i < fv->sprite_occ_count; ++i) {
            if (claimed[i]) continue;
            const auto& o = fv->sprite_occs[i];
            const int cx = o.screen_x + o.w_tiles * 4, cy = o.screen_y + o.h_tiles * 4;
            for (uint32_t sI = 0; sI < fv->sprite_sub_count; ++sI) {
                const auto& sb = fv->sprite_subs[sI];
                bool member = false;
                for (const auto& p : poses)
                    if (p.asset == sb.asset_path &&
                        std::find(p.hashes.begin(), p.hashes.end(), o.hash)
                            != p.hashes.end()) { member = true; break; }
                if (!member) continue;
                const int w = sb.w_px ? sb.w_px : sb.w_tiles * 8;
                const int h = sb.h_px ? sb.h_px : sb.h_tiles * 8;
                if (cx >= sb.screen_x && cx < sb.screen_x + w &&
                    cy >= sb.screen_y && cy < sb.screen_y + h) {
                    char t[160];
                    std::snprintf(t, sizeof(t),
                                  "  [SUPR] occ %016llx @(%d,%d) %ux%u dentro de %s\n",
                                  (unsigned long long)o.hash, o.screen_x, o.screen_y,
                                  o.w_tiles * 8, o.h_tiles * 8,
                                  basename_of(sb.asset_path).c_str());
                    supr += t;
                    break;
                }
            }
        }

        std::printf("f%-5u occs=%-3u subs=%u\n%s%s%s", f, fv->sprite_occ_count,
                    fv->sprite_sub_count, subs_line.c_str(), diag.c_str(), supr.c_str());

        if (const char* dd = ayther::env_get("AYTHER_SCAN_DUMP")) {
            const char* sc = ayther::env_get("AYTHER_SCAN_DUMP_SCALE");
            char pp[512];
            std::snprintf(pp, sizeof(pp), "%s/f%04u.ppm", dd, f);
            dump_composite(fv, pp, sc ? std::atoi(sc) : 3);
        }
    }
    return 0;
}
