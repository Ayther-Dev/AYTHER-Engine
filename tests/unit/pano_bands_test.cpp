// ---------------------------------------------------------------------------
// pano_bands_test — el voto de la Panorámica POR BANDA (#421).
//
// Lo que se fija acá es que el voto por banda (a) no cambia nada cuando el
// plano se desplaza entero —el 100 % del corpus medido salvo Sonic 3 & K— y
// (b) separa de verdad dos scrolls que hoy se pisan.
//
// Sin ROM, sin core, sin GPU: el voto es una función pura.
// ---------------------------------------------------------------------------
#include "pano_bands.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

using namespace ayther;

}  // namespace

int main() {
    std::printf("=== pano_bands_test ===\n");

    // -- 1. Sin bandas: EXACTAMENTE el modelo de hoy ------------------------
    // Es la garantía que hace seguro el cambio: los 40.854 frames de Golden
    // Axe medidos no tienen una sola banda, así que tienen que comportarse
    // igual que antes.
    {
        std::vector<PanoVote> v = {
            {  10, 100, 0 }, {  50, 100, 0 }, { 100, 100, 0 }, { 150, 64, 0 },
        };
        const auto r = pano_vote_by_band(v.data(), v.size(), nullptr, 0, 224);
        check(r.size() == 1, "sin cortes: una sola banda que cubre la pantalla");
        check(r[0].cam_x == 100 && r[0].votes == 3 && r[0].total == 4,
              "gana la MODA, como el voto de siempre");
        check(r[0].confidence() > 0.7f && r[0].confidence() < 0.8f,
              "la confianza refleja 3 de 4");
    }

    // -- 2. Dos bandas con scrolls distintos --------------------------------
    // El caso de #421: arriba el cielo va lento, abajo el suelo va rápido. Con
    // una sola cámara una de las dos queda mal ubicada; con bandas, cada una
    // saca la suya.
    {
        const int32_t bands[] = { 0, 112, 224 };
        std::vector<PanoVote> v = {
            {  10, 40, 0 }, {  30, 40, 0 }, {  90, 40, 0 },   // banda alta: 40
            { 120, 400, 0 }, { 180, 400, 0 }, { 200, 400, 0 },// banda baja: 400
        };
        const auto r = pano_vote_by_band(v.data(), v.size(), bands, 3, 224);
        check(r.size() == 2, "dos cortes ⇒ dos bandas");
        check(r[0].cam_x == 40 && r[0].votes == 3, "la banda alta fija su cámara");
        check(r[1].cam_x == 400 && r[1].votes == 3, "la banda baja fija la suya");
        check(r[0].confidence() == 1.0f && r[1].confidence() == 1.0f,
              "sin mezcla, las dos deciden con confianza total");
    }

    // -- 3. El defecto que esto arregla, demostrado -------------------------
    // Los MISMOS votos, sin bandas: gana uno de los dos scrolls y el otro
    // queda mal. Que el test lo afirme deja constancia de por qué existe la
    // feature — no es una preferencia de modelado.
    {
        std::vector<PanoVote> v = {
            {  10, 40, 0 }, {  30, 40, 0 }, {  90, 40, 0 },
            { 120, 400, 0 }, { 180, 400, 0 }, { 200, 400, 0 },
        };
        const auto r = pano_vote_by_band(v.data(), v.size(), nullptr, 0, 224);
        check(r.size() == 1 && r[0].total == 6 && r[0].votes == 3,
              "sin bandas: 6 votos partidos 3-3, la mitad del plano queda mal");
        check(r[0].confidence() == 0.5f,
              "confianza 0,5 = la señal de que hay dos scrolls mezclados");
    }

    // -- 4. Una banda sin votos NO tiene cámara -----------------------------
    // `total == 0` es distinto de «la cámara es (0,0)»: confundirlos dibuja la
    // tira en el origen, que es un defecto visible y difícil de atribuir.
    {
        const int32_t bands[] = { 0, 112, 224 };
        std::vector<PanoVote> v = { { 10, 40, 0 }, { 30, 40, 0 } };
        const auto r = pano_vote_by_band(v.data(), v.size(), bands, 3, 224);
        check(r[0].decided() && !r[1].decided(),
              "la banda sin votos queda SIN decidir, no en (0,0)");
    }

    // -- 5. Desempate determinista ------------------------------------------
    // Con `>` a secas el ganador de un empate depende del orden de iteración
    // de un hash y dos corridas idénticas difieren. Acá gana el cam_x menor.
    {
        std::vector<PanoVote> v = { { 10, 500, 0 }, { 20, 100, 0 } };
        const auto a = pano_vote_by_band(v.data(), v.size(), nullptr, 0, 224);
        std::vector<PanoVote> w = { { 10, 100, 0 }, { 20, 500, 0 } };
        const auto b = pano_vote_by_band(w.data(), w.size(), nullptr, 0, 224);
        check(a[0].cam_x == 100 && b[0].cam_x == 100,
              "empate: gana el cam_x menor, sin importar el orden de entrada");
    }

    // -- 6. Un voto fuera de toda banda se descarta -------------------------
    {
        const int32_t bands[] = { 0, 100 };
        std::vector<PanoVote> v = { { 50, 7, 0 }, { 150, 999, 0 } };
        const auto r = pano_vote_by_band(v.data(), v.size(), bands, 2, 224);
        check(r.size() == 1 && r[0].total == 1 && r[0].cam_x == 7,
              "fuera de banda no vota (y no arrastra la cámara)");
    }

    // -- 7. Sin votos: las bandas existen pero ninguna decide ---------------
    {
        const int32_t bands[] = { 0, 112, 224 };
        const auto r = pano_vote_by_band(nullptr, 0, bands, 3, 224);
        check(r.size() == 2 && !r[0].decided() && !r[1].decided(),
              "sin votos: las bandas están, sin cámara y sin crash");
    }

    // -- 8. Los CORTES salen de la tabla Hscroll ---------------------------
    // El lector es el mismo de EM-8.0 (hscroll_of_line). Acá se le da una
    // tabla sintetica: sin ROM y sin VDP.
    {
        // Tabla por linea: 28 filas. Las primeras 14 con H=100, el resto H=400.
        auto read_u32 = [](uint32_t addr) -> uint32_t {
            const uint32_t line = (addr - 0x1000) >> 2;
            const uint32_t h = line < 112 ? 100u : 400u;   // el corte cae en y=112
            return (h << 16) | h;                          // mismo valor en A y B
        };
        const auto e = pano_band_edges(read_u32, 0x1000, 0xFF, 1, 28);
        check(e.size() == 3 && e[0] == 0 && e[1] == 112 && e[2] == 224,
              "dos bandas: el corte cae donde cambia el H");

        // mask 0 = reg $0B modo 0: scroll entero, una sola banda, sin leer.
        const auto one = pano_band_edges(read_u32, 0x1000, 0, 1, 28);
        check(one.size() == 2 && one[0] == 0 && one[1] == 224,
              "scroll entero ⇒ una banda que cubre la pantalla");

        // Y encadenado con el voto: cada banda saca su camara.
        std::vector<PanoVote> v = {
            { 10, 40, 0 }, { 90, 40, 0 }, { 130, 400, 0 }, { 200, 400, 0 },
        };
        const auto r = pano_vote_by_band(v.data(), v.size(), e.data(), e.size(), 224);
        check(r.size() == 2 && r[0].cam_x == 40 && r[1].cam_x == 400,
              "cortes reales + voto: cada banda con su camara");
    }

    std::printf(g_fail ? "\n=== FAIL (%d) ===\n" : "\n=== OK ===\n", g_fail);
    return g_fail ? 1 : 0;
}
