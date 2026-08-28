// ---------------------------------------------------------------------------
// panorama_cover_test.cpp — #565: la cobertura se verifica contra lo que la
// lámina DIBUJA.
//
// EL DEFECTO QUE FIJA. Una posición de la tira puede tener varios hashes —una
// celda animada tiene uno por estado; un barrido que cruzó de zona apila dos
// tramos del nivel en la misma posición— y el índice los guarda todos, porque
// cada estado tiene que poder ANCLAR. Pero el PNG conserva UNO.
//
// Mientras la cobertura aceptaba cualquiera, una tira ambigua matcheaba todo:
// medido en Sonic 3 & Knuckles f2092, «pano=ANCLADA (34/38 votos, cobertura
// 100 %)» con el recorte exportado mostrando Angel Island en una cueva.
//
// El primer caso de este oráculo es el DEL DEFECTO: observar en una posición
// ambigua el hash que la lámina NO dibuja. Antes daba `true` y la posición
// contaba como cubierta; ahora no. Sin ese caso, un test que sólo comprueba que
// el hash bueno matchea pasa en verde con la implementación vieja.
//
// Sin ROM y sin GPU: la regla es del FORMATO de la tira, y por eso vive en
// `panorama_cover.h` en vez de adentro de la sesión. El caso real —que Golden
// Axe siga extendiendo bien y que Sonic 3 & K deje de mentir— se mide con
// `tools/widescreen_shot` sobre las tomas, que necesitan ROM.
// ---------------------------------------------------------------------------
#include "panorama_cover.h"

#include <cstdio>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

// Variantes por repaletado, de mentira pero con la MISMA forma que las reales:
// `out[0]` es el hash tal cual y las otras tres son lecturas bajo las demás
// líneas. Inyectarlas evita arrastrar el core a un test de una regla pura, y
// hace visible qué parte de la decisión es de esta función.
void fake_variants(uint64_t h, uint8_t pal, uint64_t out[4]) {
    out[0] = h;
    for (int i = 1; i < 4; ++i) out[i] = h ^ (0xA5A5ull * (uint64_t)(i + pal));
}

constexpr uint64_t kRenderedHash = 0x1111'1111'1111'1111ull;
constexpr uint64_t kOtherStateHash = 0x2222'2222'2222'2222ull;
constexpr uint64_t kUnrelatedHash = 0x3333'3333'3333'3333ull;

}  // namespace

int main() {
    using ayther::panorama_pos_matches;

    // -- EL CASO DEL DEFECTO ------------------------------------------------
    //
    // La posición es ambigua: la tira tiene dos hashes ahí. El PNG dibuja el
    // primero. Observar el SEGUNDO no puede contar como «la lámina explica esta
    // celda» — es exactamente lo que hacía que un anclaje se declarara con
    // cobertura 100 % contra un tramo del nivel que no se está mostrando.
    {
        const std::vector<uint64_t> ambiguous = { kRenderedHash, kOtherStateHash };
        check(!panorama_pos_matches(ambiguous, kOtherStateHash, 0, fake_variants),
              "el hash que la lamina NO dibuja no cubre la celda (el defecto)");
        check(panorama_pos_matches(ambiguous, kRenderedHash, 0, fake_variants),
              "y el que SI dibuja, si");
    }

    // -- Que no se rompió lo que andaba ------------------------------------
    {
        const std::vector<uint64_t> clean_strip = { kRenderedHash };
        check(panorama_pos_matches(clean_strip, kRenderedHash, 0, fake_variants),
              "una posicion limpia sigue cubriendo");
        check(!panorama_pos_matches(clean_strip, kUnrelatedHash, 0, fake_variants),
              "y una celda ajena sigue sin cubrir");
    }

    // -- Repaletado (#420) --------------------------------------------------
    //
    // El mismo dibujo bajo otra línea CRAM produce otro hash, y tiene que
    // seguir cubriendo: sin esto, un ciclo de día/noche despega la tira sola.
    // Pero SÓLO contra el dibujado — una variante del otro estado tampoco vale.
    {
        uint64_t var[4];
        fake_variants(kRenderedHash, 2, var);
        const std::vector<uint64_t> strip = { kRenderedHash, kOtherStateHash };
        check(panorama_pos_matches(strip, var[2], 2, fake_variants),
              "el dibujado repaletado sigue cubriendo (#420)");

        uint64_t other_variants[4];
        fake_variants(kOtherStateHash, 2, other_variants);
        check(!panorama_pos_matches(strip, other_variants[2], 2, fake_variants),
              "pero una variante del estado que NO se dibuja, no");
    }

    // -- «No sé» no es «sí» -------------------------------------------------
    //
    // Una posición sin nada en la tira no cubre. Devolver true ahí sería
    // afirmar que la lámina explica una celda sobre la que no tiene dato — el
    // mismo error que este issue arregla, en su forma más pura.
    check(!panorama_pos_matches({}, kRenderedHash, 0, fake_variants),
          "una posicion vacia no cubre: «no se» no es «si»");

    // -- NO VACUIDAD --------------------------------------------------------
    //
    // Si las variantes falsas colisionaran con los hashes de prueba, media
    // batería de arriba pasaría por casualidad. Se comprueba que los tres
    // hashes son distintos y que ninguna variante de uno cae sobre otro.
    {
        bool ok = kRenderedHash != kOtherStateHash && kRenderedHash != kUnrelatedHash;
        uint64_t v[4];
        for (uint64_t h : { kRenderedHash, kOtherStateHash, kUnrelatedHash })
            for (uint8_t pal = 0; pal < 4; ++pal) {
                fake_variants(h, pal, v);
                for (int i = 1; i < 4; ++i)
                    for (uint64_t other : { kRenderedHash, kOtherStateHash, kUnrelatedHash })
                        if (other != h && v[i] == other) ok = false;
            }
        check(ok, "las variantes de prueba no colisionan entre si");
    }

    std::printf("\n  %d ok · %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
