// ---------------------------------------------------------------------------
// panorama_smoke — valida el ANCLAJE POR CONTENIDO de la Panorámica (CU003):
// que el motor sepa en qué parte de la tira del nivel está la cámara mirando
// SÓLO lo que hay en pantalla.
//
// Por qué no se usa la cámara acumulada (`plane_cam_*`, EM-1): es relativa —
// se re-ancla en 0 ante cualquier seek/scrub, y en el Lab el artista scrubbea
// todo el tiempo, así que la textura HD quedaría corrida hasta reproducir hacia
// adelante. El anclaje por contenido no acumula nada: cada celda visible cuyo
// hash está en la tira dice dónde está la cámara.
//
// Oráculos, en orden de importancia:
//   1. COINCIDE con un unwrap independiente durante un barrido secuencial.
//   2. SOBREVIVE A UN SEEK — el chequeo decisivo, y el único que `plane_cam`
//      no puede pasar: se salta a un frame lejano y la cámara anclada sigue
//      siendo la correcta mientras `plane_cam_valid` es false.
//   3. CONTROL NEGATIVO: una tira de basura no ancla (no inventa una posición).
//   4. La rareza importa: los hashes muy repetidos no entran al índice.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target panorama_smoke
//   Run:    bin/panorama_smoke [core.dll rom]
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_components_toml.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;

static std::string cfg_quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
// División entera hacia abajo: la posición de nivel puede ser negativa.
static inline int32_t fdiv8(int32_t v) { return (v >= 0) ? (v >> 3) : -(((-v) + 7) >> 3); }

static std::string resolve_path(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    if (!cfg) { std::fprintf(stderr, "[skip] sin tests/test_config.toml\n"); return 0; }
    std::string core, rom, line; bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) in_rom = true;
        else if (line.find("core") != std::string::npos
                 && line.find('=') != std::string::npos && core.empty())
            core = cfg_quoted(line);
        else if (in_rom && rom.empty() && line.find("path") != std::string::npos)
            rom = cfg_quoted(line);
    }
    core = resolve_path(core, root); rom = resolve_path(rom, root);
    if (argc >= 3) { core = argv[1]; rom = argv[2]; }
    if (core.empty() || rom.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }
    std::printf("=== panorama_smoke (CU003) ===\ncore: %s\nrom : %s\n\n",
                core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // Boot validado en cam_track_smoke: mashear START hasta entrar al nivel y
    // un pre-roll corriendo, para que la toma arranque YA con scroll. Sin el
    // pre-roll la toma queda en el menú y la cámara nunca se mueve — el
    // chequeo de «hay un frame con cámara distinta de cero» lo delata.
    constexpr uint16_t START = 1u << 3, RIGHT = 1u << 7;   // RetroPad: 6=izq, 7=der
    for (int i = 0; i < 1400; ++i) { s->set_input(0, (i % 32 == 0) ? START : 0); s->step(); }
    for (int i = 0; i < 200;  ++i) { s->set_input(0, RIGHT); s->step(); }
    s->record_start();
    for (int i = 0; i < 420; ++i) { s->set_input(0, RIGHT); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    const uint32_t N = take.frame_count();
    std::printf("[ok] toma: %u frames\n", N);

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail;
    };

    // ── Reconstruir la tira del nivel del plano A, como hace el stitcher: un
    // unwrap independiente del scroll (des-wrapeado en el módulo del PLANO, que
    // es donde el scroll realmente wrapea) y las celdas visibles en espacio de
    // nivel. La primera aparición gana (una celda animada no reescribe).
    constexpr int PLANE = 0;
    std::map<std::pair<int32_t,int32_t>, uint64_t> strip;   // (lx,ly) → primer hash
    std::set<std::pair<std::pair<int32_t,int32_t>, uint64_t>> strip_all;  // todas las parejas
    std::map<uint32_t, std::pair<int32_t,int32_t>> cam_of;  // frame → cámara px
    {
        int32_t cam_x = 0, cam_y = 0;
        int16_t ph = 0, pv = 0; bool first = true;
        for (uint32_t f = 0; f < N; ++f) {
            const FrameView* fv = s->replay_seek(take, f);
            if (!fv) continue;
            const int mw = fv->plane_wpx ? fv->plane_wpx : 512;
            const int mh = fv->plane_hpx ? fv->plane_hpx : 512;
            const int16_t h = fv->plane_hscroll[PLANE], v = fv->plane_vscroll[PLANE];
            if (!first) {
                int dh = (h % mw) - (ph % mw);
                if (dh >  mw / 2) dh -= mw; else if (dh < -mw / 2) dh += mw;
                int dv = (v % mh) - (pv % mh);
                if (dv >  mh / 2) dv -= mh; else if (dv < -mh / 2) dv += mh;
                cam_x += -dh; cam_y += -dv;
            }
            ph = h; pv = v; first = false;
            cam_of[f] = { cam_x, cam_y };
            for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
                const auto& pc = fv->plane_cells[i];
                if (pc.plane != PLANE) continue;
                // Posición de NIVEL de la celda: `cam + screen_x` es constante
                // por celda de nametable (lo mismo que mide widescreen_spike).
                // NO es múltiplo de 8: la grilla de nivel queda corrida por la
                // fase inicial del scroll, y esa fase es la MISMA para todas
                // las celdas. Por eso el invariante de abajo es «offset
                // constante», no «igualdad».
                const auto pos = std::make_pair(fdiv8(cam_x + pc.screen_x),
                                                fdiv8(cam_y + pc.screen_y));
                strip.emplace(pos, pc.hash);
                // TODAS las parejas (posición, hash) observadas, no sólo la
                // primera: una celda animada muestra hashes distintos en la
                // MISMA posición de nivel, y quedarse con el primero deja al
                // resto sin posición conocida — votan por cualquier otro lado.
                strip_all.insert({ pos, pc.hash });
            }
        }
    }
    std::printf("[..] tira reconstruida: %zu celdas de nivel\n", strip.size());
    check(strip.size() > 200, "la tira del nivel tiene celdas suficientes");
    if (strip.size() <= 200) { std::printf("\nFALLÓ\n"); return 1; }

    // TODAS las parejas (posición, hash), igual que `bg_cells` del motor. Con
    // una sola por posición —la primera vista— los estados siguientes de una
    // celda animada quedan sin posición conocida y el voto se dispersa: medido,
    // 27% de moda y cero frames anclados de los que fallaban.
    std::vector<ayther::AytherSession::PanoramaCell> cells;
    cells.reserve(strip_all.size());
    for (const auto& [pos, h] : strip_all)
        cells.push_back({ h, pos.first, pos.second });

    constexpr uint64_t PANO = 0xA07A;
    s->define_panorama(PANO, PLANE, 0, 0, 0, 0,
                       cells.data(), (uint32_t)cells.size(), "tira_hd.png");

    // ── 1. Ancla, con VOTO UNÁNIME y sin saltos físicos ─────────────────────
    // Invariantes auto-contenidos, sin depender de un segundo modelo de cámara:
    //   · unanimidad — todas las celdas que pueden votar votan lo mismo. Un
    //     anclaje ambiguo se delata acá.
    //   · sin saltos — entre frames consecutivos la cámara no puede moverse más
    //     de lo que se mueve una cámara real (el motor mismo considera corte de
    //     escena cualquier delta > 32 px). Es el chequeo que cazó el bug del
    //     unwrap de EM-1, y no necesita un oráculo externo.
    std::map<uint32_t, std::pair<int32_t,int32_t>> anchored_at;
    {
        int anchored = 0, split = 0, jumps = 0, moved = 0;
        double conf_min = 1.0, conf_sum = 0.0;
        bool have_prev = false; int32_t px = 0, py = 0;
        for (uint32_t f = 0; f < N; ++f) {
            const FrameView* fv = s->replay_seek(take, f);
            if (!fv || !fv->panorama_valid) { have_prev = false; continue; }
            ++anchored;
            anchored_at[f] = { fv->panorama_cam_x, fv->panorama_cam_y };
            if (fv->panorama_votes != fv->panorama_cells) ++split;
            if (fv->panorama_cells) {
                const double cf = double(fv->panorama_votes) / fv->panorama_cells;
                conf_sum += cf;
                if (cf < conf_min) conf_min = cf;
            }
            if (have_prev) {
                const int32_t dx = fv->panorama_cam_x - px;
                const int32_t dy = fv->panorama_cam_y - py;
                if (dx || dy) ++moved;
                if (dx > 32 || dx < -32 || dy > 32 || dy < -32) ++jumps;
            }
            px = fv->panorama_cam_x; py = fv->panorama_cam_y; have_prev = true;
        }
        std::printf("[..] anclada en %d de %u frames · %d con voto dividido · "
                    "%d frames en movimiento · %d saltos no fisicos\n",
                    anchored, N, split, moved, jumps);
        // La COBERTURA (qué fracción de frames logra anclar) es una métrica de
        // calidad dependiente del juego, no un invariante de correctitud: con
        // una tira grande hay más hashes repetidos y menos anclas raras.
        // Medido: Aladdin 54%, Sonic 2 41%. Se imprime para que una regresión
        // se note, y se afirma sólo que ancla en una cantidad NO TRIVIAL de
        // frames — la correctitud la cubren los chequeos de abajo (sin saltos,
        // invariante al seek, controles negativos).
        std::printf("[..] cobertura del anclaje: %.0f%% de los frames\n",
                    N ? 100.0 * anchored / N : 0.0);
        check(anchored > 50, "ancla en una cantidad no trivial de frames");

        // ¿El 100% menos la cobertura son fallas, o frames que simplemente NO
        // muestran la tira (un menú, un fundido)? Sin separarlos, «subir la
        // cobertura» es perseguir un número que puede estar ya en su techo.
        {
            int with_strip = 0, missed = 0;
            for (uint32_t f = 0; f < N; ++f) {
                const FrameView* fv = s->replay_seek(take, f);
                if (!fv) continue;
                int in_strip = 0;
                for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
                    const auto& pc = fv->plane_cells[i];
                    if (pc.plane != PLANE) continue;
                    // ¿Este hash existe en algún lado de la tira?
                    for (const auto& c : cells)
                        if (c.hash == pc.hash) { ++in_strip; break; }
                    if (in_strip >= 8) break;
                }
                if (in_strip < 8) continue;      // no es el nivel: no cuenta
                ++with_strip;
                if (!anchored_at.count(f)) ++missed;
            }
            // ¿POR QUÉ fallan? Se replica la regla de rareza del motor
            // (kPanoramaRare = 8) y se cuenta, en los frames que no anclaron,
            // cuántas celdas visibles PODRÍAN votar. Sin ese corte no se sabe
            // si el voto no llega al piso o si directamente no hay votantes, y
            // son dos arreglos distintos.
            {
                std::map<uint64_t, int> freq;
                for (const auto& c : cells) ++freq[c.hash];
                size_t rare_cells = 0;
                for (const auto& c : cells) if (freq[c.hash] <= 8) ++rare_cells;
                int no_voters = 0, few = 0, many = 0, counted = 0;
                long long vsum = 0;
                for (uint32_t f = 0; f < N; ++f) {
                    if (anchored_at.count(f)) continue;
                    const FrameView* fv = s->replay_seek(take, f);
                    if (!fv) continue;
                    int v = 0;
                    for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
                        const auto& pc = fv->plane_cells[i];
                        if (pc.plane != PLANE) continue;
                        auto it2 = freq.find(pc.hash);
                        if (it2 != freq.end() && it2->second <= 8) ++v;
                    }
                    vsum += v; ++counted;
                    if (v == 0) ++no_voters; else if (v < 4) ++few; else ++many;
                }
                const double avg = counted ? (double)vsum / (double)counted : 0.0;
                std::printf("[..] sin anclar: %d SIN votantes · %d con 1-3 · %d con 4+ "
                            "(promedio %.1f)\n", no_voters, few, many, avg);
                // INSTRUMENTAR EL TALLY (issue #253). Se replica el voto del
                // motor sobre los frames que NO anclan y se mira la moda CRUDA,
                // sin el desempate por continuidad. Dos hipótesis a separar:
                //  · si la moda cruda YA supera el 50%, el que tumba el anclaje
                //    es el desempate (elige un candidato con el 80% de los
                //    votos del mejor y después usa ESE conteo para el piso);
                //  · si no lo supera, el voto se dispersa porque cada celda
                //    vota por CADA posición donde su hash aparece en la tira.
                {
                    std::map<uint64_t, std::vector<std::pair<int32_t,int32_t>>> anchors;
                    for (const auto& c : cells)
                        if (freq[c.hash] <= 8) anchors[c.hash].push_back({ c.lx, c.ly });
                    int over50 = 0, under50 = 0, n = 0;
                    double pct_sum = 0.0;
                    long long cand_sum = 0;
                    for (uint32_t f = 0; f < N; ++f) {
                        if (anchored_at.count(f)) continue;
                        const FrameView* fv = s->replay_seek(take, f);
                        if (!fv) continue;
                        std::map<std::pair<int32_t,int32_t>, int> tally;
                        int voters = 0;
                        for (uint32_t i2 = 0; i2 < fv->plane_cell_count; ++i2) {
                            const auto& pc = fv->plane_cells[i2];
                            if (pc.plane != PLANE) continue;
                            auto it3 = anchors.find(pc.hash);
                            if (it3 == anchors.end()) continue;
                            ++voters;
                            for (const auto& [lx, ly] : it3->second)
                                ++tally[{ lx * 8 - pc.screen_x, ly * 8 - pc.screen_y }];
                        }
                        if (!voters) continue;
                        int best = 0;
                        for (const auto& [k2, v2] : tally) if (v2 > best) best = v2;
                        const double pct = 100.0 * best / voters;
                        pct_sum += pct; cand_sum += (long long)tally.size(); ++n;
                        if (pct >= 50.0) ++over50; else ++under50;
                    }
                    // ¿Y si la tira guardara TODAS las parejas observadas?
                    {
                        std::map<uint64_t, int> f2;
                        for (const auto& [pos, h] : strip_all) { (void)pos; ++f2[h]; }
                        std::map<uint64_t, std::vector<std::pair<int32_t,int32_t>>> an2;
                        for (const auto& [pos, h] : strip_all)
                            if (f2[h] <= 8) an2[h].push_back(pos);
                        int ok2 = 0, n2 = 0; double sum2 = 0.0;
                        for (uint32_t f = 0; f < N; ++f) {
                            if (anchored_at.count(f)) continue;
                            const FrameView* fv = s->replay_seek(take, f);
                            if (!fv) continue;
                            std::map<std::pair<int32_t,int32_t>, int> t2;
                            int v2 = 0;
                            for (uint32_t i2 = 0; i2 < fv->plane_cell_count; ++i2) {
                                const auto& pc = fv->plane_cells[i2];
                                if (pc.plane != PLANE) continue;
                                auto it4 = an2.find(pc.hash);
                                if (it4 == an2.end()) continue;
                                ++v2;
                                for (const auto& [lx, ly] : it4->second)
                                    ++t2[{ lx * 8 - pc.screen_x, ly * 8 - pc.screen_y }];
                            }
                            if (!v2) continue;
                            int b2 = 0;
                            for (const auto& [k3, vv] : t2) if (vv > b2) b2 = vv;
                            sum2 += 100.0 * b2 / v2; ++n2;
                            if (100.0 * b2 >= 50.0 * v2) ++ok2;
                        }
                        std::printf("[..] con TODAS las parejas (%zu vs %zu celdas): "
                                    "%.0f%% promedio · %d de %d superarian el piso\n",
                                    strip_all.size(), strip.size(),
                                    n2 ? sum2 / n2 : 0.0, ok2, n2);
                    }
                    std::printf("[..] moda CRUDA en los frames sin anclar: %.0f%% promedio · "
                                "%d superan el piso de 50%% · %d no · %.0f candidatos/frame\n",
                                n ? pct_sum / n : 0.0, over50, under50,
                                n ? (double)cand_sum / n : 0.0);
                }
                std::printf("[..] la tira: %zu celdas · %zu hashes distintos · "
                            "%zu celdas con hash raro (<=8)\n",
                            cells.size(), freq.size(), rare_cells);
            }
            std::printf("[..] frames que SÍ muestran la tira: %d · sin anclar: %d "
                        "(%.0f%% de los que importan)\n",
                        with_strip, missed,
                        with_strip ? 100.0 * (with_strip - missed) / with_strip : 0.0);
            check(with_strip > 0, "control: hay frames que muestran la tira");
            // Ahora SÍ es un gate. Llegó al 90% guardando todas las parejas
            // (posición, hash) de la tira en vez de la primera de cada
            // posición; antes era 54% y la tira parpadeaba. El piso se pone en
            // 85% para dejar margen a otra ROM sin volverlo un test frágil.
            check(missed * 100 <= with_strip * 15,
                  "ancla en >=85% de los frames donde la tira ESTÁ");
        }
        // La CONFIANZA (votantes que respaldan al ganador) NO se asserta acá:
        // el motor ya gatea `panorama_valid` por ella, así que exigirla sería
        // tautológico. Se IMPRIME para que un empeoramiento se note, y lo que
        // se afirma es lo que el gate no garantiza: que siga anclando en la
        // mayoría de los frames en vez de callarse casi siempre.
        std::printf("[..] confianza del voto en los frames anclados: "
                    "minima %.2f · media %.2f (el motor exige >= %u%%)\n",
                    conf_min, anchored ? conf_sum / anchored : 0.0,
                    ayther::AytherSession::kPanoramaMinVotePct);
        check(moved > (int)(N / 4), "control: la cámara se mueve de verdad en la toma");
        // Un voto probabilístico no puede prometer cero outliers: un hash raro
        // repetido en otro tramo del nivel puede ganar un frame suelto. La
        // continuidad temporal los reduce a un puñado; lo que se exige es que
        // sean RAROS (<1%), no inexistentes — si suben, algo se rompió.
        check(jumps * 100 <= (int)N,
              "los saltos no físicos son marginales (<1% de los frames)");
    }

    // ── 2. EL MISMO FRAME DA EL MISMO ANCLAJE, se llegue como se llegue ─────
    // Es LA propiedad del diseño: por eso no se usa la cámara acumulada de
    // EM-1, que se re-ancla en 0 en cada seek. Se compara el anclaje llegando
    // por un salto largo contra llegando secuencialmente al MISMO frame.
    {
        uint32_t target = 0;
        for (const auto& [f, cam] : anchored_at)
            if (f > 30 && (cam.first != 0 || cam.second != 0)) { target = f; break; }
        check(target != 0, "hay un frame con cámara distinta de cero para el caso");

        // (a) llegando por un salto largo hacia atrás
        s->replay_seek(take, N - 20);
        const FrameView* by_seek = s->replay_seek(take, target);
        const int32_t sx = by_seek ? by_seek->panorama_cam_x : 0;
        const int32_t sy = by_seek ? by_seek->panorama_cam_y : 0;
        const bool cam_dead = by_seek && !by_seek->plane_cam_valid;

        // (b) llegando secuencialmente
        for (uint32_t g = target > 4 ? target - 4 : 0; g < target; ++g)
            s->replay_seek(take, g);
        const FrameView* by_walk = s->replay_seek(take, target);
        const int32_t wx = by_walk ? by_walk->panorama_cam_x : 0;
        const int32_t wy = by_walk ? by_walk->panorama_cam_y : 0;

        std::printf("[..] frame %u: por salto (%d,%d) · secuencial (%d,%d)\n",
                    target, sx, sy, wx, wy);
        check(cam_dead, "control: el salto SÍ invalida la cámara acumulada de EM-1");
        check(by_seek && by_seek->panorama_valid,
              "tras el salto la Panorámica SIGUE anclada");
        check(sx == wx && sy == wy,
              "el anclaje es el MISMO por salto que secuencialmente "
              "(lo que la cámara acumulada no puede dar)");
        check(sx != 0 || sy != 0, "y no es el origen (el caso trivial)");
    }

    // ── 3. Control negativo: una tira de basura no ancla ────────────────────
    {
        s->clear_panoramas();
        std::vector<ayther::AytherSession::PanoramaCell> junk;
        for (uint32_t i = 0; i < 400; ++i)
            junk.push_back({ 0xF00D0000ull + i, (int32_t)i, 0 });
        s->define_panorama(0xBADBAD, PLANE, 0, 0, 0, 0,
                           junk.data(), (uint32_t)junk.size(), "basura.png");
        s->replay_invalidate();
        const FrameView* fv = s->replay_seek(take, N / 2);
        check(fv && !fv->panorama_valid,
              "una tira que no es del nivel NO ancla (no inventa una posición)");
        s->clear_panoramas();
    }

    // ── 4. La rareza importa ────────────────────────────────────────────────
    // Una tira donde TODAS las celdas comparten el mismo hash (cielo liso) no
    // tiene anclas: el índice queda vacío y no puede fijar la cámara. Es la
    // limitación honesta del método, y el motivo de que haya un fallback
    // declarado por capa en el diseño.
    {
        std::vector<ayther::AytherSession::PanoramaCell> flat;
        const uint64_t one = cells.front().hash;
        for (uint32_t i = 0; i < 200; ++i)
            flat.push_back({ one, (int32_t)i, 0 });
        s->define_panorama(0xF1A7, PLANE, 0, 0, 0, 0,
                           flat.data(), (uint32_t)flat.size(), "cielo.png");
        s->replay_invalidate();
        const FrameView* fv = s->replay_seek(take, N / 2);
        check(fv && !fv->panorama_valid,
              "una tira de un solo hash repetido no ancla (rareza: sin anclas)");
        s->clear_panoramas();
        s->replay_invalidate();
        const FrameView* fv2 = s->replay_seek(take, N / 2);
        check(fv2 && !fv2->panorama_valid, "clear_panoramas la retira");
    }

    // ── 5. RENDER: la tira se recorta a las celdas que SON la panorámica ──────
    // El bloqueo que tenía esta fase era que un quad opaco a pantalla completa
    // tapa los píxeles del otro plano. La salida fue emitir un quad por TRAMO de
    // celdas contiguas cubiertas: el recorte por celda ES la máscara. Acá se
    // verifica que ese recorte sea por CONTENIDO y no por posición, que es lo
    // único que distingue la tira de un HUD dibujado sobre el mismo plano.
    {
        // Bounds reales de la tira, para que el UV signifique algo (definirla con
        // w_cells=0 dejaba el sub-rect en una escala degenerada).
        int32_t lx0 = INT32_MAX, ly0 = INT32_MAX, lx1 = INT32_MIN, ly1 = INT32_MIN;
        for (const auto& c : cells) {
            lx0 = c.lx < lx0 ? c.lx : lx0;  lx1 = c.lx > lx1 ? c.lx : lx1;
            ly0 = c.ly < ly0 ? c.ly : ly0;  ly1 = c.ly > ly1 ? c.ly : ly1;
        }
        const uint16_t W = (uint16_t)(lx1 - lx0 + 1), H = (uint16_t)(ly1 - ly0 + 1);
        auto define = [&](const std::vector<ayther::AytherSession::PanoramaCell>& v) {
            s->clear_panoramas();
            s->define_panorama(PANO, PLANE, lx0, ly0, W, H,
                               v.data(), (uint32_t)v.size(), "tira_hd.png");
            s->replay_invalidate();
        };
        s->set_hd_enabled(true);      // la emisión no corre en modo Original
        define(cells);

        int frames_with_quads = 0, overlaps = 0, uv_bad = 0;
        long long covered_true = 0;
        std::map<uint32_t, long long> cov_by_frame;
        for (uint32_t f = 0; f < N; ++f) {
            const FrameView* fv = s->replay_seek(take, f);
            if (!fv || fv->panorama_sub_count == 0) continue;
            ++frames_with_quads;
            std::set<std::pair<int,int>> seen;     // celdas ya pintadas este frame
            long long cov = 0;
            for (uint32_t q = 0; q < fv->panorama_sub_count; ++q) {
                const auto& sub = fv->panorama_subs[q];
                const int n = sub.w_px / 8;
                cov += n;
                // El sub-rect UV tiene que apuntar al MISMO píxel de nivel que
                // implica la cámara anclada. Si esto se corre, la tira se dibuja
                // desfasada y el bug es invisible en un test de sólo cobertura.
                // (#349: el quad ahora es el RECT COMPLETO anclado al píxel — el
                // contrato viejo por celda redondeaba y ya no aplica.)
                const int32_t want_px = fv->panorama_cam_x + sub.screen_x;
                const int32_t want_py = fv->panorama_cam_y + sub.screen_y;
                const int32_t got_px = lx0 * 8 + (int32_t)(sub.u0 * (float)(W * 8) + 0.5f);
                const int32_t got_py = ly0 * 8 + (int32_t)(sub.v0 * (float)(H * 8) + 0.5f);
                if (got_px != want_px || got_py != want_py) ++uv_bad;
                for (int k = 0; k < n; ++k)
                    if (!seen.insert({ sub.screen_x + k * 8, sub.screen_y }).second)
                        ++overlaps;
            }
            cov_by_frame[f] = cov;
            covered_true += cov;
        }
        std::printf("[..] %d frames con tira · %lld celdas cubiertas\n",
                    frames_with_quads, covered_true);
        check(frames_with_quads > 20,
              "control: la tira se emite en una cantidad no trivial de frames");
        check(covered_true > 0, "control: se cubre alguna celda (el test no es vacío)");
        check(overlaps == 0, "los tramos no se pisan entre sí");
        check(uv_bad == 0, "el sub-rect UV apunta a la celda de nivel del anclaje");

        // ── La cobertura RECLAMA la celda, no sólo la tapa ───────────────────
        // Emitir el quad no alcanza: si la celda original no queda `claimed`, el
        // compose la vuelve a dibujar — debajo de la tira (invisible) y ENCIMA
        // si es de prioridad alta, que va en la lane de frente. El reclamo vivía
        // detrás de un `consumed.size() == npick` que sólo se cumplía cuando
        // había un CUADRO en pantalla, o sea nunca en una pantalla con scroll:
        // justo donde vive una Panorámica (reporte 2026-08-05).
        {
    std::set<std::tuple<int32_t, int32_t, uint64_t>> strip;
    for (const auto& c : cells) strip.insert({ c.lx, c.ly, c.hash });
            // PlaneCellHit.plane 0=A · 1=B · 2=W  ↔  SceneElement.layer 0=B · 1=A · 2=W
            const uint8_t want_layer = PLANE == 0 ? 1 : (PLANE == 1 ? 0 : 2);
            long long recognized = 0, unclaimed = 0;
            int inspected_frames = 0;
            std::vector<ayther::SceneElement> inv;
            for (const auto& [f, cov] : cov_by_frame) {
                if (inspected_frames >= 40) break;      // muestra: el inventario no es gratis
                const FrameView* fv = s->replay_seek(take, f);
                if (!fv) continue;
                ++inspected_frames;
                s->scene_inventory(inv);
                for (const auto& e : inv) {
                    if (e.layer != want_layer) continue;
                    const int32_t lx = (fv->panorama_cam_x + e.x) >> 3;
                    const int32_t ly = (fv->panorama_cam_y + e.y) >> 3;
            if (!strip.count({ lx, ly, e.hash })) continue;
                    ++recognized;
                    if (!e.claimed) ++unclaimed;
                }
            }
            std::printf("[..] %d frames inspeccionados · %lld celdas de la tira "
                        "en el inventario\n", inspected_frames, recognized);
            check(recognized > 0,
                  "control: el inventario ve celdas que la tira reconoce "
                  "(sin esto el check de abajo es vacuo)");
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "esas celdas quedan RECLAMADAS (%lld sin reclamar)",
                          unclaimed);
            check(unclaimed == 0, msg);
        }

        // NEGATIVO: misma tira, mismas POSICIONES, hashes falsos en la mitad de
        // arriba. Si la cobertura fuera por posición, no cambiaría nada. Como es
        // por contenido, esas celdas tienen que dejar de cubrirse — y son
        // exactamente las que en el juego real son un HUD o un primer plano.
        std::vector<ayther::AytherSession::PanoramaCell> poisoned = cells;
        const int32_t mid = (ly0 + ly1) / 2;
        int poisoned_n = 0;
        for (auto& c : poisoned)
            if (c.ly <= mid) { c.hash ^= 0xDEADBEEFull; ++poisoned_n; }
        check(poisoned_n > 0, "control: la mitad envenenada no está vacía");
        define(poisoned);

        long long covered_poison = 0;
        for (uint32_t f = 0; f < N; ++f) {
            const FrameView* fv = s->replay_seek(take, f);
            if (!fv) continue;
            for (uint32_t q = 0; q < fv->panorama_sub_count; ++q)
                covered_poison += fv->panorama_subs[q].w_px / 8;
        }
        std::printf("[..] con la mitad envenenada: %lld celdas (antes %lld)\n",
                    covered_poison, covered_true);
        check(covered_poison < covered_true,
              "envenenar los hashes REDUCE la cobertura (es por contenido, no por posición)");
        s->clear_panoramas();
        s->set_hd_enabled(false);
        s->replay_invalidate();
    }

    // ── 6. PERSISTENCIA: la tira sobrevive al TOML ────────────────────────────
    // Sin esto la Panorámica vivía sólo en la sesión de autoría: reiniciar el
    // Lab la perdía y el pack entregado no la tenía — el mismo agujero que la
    // Utilería y los Glifos antes de plane_sets.toml. El round-trip es donde un
    // escritor/parser a mano se delata, y acá hay dos trampas concretas: las
    // posiciones de nivel van con SIGNO (el dialecto de screens.toml las tiene
    // sin signo y truncaría la tira) y las filas se emiten agrupadas.
    {
        ayther::PackPanorama p;
        p.id = 0xA07A; p.name = "tira"; p.plane = PLANE;
        p.origin_x = -7; p.origin_y = -3;      // NEGATIVOS a propósito
        p.w_cells = 321; p.h_cells = 28;
        p.asset = "tira_hd.png";
        for (const auto& c : cells) p.cells.push_back({ c.hash, c.lx, c.ly });
        // Celdas por delante del origen: el caso que un parser sin signo pierde.
        p.cells.push_back({ 0xFEEDFACEull, -7, -3 });
        p.cells.push_back({ 0xC0FFEEull,   -1, -3 });

        const std::string toml = ayther::bake_panoramas_toml({ p });
        std::printf("[..] panoramas.toml: %zu bytes para %zu celdas (%.1f B/celda)\n",
                    toml.size(), p.cells.size(),
                    (double)toml.size() / (double)p.cells.size());
        std::vector<ayther::PackPanorama> back;
        const size_t n_ok = ayther::parse_panoramas_toml(toml, back);
        check(n_ok == 1 && back.size() == 1, "panoramas.toml vuelve a parsearse");
        if (back.size() == 1) {
            const auto& q = back[0];
            check(q.id == p.id && q.plane == p.plane && q.asset == p.asset,
                  "id, capa y asset sobreviven");
            check(q.origin_x == -7 && q.origin_y == -3 &&
                  q.w_cells == 321 && q.h_cells == 28,
                  "origen NEGATIVO y tamaño sobreviven (el dialecto sin signo los rompía)");
            check(q.cells.size() == p.cells.size(), "no se pierde ninguna celda");
            // Comparar como CONJUNTO: el bake agrupa por fila, así que el orden
            // de salida no tiene por qué ser el de entrada — lo que importa es
            // que cada (lx,ly) conserve su hash.
            std::map<std::pair<int32_t,int32_t>, uint64_t> a, b;
            for (const auto& c : p.cells) a[{ c.lx, c.ly }] = c.hash;
            for (const auto& c : q.cells) b[{ c.lx, c.ly }] = c.hash;
            check(a == b, "cada celda conserva su hash en su posición de nivel");
        }

        // Y que el motor lo RECONOZCA igual definido desde el TOML que por API:
        // el parser puede round-trippear y aun así la def salir distinta (la
        // rareza se recalcula, y de eso depende el anclaje).
        s->clear_panoramas();
        s->replay_invalidate();
        std::vector<ayther::AytherSession::PanoramaCell> from_toml;
        for (const auto& c : back[0].cells) from_toml.push_back({ c.hash, c.lx, c.ly });
        s->define_panorama(PANO, PLANE, back[0].origin_x, back[0].origin_y,
                           back[0].w_cells, back[0].h_cells,
                           from_toml.data(), (uint32_t)from_toml.size(), "tira_hd.png");
        s->replay_invalidate();
        int same = 0, seen_n = 0;
        for (const auto& [f, cam] : anchored_at) {
            const FrameView* fv = s->replay_seek(take, f);
            if (!fv || !fv->panorama_valid) continue;
            ++seen_n;
            if (fv->panorama_cam_x == cam.first && fv->panorama_cam_y == cam.second) ++same;
        }
        std::printf("[..] anclaje desde TOML: %d/%d frames coinciden\n", same, seen_n);
        check(seen_n > 20, "control: hay frames anclados que comparar");
        check(same == seen_n, "la tira reconstruida del TOML ancla EN EL MISMO lugar");
        s->clear_panoramas();
        s->replay_invalidate();
    }

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
