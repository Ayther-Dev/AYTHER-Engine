// ---------------------------------------------------------------------------
// synth_rom.h — un Mega Drive de juguete, escrito acá.
//
// POR QUÉ EXISTE. El repo tiene dos mundos que no se tocan (#415): los tests que
// no necesitan ROM —y que por eso no ven el core de verdad— y las herramientas
// que sí la necesitan, que se corren a mano cuando alguien se acuerda. El
// 2026-08-13 el audio estuvo DOS DÍAS mudo con el ctest en verde porque el bug
// vivía justo en el medio.
//
// La salida de eso fue fabricar el estímulo: una ROM SINTÉTICA con 68k escrito
// en el propio test. Sin ROM comercial no hay licencia que discutir, el estímulo
// es CONOCIDO —sabemos exactamente qué tiene que pasar— y el oráculo entra al
// ctest de todos los días. `audio_output_smoke` lo estrenó con los chips de
// sonido; este header saca de ahí el ensamblador para que el resto lo use, y le
// agrega lo que hace falta para DIBUJAR.
//
// El 68k que hay acá es el mínimo: mover inmediatos a direcciones fijas, esperar
// al bus del Z80 y saltar. Los dos subsistemas del Mega Drive se manejan así —
// escribiendo bytes y palabras a direcciones fijas.
// ---------------------------------------------------------------------------
#pragma once

#include "ayther_env.h"
#include "ayther_file.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace ayther::synth {

/// Header de Mega Drive + un programa 68k armado a mano.
class Rom {
public:
    explicit Rom(const char* title = "AYTHER SYNTHETIC ROM") : data_(kSize, 0) {
        header(title);
    }

    // -- Ensamblador mínimo ---------------------------------------------------

    /// `move.b #val, (addr).L` — 8 bytes. El inmediato de un byte viaja en una
    /// palabra, que es como lo codifica el 68k.
    void write_byte_to(uint32_t addr, uint8_t val) {
        w16(pc_, 0x13FC);        pc_ += 2;
        w16(pc_, uint16_t(val)); pc_ += 2;
        w32(pc_, addr);          pc_ += 4;
    }
    /// `move.w #val, (addr).L` — 8 bytes.
    void write_word_to(uint32_t addr, uint16_t val) {
        w16(pc_, 0x33FC); pc_ += 2;
        w16(pc_, val);    pc_ += 2;
        w32(pc_, addr);   pc_ += 4;
    }
    /// `move.l #val, (addr).L` — 10 bytes. El puerto de control del VDP toma el
    /// comando de dirección entero en una sola escritura larga.
    void write_long_to(uint32_t addr, uint32_t val) {
        w16(pc_, 0x23FC); pc_ += 2;
        w32(pc_, val);    pc_ += 4;
        w32(pc_, addr);   pc_ += 4;
    }

    /// Espera a que el Z80 CONCEDA el bus: `btst #0,(0xA11100).L` + `bne` al
    /// propio btst. Sin esta espera el 68k sigue de largo y sus escrituras al
    /// YM2612 se pierden en el aire — es lo que dejó a la ROM del audio sonando
    /// sólo el PSG (que cuelga de otro bus) y al FM mudo, con el detector viendo
    /// una nota igual y todo el smoke en verde.
    void wait_bus() {
        const uint32_t at = pc_;
        w16(pc_, 0x0839);   pc_ += 2;   // btst #imm,(xxx).L
        w16(pc_, 0x0000);   pc_ += 2;   // bit 0
        w32(pc_, 0xA11100); pc_ += 4;
        w16(pc_, 0x6600);   pc_ += 2;   // bne.w
        w16(pc_, uint16_t(int16_t(int32_t(at) - int32_t(pc_))));
        pc_ += 2;
    }

    /// `bra *` — se queda acá para siempre, con el estado ya programado.
    void spin() { w16(pc_, 0x60FE); pc_ += 2; }

    /// Una espera de `n` vueltas sin tocar nada: `move.w #n,d0` + `dbra d0,*`.
    /// A ~10 ciclos por vuelta, 0xFFFF son unos cinco frames.
    ///
    /// Existe para que la ROM tenga frames QUIETOS entre los que hacen ruido. Un
    /// estímulo que escribe sin parar deja al core en actividad raster PERMANENTE
    /// y eso rompe dos oráculos por motivos opuestos: el del delta (#405) exige
    /// que el journal se vacíe alguna vez, y el multicapa (#406) exige frames
    /// limpios donde el composite tiene que dar bit-exacto. Los dos tienen razón:
    /// un juego real hace las dos cosas.
    void delay(uint16_t n) {
        w16(pc_, 0x303C); pc_ += 2;   // move.w #imm, d0
        w16(pc_, n);      pc_ += 2;
        const uint32_t at = pc_;
        w16(pc_, 0x51C8); pc_ += 2;   // dbra d0, <disp>
        w16(pc_, uint16_t(int16_t(int32_t(at) - int32_t(pc_)))); pc_ += 2;
    }

    uint32_t here() const { return pc_; }
    /// `bra.s` de vuelta a `target`.
    void loop_to(uint32_t target) {
        const int32_t d = int32_t(target) - int32_t(pc_ + 2);
        w16(pc_, uint16_t(0x6000 | uint8_t(int8_t(d)))); pc_ += 2;
    }

    // -- Sonido: YM2612 + SN76489 ---------------------------------------------

    /// Registro del YM2612. `bank` 1 = puerto 2/3 (canales 4-6).
    void fm(uint8_t reg, uint8_t val, int bank = 0) {
        write_byte_to(bank ? 0xA04002u : 0xA04000u, reg);
        write_byte_to(bank ? 0xA04003u : 0xA04001u, val);
    }
    void psg(uint8_t val) { write_byte_to(0xC00011u, val); }
    /// Pide el bus del Z80 y SACARLO del reset — un Z80 reseteado no lo concede
    /// nunca y el 68k se queda esperando para siempre.
    void take_z80_bus() {
        write_word_to(0xA11100, 0x0100);
        write_word_to(0xA11200, 0x0100);
        wait_bus();
    }

    // -- Video: VDP -----------------------------------------------------------
    // Puerto de datos en 0xC00000 y de control en 0xC00004. Un registro se
    // escribe como palabra con el bit 15 puesto; una dirección de destino, como
    // comando largo. El auto-incremento (reg 15) avanza solo entre escrituras,
    // así que un bloque se vuelca poniendo la dirección UNA vez.

    static constexpr uint32_t kVdpData = 0xC00000u;
    static constexpr uint32_t kVdpCtrl = 0xC00004u;

    void vdp_reg(uint8_t reg, uint8_t val) {
        write_word_to(kVdpCtrl, uint16_t(0x8000u | (uint16_t(reg) << 8) | val));
    }
    /// El comando de dirección: los 14 bits bajos van a la palabra alta y los
    /// dos altos a la baja. `cd` elige el destino (ver las tres de abajo).
    static uint32_t vdp_cmd(uint32_t cd, uint32_t addr) {
        return ((addr & 0x3FFFu) << 16) | ((addr >> 14) & 3u) | cd;
    }
    void vdp_vram_at(uint32_t addr) { write_long_to(kVdpCtrl, vdp_cmd(0x40000000u, addr)); }
    void vdp_cram_at(uint32_t addr) { write_long_to(kVdpCtrl, vdp_cmd(0xC0000000u, addr)); }
    void vdp_vsram_at(uint32_t addr){ write_long_to(kVdpCtrl, vdp_cmd(0x40000010u, addr)); }
    void vdp_data(uint16_t v)       { write_word_to(kVdpData, v); }

    /// Un tile de 8×8 de un solo índice de color: 8 filas de 8 nibbles iguales.
    /// `index` 0 es transparente en planos y sprites (no se dibuja), que es lo
    /// que deja ver el backdrop.
    void vdp_fill_tile(uint32_t tile, uint8_t index) {
        vdp_vram_at(tile * 32u);
        const uint8_t n = uint8_t(index & 0x0F);
        const uint16_t row = uint16_t((n << 12) | (n << 8) | (n << 4) | n);
        for (int i = 0; i < 16; ++i) vdp_data(row);   // 8 filas × 2 palabras
    }

    // -- Salida ---------------------------------------------------------------

    bool save(const std::filesystem::path& p) const {
        std::FILE* f = ayther::file_open(p.string().c_str(), "wb");
        if (!f) return false;
        const size_t n = std::fwrite(data_.data(), 1, data_.size(), f);
        std::fclose(f);
        return n == data_.size();
    }

private:
    static constexpr size_t   kSize  = 0x8000;    // 32 KB: de sobra y carga rápido
    static constexpr uint32_t kEntry = 0x000200;  // el programa arranca tras los vectores

    void w8 (uint32_t at, uint8_t v)  { data_[at] = v; }
    void w16(uint32_t at, uint16_t v) { w8(at, uint8_t(v >> 8)); w8(at + 1, uint8_t(v)); }
    void w32(uint32_t at, uint32_t v) { w16(at, uint16_t(v >> 16)); w16(at + 2, uint16_t(v)); }
    void str(uint32_t at, const char* s, size_t len) {
        for (size_t i = 0; i < len; ++i)
            w8(at + uint32_t(i), s[i] ? uint8_t(s[i]) : ' ');
    }

    void header(const char* title) {
        // Vectores: pila arriba de la RAM y PC al programa. Todos los demás
        // apuntan al mismo sitio — un juego de verdad los usaría, éste no llega
        // a tomar una excepción.
        w32(0x000, 0x00FFFF00);
        w32(0x004, kEntry);
        for (uint32_t v = 0x008; v < 0x100; v += 4) w32(v, kEntry);
        // El «SEGA» de 0x100 es por lo que el core reconoce el hardware; el
        // resto son campos de texto de largo fijo.
        str(0x100, "SEGA MEGA DRIVE ", 16);
        str(0x110, "(C)AYTHER 2026  ", 16);
        str(0x120, title, 48);
        str(0x150, title, 48);
        str(0x180, "GM 00000000-00", 14);
        w16(0x18E, 0x0000);                 // checksum (el core no lo exige)
        str(0x190, "J               ", 16); // dispositivos de entrada
        w32(0x1A0, 0x00000000);             // ROM start
        w32(0x1A4, uint32_t(kSize - 1));    // ROM end
        w32(0x1A8, 0x00FF0000);             // RAM start
        w32(0x1AC, 0x00FFFFFF);             // RAM end
        str(0x1F0, "JUE             ", 16); // regiones
        pc_ = kEntry;
    }

    std::vector<uint8_t> data_;
    uint32_t             pc_ = kEntry;
};

// ---------------------------------------------------------------------------
// LA ROM CANÓNICA
//
// Un solo estímulo para todo el repo, para que los oráculos que hoy piden una
// ROM comercial —y que por eso NO se registran en el ctest de una checkout
// limpia, CI incluido: `roms/` está en .gitignore— tengan uno commiteable.
//
// Dibuja lo mínimo que hace falta para que las dos mitades del motor tengan qué
// mirar, y se MUEVE, que es lo que separa un oráculo con sentido de un verde
// vacío: sin cambios entre frames, los checks de no-vacuidad del delta (#405) se
// caen con razón.
// ---------------------------------------------------------------------------

/// Dónde cae lo que dibuja `program_canonical`, en píxeles de pantalla. La celda
/// del plano A ocupa 32..39 en los dos ejes; el sprite arranca en (100, 64).
inline constexpr int kCellX = 36,  kCellY = 36;   ///< centro de la celda (roja)
inline constexpr int kSprX  = 103, kSprY  = 67;   ///< centro del sprite (verde)
inline constexpr int kBgX   = 200, kBgY   = 200;  ///< backdrop, lejos de los dos

/// El programa canónico. Con `draw` false hace TODO menos poner el tile en la
/// nametable y el sprite en la SAT: mismo VDP, misma paleta, mismos tiles en
/// VRAM, misma animación. Es el control de los oráculos de imagen, y es estrecho
/// a propósito — lo único que cambia es el dibujo.
inline void program_canonical(Rom& r, bool draw = true) {
    // Modo 5, display encendido, H40 (320×224) y las tablas donde vamos a
    // escribir. El backdrop es el color 0 de la paleta 0 (reg 7 = 0).
    r.vdp_reg(0x00, 0x04);   // modo 5, sin HINT
    r.vdp_reg(0x01, 0x44);   // display ON + modo 5
    r.vdp_reg(0x02, 0x30);   // plano A  @ 0xC000
    r.vdp_reg(0x03, 0x00);   // window   (regs 17/18 en 0 = no se muestra)
    r.vdp_reg(0x04, 0x07);   // plano B  @ 0xE000
    r.vdp_reg(0x05, 0x6C);   // SAT      @ 0xD800
    r.vdp_reg(0x07, 0x00);   // backdrop = paleta 0, color 0
    r.vdp_reg(0x0A, 0xFF);   // HINT lejos
    r.vdp_reg(0x0B, 0x00);   // scroll de pantalla entera
    r.vdp_reg(0x0C, 0x81);   // H40
    r.vdp_reg(0x0D, 0x3C);   // hscroll  @ 0xF000 (queda en cero: sin desplazar)
    r.vdp_reg(0x0F, 0x02);   // auto-incremento de 2 bytes
    r.vdp_reg(0x10, 0x01);   // planos de 64×32 celdas
    r.vdp_reg(0x11, 0x00);
    r.vdp_reg(0x12, 0x00);

    // La paleta 0. El backdrop NO es negro a propósito: con negro, «el core
    // dibujó el fondo» y «el framebuffer está en cero» miden igual.
    r.vdp_cram_at(0x0000);
    r.vdp_data(0x0400);   // 0: azul oscuro — el backdrop
    r.vdp_data(0x000E);   // 1: rojo
    r.vdp_data(0x00E0);   // 2: verde
    r.vdp_data(0x0E00);   // 3: azul

    r.vdp_fill_tile(1, 1);   // el tile del plano  → rojo
    r.vdp_fill_tile(2, 2);   // el tile del sprite → verde

    if (draw) {
        // Una celda del plano A: fila 4, columna 4, tile 1, paleta 0, sin
        // prioridad.
        r.vdp_vram_at(0xC000u + (4u * 64u + 4u) * 2u);
        r.vdp_data(0x0001);

        // Un sprite de 1×1 celda en la SAT. Las coordenadas del VDP llevan un
        // corrimiento de 128 en los dos ejes, y `link` 0 cierra la cadena.
        r.vdp_vram_at(0xD800u);
        r.vdp_data(uint16_t(128 + kSprY));   // Y
        r.vdp_data(0x0000);                  // tamaño 1×1 · link 0
        r.vdp_data(0x0002);                  // tile 2, paleta 0, sin prioridad
        r.vdp_data(uint16_t(128 + kSprX - 3));   // X (el muestreo cae adentro)
    }

    // El MOVIMIENTO. Toca dos cosas que el motor observa por caminos distintos y
    // que quieta no puede observar: la CRAM en pleno display (eventos raster) y
    // un patrón de VRAM (dirty patterns del delta, #405). El tile 3 no está en
    // ninguna nametable y el color 3 no lo usa nada de lo dibujado, así que la
    // imagen medida no se mueve: se mueve el ESTADO.
    //
    // A RÁFAGAS, no continuo: la espera deja unos cuatro o cinco frames quietos
    // por cada uno con ruido. Es la diferencia entre un estímulo y un juego —
    // escribiendo sin parar, el core queda en actividad raster permanente y no
    // hay un solo frame limpio que comparar (ver Rom::delay).
    // Y las dos ráfagas dejan valores DISTINTOS con una espera en el medio, que
    // es lo que hace que el cambio se vea entre frames y no sólo dentro de uno:
    // escribiendo A y B en el mismo frame, el estado al cierre es siempre B y la
    // VRAM comparada frame contra frame no se movió nunca — el core marcaba el
    // pattern como sucio y el oráculo del delta no tenía con qué contrastarlo.
    const uint32_t loop = r.here();
    r.vdp_cram_at(0x0006);            // color 3 de la paleta 0
    r.vdp_data(0x0E00);
    r.vdp_vram_at(3u * 32u);          // primera fila del tile 3
    r.vdp_data(0x3333);
    r.delay(0xFFFF);
    r.vdp_cram_at(0x0006);
    r.vdp_data(0x0EE0);
    r.vdp_vram_at(3u * 32u);
    r.vdp_data(0x4444);
    r.delay(0xFFFF);
    r.loop_to(loop);
}

/// Escribe la ROM canónica en el directorio temporal y devuelve su ruta. Se
/// re-escribe en cada corrida: son 32 KB y evita que una ROM vieja de otra
/// versión del programa sobreviva a un cambio del estímulo.
inline std::string canonical_rom_path() {
    const auto p = std::filesystem::temp_directory_path() / "ayther_synthetic.md";
    Rom r("AYTHER SYNTHETIC ROM");
    program_canonical(r, true);
    if (!r.save(p)) return {};
    return p.string();
}

/// La ROM con la que corre un probe: la que diga `AYTHER_PROBE_ROM` si está —un
/// juego de verdad mide más, y así se sigue pudiendo apuntar a uno— y la
/// sintética si no. Sin esto, los tests que la piden simplemente no existen en
/// una máquina sin ROMs, que es exactamente lo que pasa en CI.
inline std::string probe_rom_path() {
    if (const char* e = ayther::env_get("AYTHER_PROBE_ROM"))
        if (*e && std::filesystem::exists(e)) return e;
    return canonical_rom_path();
}

}  // namespace ayther::synth
