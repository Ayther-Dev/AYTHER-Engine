// ---------------------------------------------------------------------------
// screen_sig_smoke — valida la firma de PANTALLA (Cuadro · CU001): la
// identidad con la que el motor reconoce una pantalla estática de fondo para
// reemplazarla entera por un asset HD.
//
// Lo que tiene que cumplir, y en ese orden de importancia:
//   1. ESTABLE — la misma pantalla da la misma firma cada vez que se vuelve a
//      ella (re-produce del mismo frame, y volver después de haber estado en
//      otra pantalla). Sin esto no hay reconocimiento.
//   2. DISCRIMINANTE — dos pantallas distintas dan firmas distintas. Es el
//      control que un umbral flojo no cubre: sin él, "matchea siempre" pasaría
//      el test de estabilidad con nota perfecta.
//   3. COMPONIBLE POR CAPAS — la firma de una máscara es la suma de sus
//      planos, y quitar una capa de la máscara cambia la firma (si no, elegir
//      capas al capturar no serviría de nada).
//   4. SÓLO REFLEJA EL CONTENIDO — la firma se mueve si y sólo si cambió el
//      conjunto de celdas (plano, columna, fila, hash). De ahí sale gratis la
//      tolerancia a la fase sub-celda del scroll, que es la razón de que el
//      término use screen_x>>3: un título que tiembla unos px sigue siendo el
//      mismo Cuadro. (Este chequeo NO se puede escribir como "el scroll
//      sub-celda nunca mueve la firma" a secas: entre dos frames también puede
//      cambiar el contenido —una celda animada, o una que entra por el borde—
//      y ahí la firma DEBE cambiar. Por eso el reconocimiento real de un
//      Cuadro es por cobertura con umbral y no por igualdad exacta.)
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target screen_sig_smoke
//   Run:    bin/screen_sig_smoke [core.dll rom]
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_components_toml.h"   // bake/parse screens.toml
#include "ayther_core_ffi.h"          // ayther_pack_builder_*

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
    std::printf("=== screen_sig_smoke (Cuadro · CU001) ===\ncore: %s\nrom : %s\n\n",
                core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // Arranque + una toma larga: el arranque pasa por VARIAS pantallas
    // distintas (marca, título, juego), que es justo el corpus que hace falta.
    constexpr uint16_t START = 1u << 3, RIGHT = 1u << 6;
    for (int i = 0; i < 240; ++i) { s->set_input(0, 0); s->step(); }
    s->record_start();
    for (int i = 0; i < 900; ++i) { s->set_input(0, (i % 24 < 6) ? START : 0); s->step(); }
    for (int i = 0; i < 240; ++i) { s->set_input(0, RIGHT); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    const uint32_t N = take.frame_count();
    std::printf("[ok] toma: %u frames\n", N);

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail;
    };
    constexpr uint8_t kAll = 0x07;   // A|B|Window

    auto sig_at = [&](uint32_t f, uint8_t mask, uint32_t* cells = nullptr) -> uint64_t {
        const FrameView* fv = s->replay_seek(take, f);
        return fv ? fv->screen_signature(mask, cells) : 0;
    };

    // ── 0. Control: el pick-list de planos existe (sin el core forkeado la
    // firma sería 0 en todos lados y el resto del smoke no probaría nada).
    uint32_t cells_mid = 0;
    const uint64_t sig_mid = sig_at(N / 2, kAll, &cells_mid);
    check(cells_mid > 0 && sig_mid != 0,
          "control: hay celdas de plano (requiere el core _vram)");
    std::printf("[..] frame %u: %u celdas · firma 0x%016llx\n",
                N / 2, cells_mid, (unsigned long long)sig_mid);

    // ── 1. ESTABLE: re-producir el mismo frame da la misma firma, y volver a
    // él después de haber pasado por otro también.
    {
        s->replay_invalidate();
        const uint64_t again = sig_at(N / 2, kAll);
        check(again == sig_mid, "misma pantalla, re-produce: misma firma");
        sig_at(10, kAll);                       // irse a otra pantalla
        const uint64_t back = sig_at(N / 2, kAll);
        check(back == sig_mid, "volver a la pantalla da la misma firma");
    }

    // ── 2. DISCRIMINANTE: recorrer la toma y contar cuántas firmas DISTINTAS
    // aparecen. El arranque cruza varias pantallas, así que si la firma fuera
    // constante (o casi) no serviría para reconocer nada.
    std::map<uint64_t, uint32_t> hist;          // firma → frames que la muestran
    uint32_t sampled = 0, empty_frames = 0;
    for (uint32_t f = 0; f < N; f += 5) {
        uint32_t cells = 0;
        const uint64_t sg = sig_at(f, kAll, &cells);
        if (!cells) { ++empty_frames; continue; }
        ++hist[sg];
        ++sampled;
    }
    std::printf("[..] %u frames muestreados · %zu firmas distintas · %u sin celdas\n",
                sampled, hist.size(), empty_frames);
    check(hist.size() >= 3,
          "la firma DISCRIMINA: el arranque cruza varias pantallas distintas");

    // La pantalla más repetida tiene que sostenerse varios frames seguidos —
    // una firma que cambia cada frame no sirve para reconocer un Cuadro.
    uint32_t best = 0;
    for (const auto& [sg, n] : hist) if (n > best) best = n;
    std::printf("[..] la pantalla más estable ocupa %u de %u muestras (%.0f%%)\n",
                best, sampled, sampled ? 100.0 * best / sampled : 0.0);
    check(best >= 3, "hay pantallas que se sostienen en el tiempo (Cuadros reales)");

    // ── 3. COMPONIBLE POR CAPAS -------------------------------------------
    {
        const FrameView* fv = s->replay_seek(take, N / 2);
        if (fv) {
            const uint64_t a  = fv->screen_signature(0x01);
            const uint64_t b  = fv->screen_signature(0x02);
            const uint64_t ab = fv->screen_signature(0x03);
            check(ab == a + b, "la firma de una máscara es la suma de sus planos");
            uint32_t ca = 0, cab = 0;
            fv->screen_signature(0x01, &ca);
            fv->screen_signature(0x03, &cab);
            check(cab >= ca, "el conteo de celdas acompaña a la máscara");
            // Quitar una capa con contenido tiene que cambiar la firma: si no,
            // elegir capas al capturar un Cuadro no serviría de nada.
            const bool b_has = fv->screen_plane_cells[1] > 0;
            check(!b_has || ab != a,
                  "quitar una capa con contenido CAMBIA la firma");
        }
    }

    // ── 4. LA FIRMA SÓLO REFLEJA EL CONTENIDO -----------------------------
    // El invariante fuerte, que subsume la tolerancia a la fase sub-celda: la
    // firma cambia si y sólo si cambió el CONJUNTO de celdas (plano, columna,
    // fila, hash). Como la columna es screen_x>>3, un scroll menor a una celda
    // no toca el conjunto — y por lo tanto no puede tocar la firma.
    //
    // Se ejercita sobre pares de frames consecutivos cuyo scroll se movió sin
    // cruzar el borde de celda en NINGUNA de las dos fases (si lo cruzara, la
    // grilla sí se corrió y la firma debe cambiar). Los pares donde el conjunto
    // igual cambió son contenido real: una celda animada, o una que entró o
    // salió por el borde de la pantalla. Es justamente el motivo de que el
    // reconocimiento de un Cuadro sea por COBERTURA con umbral y no por
    // igualdad exacta.
    {
        auto cellset = [&](const FrameView* fv, int plane) {
            std::vector<uint64_t> out;
            for (uint32_t i = 0; fv && i < fv->plane_cell_count; ++i) {
                const auto& pc = fv->plane_cells[i];
                if (pc.plane != plane) continue;
                out.push_back(pc.hash ^ ((uint64_t)(uint16_t)(pc.screen_x >> 3) << 48)
                                      ^ ((uint64_t)(uint16_t)(pc.screen_y >> 3) << 32));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        int pairs = 0, same_set = 0, broke_same_set = 0, content_changed = 0;
        for (int p = 0; p < 3; ++p) {
            const uint8_t mask = (uint8_t)(1u << p);
            int16_t h_prev = 0, v_prev = 0;
            uint64_t sig_prev = 0;
            std::vector<uint64_t> set_prev;
            bool first = true;
            for (uint32_t f = 0; f < N; ++f) {
                const FrameView* fv = s->replay_seek(take, f);
                if (!fv || !fv->screen_plane_cells[p]) { first = true; continue; }
                const int16_t  h  = fv->plane_hscroll[p];
                const int16_t  v  = fv->plane_vscroll[p];
                const uint64_t sg = fv->screen_signature(mask);
                std::vector<uint64_t> set = cellset(fv, p);
                if (!first && h != h_prev && (h >> 3) == (h_prev >> 3)
                    && (v >> 3) == (v_prev >> 3)) {
                    ++pairs;
                    if (set == set_prev) {
                        ++same_set;
                        if (sg != sig_prev) ++broke_same_set;
                    } else if (sg != sig_prev) {
                        ++content_changed;
                    }
                }
                h_prev = h; v_prev = v; sig_prev = sg; set_prev = std::move(set);
                first = false;
            }
        }
        std::printf("[..] scroll sub-celda: %d pares · %d con el conjunto de celdas "
                    "INTACTO (%d movieron la firma) · %d cambiaron de contenido\n",
                    pairs, same_set, broke_same_set, content_changed);
        check(pairs > 0, "el corpus ejercita el caso (hay scroll sub-celda)");
        check(same_set > 0, "hay pares con el conjunto de celdas intacto");
        check(broke_same_set == 0,
              "con el conjunto intacto la firma NO se mueve: es invariante a la "
              "fase sub-celda del scroll");
    }

    // ── 5. RECONOCIMIENTO DEL kPictureId (cobertura + los dos guardas) ────────
    // Se captura la pantalla de un frame como Cuadro y se verifica que el
    // motor la reconozca ahí y NO en otra; después se degrada la captura a
    // propósito para ver dónde cae cada guarda.
    {
        // Un frame con contenido y otro claramente distinto (firmas distintas).
        uint32_t f_home = N / 2, f_away = UINT32_MAX;
        const uint64_t sig_home = sig_at(f_home, kAll);
        for (uint32_t f = 10; f < N; f += 7)
            if (sig_at(f, kAll) != sig_home && sig_at(f, kAll) != 0) { f_away = f; break; }
        check(f_away != UINT32_MAX, "hay dos pantallas distintas para el caso");

        // Capturar el Cuadro del frame «home» tal cual lo ve el motor.
        std::vector<ayther::AytherSession::ScreenCell> cap;
        if (const FrameView* fv = s->replay_seek(take, f_home)) {
            for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
                const auto& pc = fv->plane_cells[i];
                if (pc.plane > 2) continue;
                cap.push_back({ pc.hash, pc.plane,
                                (uint8_t)(pc.screen_x >> 3), (uint8_t)(pc.screen_y >> 3) });
            }
        }
        check(cap.size() > 20, "la captura del Cuadro trae celdas suficientes");

        constexpr uint64_t kPictureId = 0xC0AD01;
        auto matched = [&](uint32_t f) -> bool {
            // Histéresis de entrada: 2 frames consecutivos. Se produce el mismo
            // frame dos veces (el tracker de match no depende del nº de frame).
            s->replay_invalidate(); s->replay_seek(take, f);
            s->replay_invalidate();
            const FrameView* fv = s->replay_seek(take, f);
            return fv && fv->screen_match_id == kPictureId && fv->screen_sub_count == 1;
        };

        s->define_screen(kPictureId, kAll, cap.data(), (uint32_t)cap.size(),
                         0.92f, 0.08f, "cuadro_hd.png");
        check(matched(f_home), "el Cuadro capturado se reconoce en SU pantalla");
        if (f_away != UINT32_MAX)
            check(!matched(f_away), "NO se reconoce en otra pantalla (control negativo)");

        // RECONOCIMIENTO EXACTO: toda variación, por mínima que sea, es otro
        // Cuadro. Antes esto era cobertura con umbral y una celda animada se
        // "toleraba" — o sea que dos pantallas distintas colapsaban en una y el
        // artista no podía darle un asset propio a cada estado.
        // Se rompe una CANTIDAD, no una fracción: con `cap` de unos cientos de
        // celdas, un 0,3% redondeaba a CERO y el test no rompía nada — habría
        // pasado por vacío con cualquier implementación.
        auto with_broken = [&](size_t n) {
            std::vector<ayther::AytherSession::ScreenCell> v = cap;
            if (n > v.size()) n = v.size();
            for (size_t i = 0; i < n; ++i) v[i].hash ^= 0xDEADBEEFull;
            s->define_screen(kPictureId, kAll, v.data(), (uint32_t)v.size(),
                             0.92f, 0.08f, "cuadro_hd.png");
            return matched(f_home);
        };
        check(!with_broken(1),
              "UNA sola celda distinta ya es otro Cuadro (no hay umbral que la tolere)");
        check(!with_broken(cap.size() / 5),
              "y por supuesto tampoco matchea con el 20% cambiado");
        // Contracara: sin romper nada vuelve a matchear — el rechazo de arriba
        // es por el cambio, no porque redefinir rompa algo.
        s->define_screen(kPictureId, kAll, cap.data(), (uint32_t)cap.size(),
                         0.92f, 0.08f, "cuadro_hd.png");
        check(matched(f_home), "control: sin cambios vuelve a matchear");

        // Un SUBCONJUNTO no es la pantalla: un Cuadro que declara la mitad de
        // las celdas no matchea, y ahora sale del conteo por capa en vez de un
        // umbral de sobrantes. Es el caso del menú dibujado encima del título:
        // el título "está entero" pero la pantalla es otra.
        {
            std::vector<ayther::AytherSession::ScreenCell> half(
                cap.begin(), cap.begin() + cap.size() / 2);
            s->define_screen(kPictureId, kAll, half.data(), (uint32_t)half.size(),
                             0.92f, 0.08f, "cuadro_hd.png");
            check(!matched(f_home),
                  "un SUBCONJUNTO de la pantalla NO matchea (el conteo por capa no da)");
            // Y con el umbral de sobrantes abierto de par en par SIGUE sin
            // matchear: el rechazo ya no depende de max_extra, que quedó como
            // campo legado y no decide nada.
            s->define_screen(kPictureId, kAll, half.data(), (uint32_t)half.size(),
                             0.92f, 10.0f, "cuadro_hd.png");
            check(!matched(f_home),
                  "y sigue sin matchear con max_extra abierto (los umbrales ya no deciden)");
        }

        // El asset NO participa del reconocimiento: un Cuadro sin asset se
        // reconoce igual. Sin esto, una Cinemática de N Cuadros cubierta por UN
        // solo video no avanzaría nunca — ningún paso se reconocería.
        {
            s->define_screen(kPictureId, kAll, cap.data(), (uint32_t)cap.size(),
                             0.92f, 0.08f, "");
            s->replay_invalidate(); s->replay_seek(take, f_home);
            s->replay_invalidate();
            const FrameView* fv = s->replay_seek(take, f_home);
            check(fv && fv->screen_match_id == kPictureId,
                  "un Cuadro SIN asset se reconoce igual (el asset gatea dibujar, no reconocer)");
            check(fv && fv->screen_sub_count == 0,
                  "…pero no dibuja nada, porque no hay qué dibujar");
            s->define_screen(kPictureId, kAll, cap.data(), (uint32_t)cap.size(),
                             0.92f, 0.08f, "cuadro_hd.png");
        }

        // El gate de HD apaga el Cuadro como apaga los sets.
        s->define_screen(kPictureId, kAll, cap.data(), (uint32_t)cap.size(),
                         0.92f, 0.08f, "cuadro_hd.png");
        check(matched(f_home), "re-declarado, vuelve a matchear");
        s->set_hd_enabled(false);
        check(!matched(f_home), "set_hd_enabled(false) apaga el Cuadro");
        s->set_hd_enabled(true);

        s->clear_screens();
        check(!matched(f_home), "clear_screens lo retira");
    }

    // ── 6. UNA O VARIAS CAPAS ─────────────────────────────────────────────
    // El sentido de elegir capas al capturar: un Cuadro que declara SÓLO el
    // fondo tiene que seguir matcheando aunque otra capa (el HUD, una capa
    // animada) cambie por completo — y uno que declara las DOS tiene que dejar
    // de matchear cuando cualquiera de ellas cambia. Sin esto, la máscara sería
    // decorativa.
    {
        // Par de frames donde un plano quedó IGUAL y el otro cambió.
        uint32_t fa = UINT32_MAX, fb = UINT32_MAX;
        int stable_p = -1, moved_p = -1;
        for (uint32_t f = 0; f + 1 < N && fa == UINT32_MAX; ++f) {
            const FrameView* v1 = s->replay_seek(take, f);
            if (!v1) continue;
            const uint64_t a1 = v1->screen_plane_sig[0], b1 = v1->screen_plane_sig[1];
            const uint32_t na1 = v1->screen_plane_cells[0], nb1 = v1->screen_plane_cells[1];
            if (!na1 || !nb1) continue;
            for (uint32_t g = f + 1; g < N && g < f + 40; ++g) {
                const FrameView* v2 = s->replay_seek(take, g);
                if (!v2) continue;
                const uint64_t a2 = v2->screen_plane_sig[0], b2 = v2->screen_plane_sig[1];
                if (a1 == a2 && b1 != b2) { fa = f; fb = g; stable_p = 0; moved_p = 1; break; }
                if (b1 == b2 && a1 != a2) { fa = f; fb = g; stable_p = 1; moved_p = 0; break; }
            }
        }
        std::printf("[..] par por capas: %s (plano %d estable, plano %d cambió)\n",
                    fa == UINT32_MAX ? "no encontrado" : "hallado", stable_p, moved_p);
        check(fa != UINT32_MAX,
              "el corpus tiene un par con una capa estable y la otra cambiada");

        if (fa != UINT32_MAX) {
            constexpr uint64_t SOLO = 0xC0AD02, AMBAS = 0xC0AD03;
            const uint8_t m_solo = (uint8_t)(1u << stable_p);
            const uint8_t m_ambas = 0x03;
            std::vector<ayther::AytherSession::ScreenCell> c_solo, c_ambas;
            if (const FrameView* fv = s->replay_seek(take, fa)) {
                for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
                    const auto& pc = fv->plane_cells[i];
                    if (pc.plane > 1) continue;
                    const ayther::AytherSession::ScreenCell sc{
                        pc.hash, pc.plane,
                        (uint8_t)(pc.screen_x >> 3), (uint8_t)(pc.screen_y >> 3) };
                    c_ambas.push_back(sc);
                    if (pc.plane == stable_p) c_solo.push_back(sc);
                }
            }
            auto match_id = [&](uint64_t id, uint32_t f) {
                s->replay_invalidate(); s->replay_seek(take, f);
                s->replay_invalidate();
                const FrameView* fv = s->replay_seek(take, f);
                return fv && fv->screen_match_id == id;
            };

            // Un Cuadro de UNA capa: indiferente a lo que haga la otra.
            s->define_screen(SOLO, m_solo, c_solo.data(), (uint32_t)c_solo.size(),
                             0.92f, 0.08f, "cuadro_una_capa.png");
            check(match_id(SOLO, fa), "Cuadro de UNA capa: matchea donde se capturó");
            check(match_id(SOLO, fb),
                  "Cuadro de UNA capa: SIGUE matcheando aunque la otra capa cambie");
            s->clear_screens();

            // El mismo par, declarando las DOS capas: la que cambió lo rompe.
            s->define_screen(AMBAS, m_ambas, c_ambas.data(), (uint32_t)c_ambas.size(),
                             0.92f, 0.08f, "cuadro_dos_capas.png");
            check(match_id(AMBAS, fa), "Cuadro de DOS capas: matchea donde se capturó");
            check(!match_id(AMBAS, fb),
                  "Cuadro de DOS capas: DEJA de matchear cuando una de ellas cambia");
            s->clear_screens();
        }
    }

    // ── 7. EL kPictureId VIENE DEL PACK ───────────────────────────────────────
    // La lección de la Utilería: un reconocimiento que sólo existe en la
    // sesión de autoría no llega al `.ay` entregado. Se hornea un pack real
    // con screens.toml, se abre, y el Cuadro tiene que matchear sin que nadie
    // haya llamado define_screen.
    {
        s->clear_screens();
        const uint32_t f_home = N / 2;
        std::vector<ayther::PackScreen> scr(1);
        scr[0].id         = 0xC0ADFACE;
        scr[0].name       = "Cuadro del pack";
        scr[0].plane_mask = kAll;
        scr[0].asset      = "cuadro_desde_el_pack.png";
        if (const FrameView* fv = s->replay_seek(take, f_home)) {
            for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
                const auto& pc = fv->plane_cells[i];
                if (pc.plane > 2) continue;
                scr[0].cells.push_back({ pc.hash, pc.plane,
                                         (uint8_t)(pc.screen_x >> 3),
                                         (uint8_t)(pc.screen_y >> 3) });
            }
        }
        const std::string toml = ayther::bake_screens_toml(scr);
        std::vector<ayther::PackScreen> rt;
        const size_t nrt = ayther::parse_screens_toml(toml, rt);
        check(nrt == 1 && rt.size() == 1 &&
              rt[0].cells.size() == scr[0].cells.size() &&
              rt[0].plane_mask == kAll && rt[0].asset == scr[0].asset,
              "screens.toml: round-trip bake→parse");

        const std::string pack_path =
            (std::filesystem::temp_directory_path() / "ayther_screens_smoke.ay").string();
        AytherPackBuilder* b = ayther_pack_builder_new();
        const std::string man =
            "[pack]\nname = \"screens_smoke\"\nversion = \"1.0.0\"\n"
            "game_id = \"smoke\"\nayther_min = \"0.8.0\"\n"
            "\n[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";
        ayther_pack_builder_add_bytes(b, "manifest.toml",
            reinterpret_cast<const uint8_t*>(man.data()), man.size());
        ayther_pack_builder_add_bytes(b, "screens.toml",
            reinterpret_cast<const uint8_t*>(toml.data()), toml.size());
        char perr[256] = {};
        const bool built = ayther_pack_builder_finish(b, true, pack_path.c_str(),
                                                      perr, sizeof(perr));
        ayther_pack_builder_free(b);
        check(built, "se horneó un .ay con screens.toml");
        if (built) {
            const auto pr = s->set_pack(pack_path);
            check(static_cast<bool>(pr), "la sesión abrió el pack");
            s->replay_invalidate(); s->replay_seek(take, f_home);
            s->replay_invalidate();
            const FrameView* fv = s->replay_seek(take, f_home);
            check(fv && fv->screen_sub_count == 1 &&
                  !std::strcmp(fv->screen_subs[0].asset_path,
                               "cuadro_desde_el_pack.png"),
                  "el Cuadro matchea DESDE EL PACK (sin define_screen)");
            check(fv && fv->screen_subs && fv->screen_subs[0].w_px == fv->fb_width &&
                  fv->screen_subs[0].h_px == fv->fb_height,
                  "el quad del Cuadro cubre la pantalla completa");
            std::error_code rec;
            std::filesystem::remove(pack_path, rec);
        }
    }

    // ── 8. LA ESCALERA: el Cuadro le gana a la Utilería ────────────────────
    // Regla del producto: el match prioriza de complejidad MAYOR a MENOR, y la
    // entidad que gana RECLAMA su cobertura — las contenidas en ella NO se
    // emiten. Un Cuadro es la pantalla entera en HD, así que la Utilería que
    // cae dentro ya está dibujada en ese asset: emitirla otra vez la duplicaría
    // encima. Ningún smoke cubría la interacción ENTRE tipos.
    {
        s->clear_screens();
        s->clear_plane_sets();
        const uint32_t f = N / 2;

        // Una Utilería de dos celdas adyacentes de plano A, tomadas del frame.
        uint64_t ha = 0, hb = 0; int ax = -1, ay = -1;
        if (const FrameView* fv = s->replay_seek(take, f)) {
            for (uint32_t i = 0; i < fv->plane_cell_count && ax < 0; ++i) {
                const auto& p0 = fv->plane_cells[i];
                if (p0.plane != 0) continue;
                for (uint32_t j = 0; j < fv->plane_cell_count; ++j) {
                    const auto& p1 = fv->plane_cells[j];
                    if (p1.plane == 0 && p1.screen_x == p0.screen_x + 8 &&
                        p1.screen_y == p0.screen_y) {
                        ha = p0.hash; hb = p1.hash;
                        ax = p0.screen_x; ay = p0.screen_y;
                        break;
                    }
                }
            }
        }
        check(ax >= 0, "hay un par de celdas adyacentes para la Utilería");

        auto props_visible = [&]() {
            s->replay_invalidate(); s->replay_seek(take, f);
            s->replay_invalidate();
            const FrameView* fv = s->replay_seek(take, f);
            for (uint32_t j = 0; fv && j < fv->plane_tile_sub_count; ++j)
                if (!std::strcmp(fv->plane_tile_subs[j].asset_path, "utileria.png"))
                    return true;
            return false;
        };

        if (ax >= 0) {
            const ayther::AytherSession::PlaneSetMember ms[2] = {
                { ha, 0, 0 }, { hb, 1, 0 } };
            s->define_plane_set(0x5E70, 0, 2, 1, ms, 2, "utileria.png");
        check(props_visible(), "sola, la Utilería SÍ se dibuja (control)");

            // Ahora el Cuadro de esa misma pantalla, que la contiene.
            std::vector<ayther::AytherSession::ScreenCell> cap;
            if (const FrameView* fv = s->replay_seek(take, f)) {
                for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
                    const auto& pc = fv->plane_cells[i];
                    if (pc.plane > 2) continue;
                    cap.push_back({ pc.hash, pc.plane,
                                    (uint8_t)(pc.screen_x >> 3),
                                    (uint8_t)(pc.screen_y >> 3) });
                }
            }
            s->define_screen(0xC0AD09, kAll, cap.data(), (uint32_t)cap.size(),
                             0.92f, 0.08f, "cuadro.png");
        check(!props_visible(),
                  "con el Cuadro activo, la Utilería que contiene NO se emite");

            // EL CASO QUE DECIDE: el mismo Cuadro reconocido pero SIN asset.
            // Un Cuadro es sus capas completas y le gana a la Utilería, pero
            // sólo cuando tiene algo que dibujar: si el artista todavía no le
            // asignó nada, no tapa nada y la Utilería se sigue dibujando sobre
            // el original. Es el flujo de la estrella que titila — se autora
            // sólo la estrella como Utilería y el resto de la pantalla queda
            // como está. En cuanto el Cuadro recibe un asset, se lleva la capa
            // entera y la Utilería deja de contar.
            //
            // Excluir celdas de la definición NO es la salida: el Cuadro
            // reclama la capa, no su subconjunto de celdas.
            {
                s->clear_screens();
                s->define_screen(0xC0AD0A, kAll, cap.data(), (uint32_t)cap.size(),
                                 0.92f, 0.08f, "");        // <- sin asset
        check(props_visible(),
                      "Cuadro reconocido SIN asset: no tapa, la Utilería se dibuja");
                s->clear_screens();
                s->define_screen(0xC0AD0A, kAll, cap.data(), (uint32_t)cap.size(),
                                 0.92f, 0.08f, "cuadro.png");   // <- con asset
        check(!props_visible(),
                      "al asignarle un asset, el Cuadro se lleva la capa entera y "
                      "la Utilería deja de contar");
            }

            // Y al retirar el Cuadro vuelve: la supresión es del Cuadro, no un
            // efecto colateral de haber redefinido el set.
            s->clear_screens();
        check(props_visible(),
                  "al retirar el Cuadro, la Utilería vuelve a dibujarse");
            s->clear_plane_sets();
        }
    }

    // ── CINEMÁTICA (CU004): la secuencia ordenada de Cuadros ─────────────────
    // Lo que agrega sobre Cuadros sueltos es el ORDEN, así que los oráculos que
    // valen son los del orden: que avance, que se cancele al romperse, y —el
    // más delicado— que NO se cancele en una transición legítima.
    {
        std::printf("\n-- Cinemática (CU004) --\n");
        s->clear_screens();
        s->clear_kinematics();
        s->set_hd_enabled(true);

        // Dos pantallas distintas que el juego muestra EN ESE ORDEN dentro de
        // la toma. Se capturan del frame donde se las ve.
        // Tienen que ser pantallas ESTABLES, no frames de fundido: se buscan
        // dos corridas de al menos `kRun` frames con la misma firma. Tomar «el
        // primer frame con firma distinta» agarraba un frame de transición que
        // no vuelve a aparecer nunca y por lo tanto no se confirma jamás — la
        // histéresis pide dos frames iguales seguidos.
        uint32_t fA = UINT32_MAX, fB = UINT32_MAX;
        {
            constexpr int kRun = 8;
            uint64_t cur = 0; int run_n = 0; uint32_t run_start = 0;
            uint64_t sigA = 0;
            for (uint32_t f = 0; f < N && fB == UINT32_MAX; ++f) {
                const uint64_t sg = sig_at(f, kAll);
                if (!sg) { cur = 0; run_n = 0; continue; }
                if (sg != cur) { cur = sg; run_n = 1; run_start = f; continue; }
                if (++run_n != kRun) continue;
                // Frame representativo: el del medio de la corrida.
                const uint32_t rep = run_start + kRun / 2;
                if (fA == UINT32_MAX) { fA = rep; sigA = sg; }
                else if (sg != sigA)  { fB = rep; }
            }
        }
        std::printf("[..] par ordenado: A=%u B=%s\n", fA,
                    fB == UINT32_MAX ? "no hallado" : std::to_string(fB).c_str());
        check(fA != UINT32_MAX && fB != UINT32_MAX,
              "control: la toma muestra dos pantallas distintas en orden");

        if (fA != UINT32_MAX && fB != UINT32_MAX) {
            constexpr uint64_t SA = 0xC15E01, SB = 0xC15E02, KIN = 0xC15E10;
            auto capture = [&](uint32_t f, uint64_t id) {
                const FrameView* fv = s->replay_seek(take, f);
                if (!fv) return;
                std::vector<ayther::AytherSession::ScreenCell> c;
                for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
                    const auto& pc = fv->plane_cells[i];
                    if (pc.plane > 2) continue;
                    c.push_back({ pc.hash, pc.plane,
                                  (uint8_t)(pc.screen_x >> 3), (uint8_t)(pc.screen_y >> 3) });
                }
                s->define_screen(id, kAll, c.data(), (uint32_t)c.size(),
                                 0.92f, 0.08f, id == SA ? "a.png" : "b.png");
            };
            capture(fA, SA);
            capture(fB, SB);

            // La histéresis del Cuadro pide DOS frames consecutivos para
            // confirmar, así que un solo seek a un frame nunca lo confirma —
            // hay que llegar caminando. Vale también en la app: tras un scrub
            // el HD aparece un frame después.
            auto settle = [&](uint32_t f) -> const FrameView* {
                if (f > 0) s->replay_seek(take, f - 1);
                return s->replay_seek(take, f);
            };
            // El recorrido llega UNOS FRAMES MÁS ALLÁ de B a propósito: en el
            // frame donde B aparece por primera vez todavía no está confirmado
            // (es el primero de los dos que pide la histéresis).
            const uint32_t fEnd = (fB + 8 < N) ? fB + 8 : N - 1;
            auto run = [&](uint32_t from, uint32_t to, uint32_t* out_first,
                           uint32_t* out_maxstep, int* out_drops) {
                *out_first = UINT32_MAX; *out_maxstep = 0; *out_drops = 0;
                bool was_on = false;
                s->replay_invalidate();
                for (uint32_t f = from; f <= to; ++f) {
                    const FrameView* fv = s->replay_seek(take, f);
                    if (!fv) continue;
                    if (fv->kinematic_id) {
                        if (*out_first == UINT32_MAX) *out_first = fv->kinematic_step;
                        if (fv->kinematic_step > *out_maxstep)
                            *out_maxstep = fv->kinematic_step;
                        was_on = true;
                    } else if (was_on) { ++*out_drops; was_on = false; }
                }
            };

            const ayther::AytherSession::KinematicStep fwd[2] = {
                { SA, "cine_0.png" }, { SB, "cine_1.png" } };
            s->define_kinematic(KIN, fwd, 2);
            uint32_t first = 0, maxstep = 0; int drops = 0;
            run(fA, fEnd, &first, &maxstep, &drops);
            std::printf("[..] recorrido: entra en el paso %u, llega al %u, %d caida(s)\n",
                        first, maxstep, drops);
            check(first == 0, "entra por el paso 0 (es donde arranca el contenido)");
            check(maxstep >= 1, "la secuencia AVANZA al paso siguiente");
            // EL CHEQUEO DELICADO: `screen_match_id` cae a 0 durante un frame en
            // toda transición limpia (la histéresis pide 2 frames para confirmar
            // el nuevo Cuadro). Sin la tolerancia de gap, la Cinemática se
            // cancelaría en CADA paso legítimo y esto contaría una caída.
            check(drops == 0,
                  "NO se cancela en la transición legítima (tolerancia del hueco de la histéresis)");

            // El asset del paso GANA sobre el del Cuadro (Kinematic > Picture).
            {
                const FrameView* fv = settle(fA);
                const bool own = fv && fv->screen_sub_count == 1 && fv->screen_subs &&
                                 std::string(fv->screen_subs[0].asset_path)
                                     .find("cine_") != std::string::npos;
                check(own, "el asset del PASO gana sobre el del Cuadro");
                check(fv && fv->kinematic_steps == 2, "publica el largo de la secuencia");
            }

            // Re-producir el MISMO frame no puede mover el cursor: produce_frame
            // corre de nuevo en el re-render bare del compose y en export_frame.
            {
                const FrameView* v1 = settle(fEnd);
                const uint32_t st1 = v1 ? v1->kinematic_step : 999;
                const uint64_t id1 = v1 ? v1->kinematic_id : 0;
                s->replay_invalidate();
                const FrameView* v2 = s->replay_seek(take, fEnd);
                const uint32_t st2 = v2 ? v2->kinematic_step : 998;
                check(id1 != 0, "control: hay cinemática viva para el chequeo de re-produce");
                check(st1 == st2, "re-producir el mismo frame NO avanza el cursor");
            }

            // Un SALTO re-evalúa desde cero en vez de leerse como «se rompió»:
            // se entra en el paso que corresponda al contenido de la pantalla.
            {
                s->replay_invalidate();
                const FrameView* fv = settle(fEnd);
                check(fv && fv->kinematic_id == KIN,
                      "tras un salto, la Cinemática se re-engancha (entrada en cualquier paso)");
                check(fv && fv->kinematic_step >= 1,
                      "y se re-engancha en el paso que dice la PANTALLA, no en el 0");
            }

            // CONTROL NEGATIVO: la MISMA pareja en orden inverso nunca avanza.
            // Si esto pasara igual, el «orden» no estaría haciendo nada y toda
            // la entidad sería un Cuadro con más pasos.
            {
                const ayther::AytherSession::KinematicStep rev[2] = {
                    { SB, nullptr }, { SA, nullptr } };
                s->clear_kinematics();
                s->define_kinematic(KIN, rev, 2);
                uint32_t fi = 0, ms = 0; int dr = 0;
                run(fA, fEnd, &fi, &ms, &dr);
                std::printf("[..] orden inverso: entra en el paso %u, llega al %u\n", fi, ms);
                // Con la pareja invertida se ENTRA en el paso 1 (la pantalla A
                // es el último paso de esa secuencia) y eso es correcto: la
                // entrada es por contenido. Lo que NO puede pasar es AVANZAR —
                // si avanzara, el orden no estaría haciendo nada y la entidad
                // entera sería un Cuadro con más pasos.
                check(ms == fi, "en orden inverso NO avanza: sólo entra (el orden importa)");
            }

            // Una secuencia de UN paso es un Cuadro con otro nombre: se rechaza.
            {
                const ayther::AytherSession::KinematicStep one[1] = { { SA, nullptr } };
                s->clear_kinematics();
                s->define_kinematic(0xC15E99, one, 1);
                s->replay_invalidate();
                const FrameView* fv = settle(fA);
                check(fv && fv->kinematic_id == 0,
                      "una secuencia de un solo paso se rechaza (es un Cuadro)");
            }

            s->clear_kinematics();
            s->clear_screens();
            s->replay_invalidate();
            const FrameView* fv = settle(fA);
            check(fv && fv->kinematic_id == 0, "clear_kinematics la retira");
        }
        s->set_hd_enabled(false);
    }

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
