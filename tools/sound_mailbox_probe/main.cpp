// ---------------------------------------------------------------------------
// sound_mailbox_probe — F1a de #507: ¿dónde deja el 68k el ID DEL SONIDO?
//
// EL PROBLEMA QUE HABILITA. Una Secuencia de audio se arma desde una toma, y
// en una toma la música viene mezclada con los efectos: cuando un golpe pisa el
// canal 5, la melodía DEJA DE EXISTIR ahí. Ninguna limpieza posterior recupera
// lo que el driver no tocó. La salida es pedirle el tema al driver directamente
// — el 68k le pasa al Z80 un id de sonido por una casilla de memoria, y si el
// Lab sabe escribir esa casilla puede grabar cualquier tema limpio y en loop.
//
// Esta sonda encuentra la casilla. Es el paso F1a del plan de la issue.
//
// #563: LA RAM DEL Z80 YA SE PUEDE MIRAR. Cuando esto se escribió, la ABI del
// fork no la exponía —sus regiones eran VRAM, CRAM, VDP regs, VSRAM, audio
// writes, sprites y máscaras— y el barrido tenía que conformarse con work RAM.
// En Golden Axe eso dejó dos candidatos que la confirmación automática descartó
// a los dos, y la única hipótesis que quedaba era justamente esa RAM.
//
// Desde la ABI 1.9 está: `--space z80` barre los 8 KB de 0xA00000-0xA01FFF con
// el MISMO método —diferencial alrededor del primer key-on, intersección de
// varios arranques, confirmación por dos pasadas—. Lo único que cambia es de
// dónde salen los bytes. En los drivers Sega de la época el 68k suele
// dejar el id en WORK RAM para que el V-int lo copie al Z80, y la work RAM sí
// se lee hoy.
//
// EL MÉTODO. Diferencial alrededor del arranque de un tema:
//
//   1. correr hasta encontrar el primer KEY-ON de FM (registro $28 del YM2612
//      con al menos un operador encendido) — ése es «el tema arrancó»;
//   2. comparar la work RAM del frame ANTERIOR con la del frame del key-on;
//   3. quedarse con los bytes que cambiaron a un valor CHICO (un id de sonido
//      es un byte bajo, no un puntero ni un contador) y que no venían
//      cambiando en los frames previos (descarta timers, RNG y contadores).
//
// El tercer filtro es el que hace la diferencia: sin él, un frame cualquiera de
// Mega Drive tiene decenas de bytes que cambian y la lista es inútil.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target sound_mailbox_probe
//   Args:  <core.dll> <rom> [frames]
//
// La toma no hace falta: se corre el boot, que es más reproducible que depender
// del proyecto de alguien. Con la ROM sola alcanza para el título.
// ---------------------------------------------------------------------------
#include "libretro_host/retro_runner.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {

/// Un key-on de FM es una escritura al registro $28 cuyos 4 bits altos —los
/// operadores— no son todos cero. El key-OFF usa el mismo registro con los
/// cuatro en cero, y confundirlos daría «el tema arrancó» cuando en realidad
/// terminó.
bool is_fm_key_on(const ayther_audio_write_v1& w) {
    return w.chip == 0 && (w.addr & 0xFF) == 0x28 && (w.data & 0xF0) != 0;
}

struct Candidate {
    uint32_t addr;
    uint8_t  before, after;
    uint32_t churn;   ///< en cuántos frames previos ya venía cambiando
};

}  // namespace

/// #507 F1a-confirm: escribir el id y VER si suena, sin oido.
///
/// Corre hasta `at`, escribe `id` en la direccion candidata (vista 68k) y
/// cuenta los key-on de FM de los siguientes `win` frames. Compara contra una
/// pasada de CONTROL identica que no escribe nada. Si la casilla es el mailbox,
/// la pasada que escribe dispara notas que la de control no tiene.
///
/// Por que cuenta key-ons y no RMS: un tema que arranca es un pico de key-ons
/// aunque su volumen tarde en subir, y el contador no depende de que el audio
/// llegue a un device. Es el mismo criterio con el que se detectan los
/// arranques mas arriba.
int confirm(const std::string& core, const std::string& rom,
            uint32_t addr68, uint8_t id, int at, int win) {
    auto run = [&](bool write) -> int {
        RetroRunner r;
        if (!r.init(core, rom)) return -1;
        r.subscribe_all_supported();
        std::vector<ayther_audio_write_v1> aw(4096);
        int keyons = 0;
        for (int f = 0; f < at + win; ++f) {
            if (write && f == at) {
                // La work RAM esta word-swapped: la direccion 68k 0xFFxxxx cae
                // en el byte (offset ^ 1). Escribir el crudo toca el vecino.
                if (addr68 >= 0xA00000u && addr68 < 0xA02000u) {
                    // #563: RAM del Z80. SIN word-swap: es un procesador de 8
                    // bits y el fork la publica tal cual — aplicar el `^1` acá
                    // escribiría el byte de al lado, que es exactamente el
                    // error que la vista 68k de work RAM existe para evitar del
                    // otro lado.
                    const uint32_t off = addr68 - 0xA00000u;
                    if (off < r.z80_ram_size()) r.z80_ram_mut()[off] = id;
                } else {
                    // La work RAM esta word-swapped: la direccion 68k 0xFFxxxx
                    // cae en el byte (offset ^ 1). Escribir el crudo toca el
                    // vecino.
                    const uint32_t off = (addr68 - 0xFF0000u) ^ 1u;
                    if (off < r.work_ram_size()) r.work_ram_mut()[off] = id;
                }
            }
            r.run_frame();
            if (f < at) continue;
            ayther_frame_snapshot_v1 sn{};
            if (!r.capture_frame_snapshot(sn).ok()) continue;
            const auto rd = r.read_audio_writes_v1(
                aw.data(), static_cast<uint32_t>(aw.size()), sn);
            if (rd.ok())
                for (uint32_t i = 0; i < rd.count; ++i)
                    if (is_fm_key_on(aw[i])) ++keyons;
        }
        return keyons;
    };
    const int base = run(false);
    const int test = run(true);
    if (base < 0 || test < 0) { std::fprintf(stderr, "[FAIL] init\n"); return 1; }
    std::printf("=== confirmacion 0x%06X = 0x%02X (frame %d, ventana %d) ===\n",
                addr68, id, at, win);
    std::printf("  key-ons control : %d\n  key-ons escribiendo: %d\n", base, test);
    if (test > base * 2 + 4) {
        std::printf("\n[ok] ES el mailbox: escribir el id disparo notas que el"
                    " control no tiene.\n");
        return 0;
    }
    std::printf("\n[--] sin diferencia clara: o no es la casilla, o el driver"
                " la lee en otro\n     momento (probá otro `at`) o el id no"
                " existe.\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "uso: sound_mailbox_probe <core.dll> <rom> [frames]\n");
        return 2;
    }
    // Modo confirmacion: <core> <rom> --confirm <addr68> <id> [at] [win]
    if (argc >= 6 && std::strcmp(argv[3], "--confirm") == 0) {
        const uint32_t a = (uint32_t)std::strtoul(argv[4], nullptr, 0);
        const uint8_t  i = (uint8_t)std::strtoul(argv[5], nullptr, 0);
        const int at  = argc > 6 ? std::atoi(argv[6]) : 900;
        const int win = argc > 7 ? std::atoi(argv[7]) : 180;
        return confirm(argv[1], argv[2], a, i, at, win);
    }
    const std::string core = argv[1], rom = argv[2];
    const int frames = argc > 3 ? std::atoi(argv[3]) : 2000;
    // El 68k PIDE el sonido y el Z80 lo toca varios frames despues: el
    // mailbox se escribe ANTES del key-on. Comparar contra f-1 mira el
    // frame equivocado y devuelve el estado que el juego movio mientras
    // tanto (tablas de objetos, contadores). La ventana lo corrige.
    const int lag = argc > 4 ? std::atoi(argv[4]) : 8;

    RetroRunner runner;
    if (!runner.init(core, rom)) {
        std::fprintf(stderr, "[FAIL] no se pudo cargar core+ROM\n");
        return 1;
    }
    // El fork no instrumenta hasta que se lo piden: sin esto el log de audio
    // viene VACIO (ceros que parecen silencio).
    runner.subscribe_all_supported();
    const bool abi = runner.has_ayther_v1();
    std::printf("=== sound_mailbox_probe (#507 F1a) ===\nrom: %s\nABI v1: %s\n\n",
                rom.c_str(), abi ? "sí" : "NO (sin log de audio no hay key-on)");
    if (!abi) return 2;

    // #563: QUE ESPACIO se barre. `--space z80` mira la RAM del Z80 (ABI 1.9);
    // sin eso, work RAM, que es lo que hacia siempre. El metodo es el mismo —
    // diferencial alrededor del primer key-on, interseccion de arranques,
    // confirmacion por dos pasadas—: lo unico que cambia es de donde salen los
    // bytes y en que direccion base se reportan los candidatos.
    bool z80 = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--space" && i + 1 < argc)
            z80 = (std::string(argv[i + 1]) == "z80");
    const uint32_t base68 = z80 ? 0xA00000u : 0xFF0000u;
    if (z80 && runner.z80_ram_size() == 0) {
        // Se dice y se sale, en vez de barrer un buffer vacio y reportar cero
        // candidatos: «no hay region» y «no encontre nada» son cosas distintas.
        std::fprintf(stderr,
            "[FAIL] este core no expone la RAM del Z80 (hace falta ABI 1.9, #563)\n");
        return 2;
    }
    const size_t rsz = z80 ? runner.z80_ram_size() : runner.work_ram_size();
    if (!rsz) { std::fprintf(stderr, "[FAIL] sin work RAM\n"); return 1; }

    // Anillo de los ultimos `lag+1` snapshots: el diferencial se toma
    // contra el mas viejo.
    std::vector<std::vector<uint8_t>> ring(static_cast<size_t>(lag) + 1);
    for (auto& r : ring) r.resize(rsz);
    size_t ring_n = 0;
    std::vector<uint8_t> prev(rsz), cur(rsz);
    // Cuántas veces cambió cada byte ANTES del key-on. Un contador de frames o
    // un RNG cambia todo el tiempo; el mailbox, casi nunca.
    std::vector<uint32_t> churn(rsz, 0);
    std::vector<ayther_audio_write_v1> aw(4096);

    bool have_prev = false;
    int  key_frame = -1;
    // #507: UN disparo no alcanza. El mailbox es la direccion que cambia en
    // TODOS los arranques y con un valor DISTINTO en cada uno (el id del tema);
    // una tabla de estado cambia igual en todos y se descarta sola. Por eso se
    // detectan varios arranques —key-on despues de un silencio largo— y se
    // intersecan.
    struct Hit { uint32_t addr; uint8_t val; };
    std::vector<std::vector<Hit>> shots;
    int quiet = 0;          // frames seguidos sin key-on
    const int kQuietGap = 45;   // ~0,75 s: separa temas, no notas

    for (int f = 0; f < frames; ++f) {
        runner.run_frame();
        std::memcpy(cur.data(), z80 ? runner.z80_ram() : runner.work_ram(), rsz);
        std::memcpy(ring[ring_n % ring.size()].data(), cur.data(), rsz);
        ++ring_n;

        ayther_frame_snapshot_v1 snap{};
        bool key_on = false;
        if (runner.capture_frame_snapshot(snap).ok()) {
            const auto r = runner.read_audio_writes_v1(
                aw.data(), static_cast<uint32_t>(aw.size()), snap);
            if (r.ok())
                for (uint32_t i = 0; i < r.count; ++i)
                    if (is_fm_key_on(aw[i])) { key_on = true; break; }
        }

        // ARRANQUE = key-on despues de un silencio largo. Sin el silencio de
        // por medio cada nota contaria como tema nuevo y la interseccion se
        // quedaria sin poder de discriminacion.
        const bool arranca = key_on && quiet >= kQuietGap && ring_n > ring.size();
        quiet = key_on ? 0 : quiet + 1;
        if (!arranca) {
            if (have_prev)
                for (size_t a = 0; a < rsz; ++a)
                    if (prev[a] != cur[a]) ++churn[a];
            prev.swap(cur);
            have_prev = true;
            continue;
        }

        const std::vector<uint8_t>& base = ring[ring_n % ring.size()];
        if (key_frame < 0) key_frame = f;
        std::vector<Hit> hits;
        for (size_t a = 0; a < rsz; ++a) {
            if (base[a] == cur[a]) continue;
            if (cur[a] == 0 || cur[a] > 0xC0) continue;
            // El churn NO filtra aca: en el segundo arranque casi todo cambio
            // alguna vez y el mailbox tambien (se escribio en el primero). El
            // filtro de verdad es la INTERSECCION mas el "valor distinto": una
            // direccion que se mueve por otra razon rara vez coincide en TODOS
            // los arranques con un valor nuevo cada vez.
            hits.push_back(Hit{static_cast<uint32_t>(a), cur[a]});
        }
        std::printf("arranque #%zu en el frame %d - %zu candidatos\n",
                    shots.size() + 1, f, hits.size());
        shots.push_back(std::move(hits));

        if (have_prev)
            for (size_t a = 0; a < rsz; ++a)
                if (prev[a] != cur[a]) ++churn[a];
        prev.swap(cur);
        have_prev = true;
    }

    if (shots.empty()) {
        std::printf("no hubo ningun ARRANQUE en %d frames (key-on tras silencio"
                    " de %d)\n", frames, kQuietGap);
        return 2;
    }
    std::printf("\narranques detectados: %zu\n\n", shots.size());
    if (shots.size() < 2) {
        std::printf("Con UN solo arranque no se puede discriminar: una tabla de"
                    " estado cambia\nigual que el mailbox. Corre mas frames para"
                    " agarrar un segundo tema.\n");
        return 2;
    }

    // La INTERSECCION: direcciones que cambiaron en TODOS los arranques. Y
    // entre esas, las que quedaron con un valor DISTINTO en cada uno son las
    // candidatas de verdad — un id de tema cambia de tema a tema; un flag de
    // "esta sonando" vale lo mismo siempre.
    std::vector<uint32_t> inter;
    for (const Hit& h : shots[0]) {
        bool present_in_all = true;
        for (size_t s2 = 1; s2 < shots.size() && present_in_all; ++s2) {
            bool found = false;
            for (const Hit& o : shots[s2]) if (o.addr == h.addr) { found = true; break; }
            present_in_all = found;
        }
        if (present_in_all) inter.push_back(h.addr);
    }
    std::printf("en TODOS los arranques: %zu direcciones\n\n", inter.size());

    std::printf("MAILBOX candidato (valor distinto por arranque):\n");
    size_t n_var = 0;
    for (uint32_t a : inter) {
        std::vector<uint8_t> vals;
        for (const auto& sh : shots)
            for (const Hit& h : sh) if (h.addr == a) { vals.push_back(h.val); break; }
        bool varia = false;
        for (size_t i = 1; i < vals.size(); ++i) if (vals[i] != vals[0]) { varia = true; break; }
        if (!varia) continue;
        // #563: la RAM del Z80 no esta word-swapped —es un procesador de 8
        // bits— asi que el `^1` solo aplica a work RAM. Reportar la direccion
        // con el swap puesto en el espacio equivocado manda al que la lea un
        // byte al lado.
        std::printf("  0x%06X  ", base68 + (z80 ? a : (a ^ 1u)));
        for (uint8_t v : vals) std::printf("0x%02X ", v);
        std::printf("\n");
        if (++n_var >= 15) { std::printf("  ...\n"); break; }
    }
    if (!n_var)
        std::printf("  (ninguna varia: probá mas arranques o revisá el lag)\n");
    else
        std::printf("\nConfirmar es escribir uno de esos ids en esa direccion y"
                    " oir el tema.\n");
    return 0;
}
