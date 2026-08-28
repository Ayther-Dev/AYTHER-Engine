#pragma once
// ---------------------------------------------------------------------------
// parallax_bands.h — la columna de nivel POR BANDA ( EM-8.0).
//
// El plano B lleva parallax por bandas: cada entrada de la tabla Hscroll tiene
// su propio desplazamiento, así que «columna de nivel» **no es una sola cosa en
// ese plano** — depende de la fila. Con una cámara única por plano, todas las
// bandas colapsan en las mismas columnas y se apilan unas sobre otras.
//
// MEDIDO en Sonic 2 (`background_spike`, 1200 frames): el plano A reconstruía 607
// columnas de nivel y el B sólo **37** —menos de una pantalla— con 45 bandas por
// frame. No era que faltara arte: estaba todo apilado en el lugar equivocado.
//
// DOS COSAS QUE ESTE ARCHIVO APRENDIÓ A LOS GOLPES
//
// 1. La regla vive acá y no adentro del bucle de `ayther_session.cpp`. La
//    primera versión quedó enterrada ahí, donde el oráculo del stitcher no la
//    veía —llama al stitcher directamente—, así que al medirla no movió un solo
//    número. No estaba mal: no se estaba ejecutando.
//
// 2. No alcanza con restar los H de dos bandas dentro del mismo frame. El campo
//    del VDP es de 10 bits y envuelve, y la separación entre bandas CRECE sin
//    límite a lo largo de un nivel (medido: 17 px contra 566 px en 1033 px de
//    scroll). Cada banda necesita su propio des-enrollado, igual que la cámara
//    del plano. Por eso `BandCameras` tiene estado: una resta pura no puede
//    saber cuántas vueltas dio cada banda.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayther {

/// Cómo está organizada la tabla Hscroll (registro $0B, bits 0-1).
///
///   0 = uno para todo el plano · 1 = por celda dentro del primer tile ·
///   2 = por celda (cada 8 líneas) · 3 = por línea
inline uint32_t hscroll_mask(uint8_t reg0b) {
    static const uint32_t kMask[4] = { 0x00, 0x07, 0xF8, 0xFF };
    return kMask[reg0b & 3];
}

/// Base de la tabla Hscroll en VRAM (registro $0D).
inline uint32_t hscroll_base(uint8_t reg0d) {
    return (static_cast<uint32_t>(reg0d) << 10) & 0xFC00u;
}

/// Lee el H de una LÍNEA de pantalla para un plano. `read_u32` entrega la
/// palabra larga de VRAM ya des-swapeada.
///
/// El campo del VDP es de 10 bits: el juego puede escribir por encima y el resto
/// se ignora, así que se enmascara acá y no en el llamador — hacerlo afuera es
/// como se cuela un valor de 16 bits en un cálculo de 10.
template <typename ReadU32>
inline int hscroll_of_line(const ReadU32& read_u32, uint32_t base, uint32_t mask,
                           uint8_t plane, int line) {
    const uint32_t idx = static_cast<uint32_t>(line < 0 ? 0 : line) & mask;
    const uint32_t hw  = read_u32(base + (idx << 2));
    return plane == 0 ? static_cast<int>(hw & 0x3FF)
                      : static_cast<int>((hw >> 16) & 0x3FF);
}

/// El resto SIEMPRE positivo. `-1 % 512` es -1 en C++, y una columna negativa
/// acá significaría leer la nametable por afuera.
inline int wrap_px(int v, int w) {
    if (w <= 0) return 0;
    const int r = v % w;
    return r < 0 ? r + w : r;
}

/// Cuántas BANDAS distintas hay en este plano, mirando las `rows` filas que se
/// dibujan. Es la medida que dice si vale la pena separar: 1 significa que el
/// plano no tiene parallax por bandas.
template <typename ReadU32>
inline int band_count(const ReadU32& read_u32, uint32_t base, uint32_t mask,
                      uint8_t plane, int rows) {
    if (mask == 0 || rows <= 0) return 1;
    int distinct = 1;
    int last = hscroll_of_line(read_u32, base, mask, plane, 0);
    for (int r = 1; r < rows; ++r) {
        const int h = hscroll_of_line(read_u32, base, mask, plane, r * 8);
        if (h != last) { ++distinct; last = h; }
    }
    return distinct;
}

// ---------------------------------------------------------------------------
/// La cámara ABSOLUTA de cada banda de un plano, des-enrollada frame a frame.
///
/// El VDP sólo dice dónde está cada banda DENTRO del plano (0..ancho-1). Para
/// reconstruir una lámina de nivel hace falta la posición absoluta, y eso pide
/// memoria: la vuelta que dio la banda entre dos frames se deduce de que el
/// salto sea chico. El criterio es el de siempre —un salto de más de medio
/// plano se lee como una vuelta— y por eso hay que alimentarlo TODOS los
/// frames: saltear frames convierte un scroll rápido en una vuelta inventada.
class BandCameras {
public:
    /// `rows` = filas de celdas en pantalla · `width_px` = ancho del plano.
    /// Reconfigurar (otro nivel, otro tamaño de plano) reinicia el arranque:
    /// las bandas vuelven a sembrarse en el próximo frame.
    void configure(int rows, int width_px) {
        if (rows == rows_ && width_px == width_) return;
        rows_ = rows > 0 ? rows : 0;
        width_ = width_px > 0 ? width_px : 512;
        st_.assign(static_cast<size_t>(rows_), State{});
    }

    void reset() { st_.assign(st_.size(), State{}); }

    int rows() const { return rows_; }

    /// Consume un frame: lee la tabla Hscroll y actualiza las `rows` bandas.
    ///
    /// El H del VDP es cuánto se corrió el plano HACIA la derecha, así que la
    /// cámara es su negativo — el mismo signo que usa el resto del motor.
    template <typename ReadU32>
    void observe(const ReadU32& read_u32, uint32_t base, uint32_t mask,
                 uint8_t plane) {
        if (rows_ <= 0) return;
        // La banda de arriba es el origen, y las demás se SIEMBRAN respecto de
        // ella. Sembrar cada una en su propio valor envuelto pondría a una banda
        // 64 px atrás en la columna +56 en vez de en la -8: el VDP dice dónde
        // está dentro del plano, no de qué lado.
        const int h0 = hscroll_of_line(read_u32, base, mask, plane, 0);
        const int w0 = wrap_px(-h0, width_);
        push(0, w0, w0, 0);
        const int64_t ref = st_[0].abs;
        for (int r = 1; r < rows_; ++r) {
            const int h = hscroll_of_line(read_u32, base, mask, plane, r * 8);
            push(r, wrap_px(-h, width_), w0, ref);
        }
    }

    /// La columna de nivel de la banda que le toca a la fila `row`.
    /// Fuera de rango devuelve 0, que es la cámara única de antes: la
    /// degradación es «como estaba», nunca un salto.
    int column(int row) const {
        if (row < 0 || row >= rows_) return 0;
        const int64_t a = st_[static_cast<size_t>(row)].abs;
        return static_cast<int>(a >= 0 ? a / 8 : -((-a + 7) / 8));
    }

    int64_t absolute_px(int row) const {
        if (row < 0 || row >= rows_) return 0;
        return st_[static_cast<size_t>(row)].abs;
    }

    /// Cuánto separa a esta banda de la banda 0, en columnas. Es lo que se le
    /// suma a una cámara de plano ya des-enrollada por afuera.
    int offset_from_top(int row) const { return column(row) - column(0); }

private:
    struct State { bool seeded = false; int last = 0; int64_t abs = 0; };

    /// El representante más cercano a cero. Medio plano: por debajo es scroll,
    /// por encima es que dio la vuelta.
    ///
    /// Es una APUESTA, y la única que se puede hacer mirando un valor envuelto:
    /// dos bandas separadas por más de medio plano en el mismo frame son
    /// indistinguibles de dos separadas por lo que sobra del otro lado. Al
    /// arrancar un nivel las bandas están todas juntas, así que la apuesta se
    /// hace cuando es segura y después sólo se acumula.
    int shortest(int d) const {
        if (d >  width_ / 2) d -= width_;
        if (d < -width_ / 2) d += width_;
        return d;
    }

    void push(int row, int wrapped, int ref_wrapped, int64_t ref_abs) {
        State& s = st_[static_cast<size_t>(row)];
        if (!s.seeded) {
            s.seeded = true;
            s.last   = wrapped;
            s.abs    = ref_abs + shortest(wrapped - ref_wrapped);
            return;
        }
        s.abs += shortest(wrapped - s.last);
        s.last = wrapped;
    }

    std::vector<State> st_;
    int rows_  = 0;
    int width_ = 512;
};

}  // namespace ayther
