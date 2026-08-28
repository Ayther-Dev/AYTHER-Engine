// ---------------------------------------------------------------------------
// clean_theme_probe — grabar un tema LIMPIO por id, repetible (#563 / #507).
//
// # El problema
//
// Una Secuencia de audio se arma desde una toma, y en una toma la música viene
// mezclada con los efectos: cuando un golpe pisa el canal 5, la melodía DEJA DE
// EXISTIR ahí. Ninguna limpieza posterior recupera lo que el driver no tocó.
//
// La salida es pedirle el tema al driver: el 68k le pasa al Z80 un id de sonido
// por una casilla de memoria, y quien sepa escribir esa casilla puede grabar
// cualquier tema limpio, en loop y sin nada encima.
//
// # Por qué esto y no el botón
//
// La issue pide un botón «Grabar tema limpio» en Mezclar. Esto es el flujo que
// ese botón necesita, hecho sin UI: dado el core, la ROM, la casilla y un id,
// arranca una sesión LIMPIA, deja el id, graba la ventana y escribe el WAV.
//
// Y hace lo que el botón no: **es repetible por catálogo**. Con una lista de
// ids se graban los cuarenta temas de un juego en una corrida, que es
// exactamente el volumen que la issue dice esperar antes de que valga la pena
// automatizar. El botón es azúcar sobre esto.
//
// # La casilla NO se adivina: la encuentra `sound_mailbox_probe`
//
// Es por juego. Esta herramienta la recibe, no la busca — separar el
// descubrimiento de la ejecución es lo que permite que el descubrimiento se
// haga una vez y la grabación, cuarenta.
//
// Uso:
//   clean_theme_probe <core> <rom> <addr68> <id> <salida.wav> [frames] [at]
//
//   addr68  la casilla, del `sound_mailbox_probe`. 0xA0xxxx = RAM del Z80
//           (#563, ABI 1.9); 0xFFxxxx = work RAM.
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 6) {
        std::printf(
            "uso: clean_theme_probe <core> <rom> <addr68> <id> <salida.wav> "
            "[frames] [at]\n"
            "  addr68  la casilla del sound_mailbox_probe.\n"
            "          0xA0xxxx = RAM del Z80 (#563) - 0xFFxxxx = work RAM\n");
        return 2;
    }
    const std::string core = argv[1], rom = argv[2], wav = argv[5];
    const uint32_t addr68 = (uint32_t)std::strtoul(argv[3], nullptr, 0);
    const uint8_t  id     = (uint8_t)std::strtoul(argv[4], nullptr, 0);
    const uint32_t frames = argc > 6 ? (uint32_t)std::atoi(argv[6]) : 600;
    // `at` es cuántos frames dejar correr ANTES de escribir el id. Un juego
    // recién arrancado todavía está inicializando su driver de sonido, y
    // escribir la casilla ahí se pierde con la inicialización.
    const uint32_t at = argc > 7 ? (uint32_t)std::atoi(argv[7]) : 300;

    ayther::AytherSession::Config cfg;
    cfg.core_path = core;
    cfg.rom_path  = rom;
    // CON audio, pero headless: el mixdown re-simula, no necesita el device.
    cfg.enable_audio = false;
    cfg.derive_core_pack = false;
    auto abierta = ayther::AytherSession::create(cfg);
    if (!abierta) {
        std::fprintf(stderr, "[FAIL] no abrio: %s\n", abierta.error.message.c_str());
        return 1;
    }
    ayther::AytherSession& s = **abierta.value;

    const bool z80 = (addr68 >= 0xA00000u && addr68 < 0xA02000u);
    if (z80 && s.z80_ram_size() == 0) {
        // Se dice y se sale: «este core no puede» y «no funcionó» son cosas
        // distintas, y la primera tiene arreglo conocido (usar el core del
        // fork con ABI 1.9).
        std::fprintf(stderr,
            "[FAIL] la casilla esta en la RAM del Z80 y este core no la expone.\n"
            "       Hace falta el core del fork con ABI 1.9 (#563).\n");
        return 2;
    }

    std::printf("=== clean_theme_probe (#563) ===\n");
    std::printf("  casilla: 0x%06X (%s)  id: 0x%02X\n",
                addr68, z80 ? "RAM del Z80" : "work RAM", (unsigned)id);

    // -- 1. Sesión limpia hasta `at` -----------------------------------------
    //
    // La toma arranca ANTES de escribir el id: así el WAV incluye el silencio
    // previo y el ataque completo del tema. Empezar a grabar después del
    // key-on se come el principio, que es justo lo que un autor necesita para
    // alinear su HD.
    s.record_start();
    for (uint32_t f = 0; f < at; ++f) s.step();

    // -- 2. Dejar el id donde el driver lo lee -------------------------------
    if (z80) {
        // SIN word-swap: el Z80 es de 8 bits y el fork publica su RAM tal cual.
        const uint32_t off = addr68 - 0xA00000u;
        if (!s.z80_poke(off, &id, 1)) {
            std::fprintf(stderr, "[FAIL] no se pudo escribir la casilla del Z80\n");
            return 1;
        }
    } else {
        // Work RAM está word-swapped y `poke` ya aplica el `^1`: se le pasa la
        // dirección lógica del 68k, no el offset crudo.
        const uint32_t off = addr68 - 0xFF0000u;
        if (!s.poke(off, &id, 1)) {
            std::fprintf(stderr, "[FAIL] no se pudo escribir la casilla\n");
            return 1;
        }
    }

    // -- 3. Grabar la ventana -------------------------------------------------
    for (uint32_t f = 0; f < frames; ++f) s.step();
    const ayther::AytherRecording rec = s.take_recording();
    if (rec.frame_count() == 0) {
        std::fprintf(stderr, "[FAIL] la toma salio vacia\n");
        return 1;
    }

    // -- 4. El WAV ------------------------------------------------------------
    //
    // Mixdown SIN hd: lo que se quiere es el audio del EMULADOR, que es la
    // referencia contra la que el autor va a hacer su versión. Con hd puesto
    // grabaría lo que ya sustituyó, que no sirve de referencia de nada.
    if (!s.export_mixdown_wav(rec, at, at + frames - 1, wav.c_str(), /*hd=*/false)) {
        std::fprintf(stderr, "[FAIL] no se pudo escribir el WAV\n");
        return 1;
    }
    std::printf("  toma: %u frames\n", (unsigned)rec.frame_count());
    std::printf("  escrito: %s  (ventana [%u, %u])\n",
                wav.c_str(), at, at + frames - 1);

    // -- 5. Qué sonó ----------------------------------------------------------
    //
    // El conteo de eventos es el control de NO VACUIDAD: un WAV de silencio
    // pesa lo mismo que uno con música, y sin esto la herramienta reportaría
    // «escrito» sobre una grabación que no tiene nada. Si esto da 0, la casilla
    // o el id están mal — y decirlo acá ahorra escuchar el archivo para
    // enterarse.
    s.analyze_audio_events(rec);
    const uint32_t n = s.audio_event_count();
    std::printf("  eventos de audio detectados: %u%s\n", n,
                n == 0 ? "  <-- NADA SONO: revisa la casilla o el id" : "");
    return n > 0 ? 0 : 3;
}
