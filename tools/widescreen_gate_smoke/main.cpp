// ---------------------------------------------------------------------------
// widescreen_gate_smoke (#231 EM-8.2) — el ancho lo decide el PACK, por frame.
//
// POR QUÉ EXISTE. `core/src/widescreen_gate.rs` fija el gate como función pura
// (7 casos): compila el TOML y elige el ancho. Lo que eso NO puede afirmar es
// que el ancho elegido LLEGUE al frame — que la sesión lo evalúe con la RAM
// viva, en el momento correcto del produce, y que apagar el gate devuelva el
// control intacto a `set_widescreen()`.
//
// Ese cableado tiene dos formas conocidas de romperse, y las dos se ven acá:
//
//   · EL FRAME DE DESFASE. Si el gate se evalúa DESPUÉS de que la Panorámica
//     emitió sus quads, el ancho del frame N usa la condición del frame N-1.
//     En la transición a un menú eso es exactamente el «artefacto» que el
//     criterio de aceptación prohíbe: un frame ensanchado de más con la lámina
//     que ya no corresponde. (Pasó: la primera versión evaluaba junto al gate
//     de audio, ~2.000 líneas después de la emisión de la tira.)
//
//   · EL PEDIDO PISADO. Si el gate escribe sobre `wide_w` en vez de sobre un
//     ancho efectivo aparte, desarmarlo deja el ensanchado apagado para
//     siempre y el Lab ya no puede ponerlo a mano.
//
// EL AC DE EM-8.2 se comprueba con la RAM REAL del juego: se busca una
// dirección que separe dos estados de una toma, se declara el gate sobre ella,
// y se verifica que el ancho cambia solo. Sin ROM no hay AC que comprobar — un
// gate sobre RAM sintética ya lo cubren los tests del core.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target widescreen_gate_smoke
//   Run:   bin/widescreen_gate_smoke <toma.arp>   (ROM en AYTHER_PROBE_ROM)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_env.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using namespace ayther;

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

std::string cfg_quoted(const std::string& l) {
    const size_t a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string()
                                              : l.substr(a + 1, b - a - 1);
}
std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\')
        return p;
    return base + "/" + p;
}

constexpr uint32_t kWide = 398;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: widescreen_gate_smoke <toma.arp>\n");
        return 2;
    }
    const std::string root = AYTHER_SOURCE_DIR;
    std::string core, rom, line;
    {
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (core.empty() && line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos)
                core = cfg_quoted(line);
    }
    core = resolve(core, root);
    if (const char* e = ayther::env_get("AYTHER_PROBE_ROM")) rom = e;
    if (core.empty() || rom.empty()) {
        std::fprintf(stderr, "[skip] falta core o AYTHER_PROBE_ROM\n");
        return 0;
    }

    AytherSession::Config c;
    c.core_path = core; c.rom_path = rom;
    c.enable_audio = false; c.derive_core_pack = false;
    auto r = AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] sesion\n"); return 1; }
    std::unique_ptr<AytherSession>& s = *r;

    auto rec_opt = AytherRecording::load(argv[1]);
    if (!rec_opt) { std::fprintf(stderr, "[FAIL] no abre la toma\n"); return 1; }
    const AytherRecording rec = std::move(*rec_opt);

    std::printf("=== widescreen_gate_smoke (#231 EM-8.2) ===\ntoma %s (%u frames)\n\n",
                argv[1], rec.frame_count());

    const uint32_t fA = rec.frame_count() / 4;
    const uint32_t fB = rec.frame_count() * 3 / 4;

    // -- Una dirección de RAM que DISTINGA los dos frames ---------------------
    // El AC habla de «un flag de RAM». Cuál es depende del juego, así que en vez
    // de hardcodear una se busca: cualquier byte que valga distinto en fA y fB
    // sirve para demostrar que el gate sigue la RAM. Se exige además que sea
    // ESTABLE unos frames alrededor de cada uno — un byte que cambia todo el
    // tiempo (un contador, el RNG) haría pasar el test por casualidad y no
    // probaría que el gate lee lo que cree leer.
    auto byte_at = [&](uint32_t f, uint32_t addr) -> int {
        if (!s->replay_seek(rec, f)) return -1;
        const uint8_t* ram = s->work_ram();
        const size_t   n   = s->work_ram_size();
        if (!ram || addr >= n) return -1;
        return ram[addr ^ 1];   // la work RAM del 68000 se ve word-swapped
    };
    uint32_t addr = 0; int vA = -1, vB = -1;
    {
        s->replay_seek(rec, fA);
        const size_t n = s->work_ram_size();
        std::vector<uint8_t> a(n), b(n);
        if (const uint8_t* p = s->work_ram()) std::copy(p, p + n, a.begin());
        s->replay_seek(rec, fB);
        if (const uint8_t* p = s->work_ram()) std::copy(p, p + n, b.begin());
        for (size_t i = 0; i < n && !addr; ++i) {
            if (a[i] == b[i]) continue;
            const uint32_t cand = (uint32_t)(i ^ 1);   // vuelta a vista 68k
            const int a0 = byte_at(fA, cand), a1 = byte_at(fA + 3, cand);
            const int b0 = byte_at(fB, cand), b1 = byte_at(fB + 3, cand);
            if (a0 < 0 || b0 < 0 || a0 != a1 || b0 != b1 || a0 == b0) continue;
            addr = cand; vA = a0; vB = b0;
        }
    }
    std::printf("  discriminante: 0x%06X  f%u=0x%02X  f%u=0x%02X\n\n",
                addr, fA, vA, fB, vB);
    check(addr != 0, "CONTROL: hay una direccion de RAM estable que separa los dos frames");
    if (!addr) return 1;

    // -- El gate: ensancha donde vale vA, 4:3 en el resto ---------------------
    char toml[512];
    std::snprintf(toml, sizeof(toml),
        "[[widescreen]]\nwidth = %u\n"
        "[[widescreen.condition]]\nkind = \"memory_const\"\naddr = %u\n"
        "width = \"u8\"\nop = \"eq\"\nvalue = %d\n\n"
        "[[widescreen]]\nwidth = 0\n", kWide, addr, vA);

    s->set_widescreen(0);
    s->set_widescreen_gate(toml);
    check(s->widescreen_gated(), "el gate queda armado");

    auto width_at = [&](uint32_t f) -> uint32_t {
        s->replay_invalidate();
        const FrameView* fv = s->replay_seek(rec, f);
        return fv ? fv->wide_w : 0u;
    };

    // EL AC, en las dos direcciones. Que ensanche donde corresponde no alcanza:
    // un gate que devolviera siempre kWide pasaría la mitad del test.
    const uint32_t wA = width_at(fA), wB = width_at(fB);
    std::printf("  f%u -> %u   ·   f%u -> %u\n", fA, wA, fB, wB);
    check(wA == kWide, "donde la condicion se cumple, el frame ENSANCHA");
    check(wB == 0, "y donde no, vuelve a 4:3 — el AC de EM-8.2");

    // SIN DESFASE DE UN FRAME. Ir y volver alternando: si el gate se evaluara
    // después de que la Panorámica emitió, cada ancho llegaría con la condición
    // del frame anterior y esta secuencia saldría corrida.
    {
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            if (width_at(fA) != kWide) ok = false;
            if (width_at(fB) != 0)     ok = false;
        }
        check(ok, "alternando ida y vuelta el ancho NUNCA llega corrido un frame");
    }

    // EL PEDIDO NO SE PISA. Desarmar el gate tiene que devolver el control a
    // set_widescreen() con lo que estaba puesto — si el gate hubiera escrito
    // sobre `wide_w`, acá el ensanchado quedaría apagado para siempre.
    s->set_widescreen(kWide);
    check(width_at(fB) == 0, "con el gate puesto, el pedido manual NO gana");
    s->set_widescreen_gate("");
    check(!s->widescreen_gated(), "el gate se desarma con texto vacio");
    check(width_at(fB) == kWide, "desarmado, el pedido manual vuelve INTACTO");

    // Un pack sin `[[widescreen]]` no arma gate — no lo apaga. Es lo que hace
    // que todos los packs ya horneados sigan andando.
    s->set_widescreen_gate("[[sub]]\nhash = \"0x1\"\n");
    check(!s->widescreen_gated(), "un TOML sin [[widescreen]] no arma gate");
    check(width_at(fB) == kWide, "y por lo tanto no apaga el ensanchado manual");

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
