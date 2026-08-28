// ---------------------------------------------------------------------------
// cram_palette_test.cpp — #230 EM-9.4: la CRAM leída, con su formato fijado.
//
// La conversión vivía suelta en tres lugares de `ayther_session.cpp`, sin
// oráculo, y de ella dependen el hash de tile de plano, la firma de variante
// por paleta (#138), el tinte y ahora el visor. Tres copias de una conversión
// sin test es una que se va a arreglar en dos lugares.
//
// LOS DOS ERRORES QUE ESTO FIJA, ambos de los que no se ven:
//
//   · Leer la CRAM como si viniera en el formato del BUS (R=1-3, G=5-7, B=9-11)
//     en vez del EMPAQUETADO que publica el fork (R=0-2, G=3-5, B=6-8). Da
//     colores razonables —todo a media intensidad y con el tono corrido— y
//     nadie lo mira dos veces.
//   · Expandir 3 bits a 8 con `x << 5`. El blanco máximo daría 224 en vez de
//     255 y toda la imagen queda lavada, que también parece «así es el juego».
//
// Puro: sin ROM, sin GPU, sin sesión.
// ---------------------------------------------------------------------------
#include "cram_palette.h"

#include <cstdio>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

/// CRAM con un word por entrada, little-endian, como llega del fork.
std::vector<uint8_t> make_cram(const std::vector<uint16_t>& words) {
    std::vector<uint8_t> v;
    for (uint16_t w : words) {
        v.push_back(static_cast<uint8_t>(w & 0xFF));
        v.push_back(static_cast<uint8_t>(w >> 8));
    }
    v.resize(128, 0);   // 64 colores
    return v;
}

}  // namespace

int main() {
    using namespace ayther;

    // -- Los dos colores medidos contra el juego -----------------------------
    //
    // Blanco = 0x1FF y azul = 0x1E3 → R3 G4 B7. Están en el comentario de
    // `ayther_session.cpp` desde que se verificó contra la ROM; acá dejan de
    // ser un comentario.
    {
        const auto c = make_cram({ 0x1FF, 0x1E3 });
        const CramColor white = cram_color(c.data(), c.size(), 0);
        check(white.r == 255 && white.g == 255 && white.b == 255,
              "0x1FF es blanco PURO (y no 224: la expansion no es x<<5)");

        const CramColor blue = cram_color(c.data(), c.size(), 1);
        // 0x1E3 = 0b1_1110_0011 → R=3, G=4, B=7
        check(blue.r == cram_c8(3) && blue.g == cram_c8(4) && blue.b == cram_c8(7),
              "0x1E3 da R3 G4 B7 (formato EMPAQUETADO, no el del bus)");
        // Y el azul domina, que es lo que se ve en pantalla: si estuviera
        // leyendo el formato del bus, este color saldria casi gris.
        check(blue.b > blue.g && blue.g > blue.r, "y el azul domina");
    }

    // -- La expansión de 3 bits ----------------------------------------------
    {
        check(cram_c8(0) == 0,   "0 -> 0");
        check(cram_c8(7) == 255, "7 -> 255 (el maximo LLEGA al maximo)");
        // Monotona y sin saltos raros: cada escalon sube parejo.
        bool mono = true;
        for (int i = 1; i < 8; ++i)
            if (cram_c8(static_cast<uint8_t>(i)) <= cram_c8(static_cast<uint8_t>(i - 1)))
                mono = false;
        check(mono, "monotona creciente");
        // El error clasico, para que quede fijado por contraste.
        check(cram_c8(7) != static_cast<uint8_t>(7 << 5),
              "y NO es x<<5, que daria 224 y dejaria la imagen lavada");
    }

    // -- Línea y entrada ------------------------------------------------------
    {
        std::vector<uint16_t> w(64, 0);
        w[1 * 16 + 3] = 0x1FF;   // línea 1, entrada 3
        const auto c = make_cram(w);
        const CramColor x = cram_color_at(c.data(), c.size(), 1, 3);
        check(x.r == 255 && x.g == 255 && x.b == 255, "linea 1 entrada 3");
        const CramColor y = cram_color_at(c.data(), c.size(), 0, 3);
        check(y.r == 0 && y.g == 0 && y.b == 0, "y la linea 0 no se contagia");
    }

    // -- Los bordes no revientan ---------------------------------------------
    //
    // Negro para «no sé» y no magenta: un color de fondo es un resultado
    // legítimo, y magenta convertiría cada lectura de más en un falso positivo
    // visual.
    {
        const auto c = make_cram({ 0x1FF });
        const CramColor a = cram_color(nullptr, 0, 0);
        const CramColor b = cram_color(c.data(), c.size(), 64);
        const CramColor d = cram_color(c.data(), 1, 0);   // buffer cortado
        check(a.r == 0 && b.r == 0 && d.r == 0, "sin datos / fuera de rango / cortado = negro");
    }

    // -- La firma de línea (#138) --------------------------------------------
    //
    // Es lo que un visor puede decir y una comparación visual no: si dos
    // momentos del juego tienen la MISMA paleta.
    {
        std::vector<uint16_t> w(64, 0);
        w[16] = 0x1FF;
        const auto c1 = make_cram(w);
        w[16] = 0x1FE;              // un bit de diferencia
        const auto c2 = make_cram(w);

        check(cram_line_signature(c1.data(), c1.size(), 1)
              != cram_line_signature(c2.data(), c2.size(), 1),
              "un bit de la linea cambia su firma");
        check(cram_line_signature(c1.data(), c1.size(), 1)
              == cram_line_signature(c1.data(), c1.size(), 1),
              "y es estable");
        check(cram_line_signature(c1.data(), c1.size(), 0)
              == cram_line_signature(c2.data(), c2.size(), 0),
              "un cambio en la linea 1 NO mueve la firma de la 0");
        // Y las cuatro lineas de una CRAM en cero NO son iguales por casualidad
        // ... si lo fueran, el test de arriba no probaria nada: son iguales, y
        // ESO es lo correcto — misma entrada, misma firma.
        check(cram_line_signature(c1.data(), c1.size(), 0)
              == cram_line_signature(c1.data(), c1.size(), 2),
              "dos lineas identicas dan la MISMA firma (es una firma de contenido)");
    }

    // -- NO VACUIDAD ----------------------------------------------------------
    //
    // Si el fixture fuera todo ceros, media bateria pasaria con una funcion que
    // devuelve negro siempre.
    {
        const auto c = make_cram({ 0x1FF, 0x1E3, 0x038 });
        const CramColor a = cram_color(c.data(), c.size(), 0);
        const CramColor b = cram_color(c.data(), c.size(), 2);
        check(!(a.r == b.r && a.g == b.g && a.b == b.b),
              "dos entradas distintas dan colores distintos");
    }

    std::printf("\n  %d ok · %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
