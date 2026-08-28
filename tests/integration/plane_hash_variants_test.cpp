// ---------------------------------------------------------------------------
// plane_hash_variants_test — las 4 lecturas de un hash de celda de plano (#420).
//
// El hash de celda mezcla el ÍNDICE de línea CRAM al final del FNV1a del
// patrón. La Panorámica guarda hashes SIN su paleta (`PanoramaCell` es hash +
// posición, y ese es también el formato del pack), así que cuando un juego
// reasigna la celda a otra línea —los ciclos de día/noche que REPINTAN— el hash
// observado deja de coincidir y la tira se despega sola. El arreglo es
// preguntar por las otras lecturas del hash con
// `ayther_plane_tile_hash_variants`.
//
// El test construye los hashes con la FÓRMULA DEL MOTOR documentada en el
// header (`h = (h_patrón ^ pal) * PRIME`, ver `collect_plane_tiles`) y no con la
// función que está probando: si la convención del hash cambia y el re-paletado
// no la sigue, esto se cae — que es justo lo que un oráculo que fabrica sus
// datos con la convención que testea no puede detectar.
//
// Puro: sin ROM, sin core, sin GPU.
// ---------------------------------------------------------------------------
#include "ayther_session.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

/// El hash tal como lo arma el motor: FNV1a del patrón con la línea mezclada
/// al final. `pattern_h` hace de FNV1a ya calculado.
constexpr uint64_t kPrime = 1099511628211ULL;
uint64_t cell_hash(uint64_t pattern_h, uint8_t pal) {
    return (pattern_h ^ static_cast<uint64_t>(pal & 3u)) * kPrime;
}

/// Unos cuantos "patrones" con bits repartidos (incluye los extremos, que son
/// los que suelen romper la aritmética modular).
const std::vector<uint64_t> kPatterns = {
    0x0000000000000000ull, 0x0000000000000001ull, 0xFFFFFFFFFFFFFFFFull,
    0x0123456789ABCDEFull, 0xDEADBEEFCAFEBABEull, 0x8000000000000000ull,
    0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull, 0x00000000FFFFFFFFull,
};

/// FNV1a de verdad, para generar patrones REALISTAS: en el motor el patrón es
/// el FNV1a de los 32 bytes del tile, no un número elegido a mano.
uint64_t fnv1a(const unsigned char* p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= kPrime; }
    return h;
}

/// 64 tiles distintos, hasheados como los hashea el motor.
std::vector<uint64_t> real_patterns() {
    std::vector<uint64_t> v;
    for (int t = 0; t < 64; ++t) {
        unsigned char tile[32];
        for (int i = 0; i < 32; ++i)
            tile[i] = static_cast<unsigned char>((t * 37 + i * 11) ^ (t << 3));
        v.push_back(fnv1a(tile, sizeof(tile)));
    }
    return v;
}

}  // namespace

int main() {
    std::printf("=== plane_hash_variants (#420) ===\n\n");

    // 1. COBERTURA: desde la celda vista bajo la línea P, las variantes tienen
    //    que traer la MISMA celda tal como se hashearía bajo cualquier otra
    //    línea. Es la propiedad de la que depende el anclaje.
    {
        bool ok = true;
        for (uint64_t pat : kPatterns)
            for (uint8_t p = 0; p < 4 && ok; ++p) {
                uint64_t var[4];
                ayther::ayther_plane_tile_hash_variants(cell_hash(pat, p), p, var);
                for (uint8_t q = 0; q < 4; ++q) {
                    const uint64_t want = cell_hash(pat, q);
                    bool found = false;
                    for (uint64_t v : var) if (v == want) { found = true; break; }
                    if (!found) {
                        std::printf("      patrón %016llx: falta la lectura de "
                                    "la línea %u vista desde la %u\n",
                                    (unsigned long long)pat, q, p);
                        ok = false; break;
                    }
                }
            }
        check(ok, "las 4 variantes cubren las 4 líneas de CRAM");
    }

    // 2. El camino DIRECTO va primero: quien busca prueba out[0] y, sin
    //    repaletado, acierta sin calcular nada más.
    {
        bool ok = true;
        for (uint64_t pat : kPatterns)
            for (uint8_t p = 0; p < 4; ++p) {
                const uint64_t h = cell_hash(pat, p);
                uint64_t var[4];
                ayther::ayther_plane_tile_hash_variants(h, p, var);
                if (var[0] != h) ok = false;
            }
        check(ok, "out[0] es el hash tal cual (camino directo primero)");
    }

    // 3. Las 4 son DISTINTAS: si dos líneas colapsaran al mismo hash, una celda
    //    repintada se confundiría con otra cosa en vez de reconocerse.
    {
        bool ok = true;
        for (uint64_t pat : kPatterns)
            for (uint8_t p = 0; p < 4; ++p) {
                uint64_t var[4];
                ayther::ayther_plane_tile_hash_variants(cell_hash(pat, p), p, var);
                std::set<uint64_t> uniq(var, var + 4);
                if (uniq.size() != 4) ok = false;
            }
        check(ok, "las 4 lecturas de una celda son distintas entre sí");
    }

    // 4. SIN COLISIÓN entre tiles REALES: tolerar el repaletado no puede
    //    volverse "matchea con cualquier cosa" — eso anclaría la tira sobre un
    //    HUD o un primer plano.
    {
        std::set<uint64_t> seen;
        bool ok = true;
        for (uint64_t pat : real_patterns()) {
            uint64_t var[4];
            ayther::ayther_plane_tile_hash_variants(cell_hash(pat, 0), 0, var);
            for (uint64_t v : var)
                if (!seen.insert(v).second) ok = false;
        }
        check(ok, "64 tiles distintos no comparten ninguna lectura");
    }

    // 5. LÍMITE CONOCIDO, fijado a propósito. La paleta se mezcla con un XOR en
    //    los 2 bits BAJOS, así que `patrón ^ línea` es ambiguo por
    //    construcción: dos patrones que difieran SÓLO en esos 2 bits producen
    //    exactamente el mismo conjunto de lecturas, y el matcheo tolerante no
    //    puede distinguirlos.
    //
    //    No es un defecto del re-paletado sino del formato del hash, y en la
    //    práctica no muerde: el patrón es un FNV1a de 32 bytes, y que dos tiles
    //    distintos caigan a 2 bits uno del otro es del orden de 1 en 2^62. Se
    //    fija acá para que, si algún día alguien mueve la paleta a otros bits o
    //    ensancha el campo, este test diga qué se estaba asumiendo.
    {
        const uint64_t a = 0x0123456789ABCDECull;   // los 2 bits bajos en 00
        bool collide = true;
        for (uint64_t d = 1; d < 4; ++d) {
            uint64_t va[4], vb[4];
            ayther::ayther_plane_tile_hash_variants(cell_hash(a, 0), 0, va);
            ayther::ayther_plane_tile_hash_variants(cell_hash(a ^ d, 0), 0, vb);
            if (std::set<uint64_t>(va, va + 4) != std::set<uint64_t>(vb, vb + 4))
                collide = false;
        }
        check(collide, "ambigüedad conocida: 2 bits bajos = mismo conjunto");
    }

    // 6. SIMETRÍA: mirar la misma celda desde cualquier línea da el mismo
    //    conjunto. Sin esto el matcheo dependería de con qué paleta se la vio
    //    primero.
    {
        bool ok = true;
        for (uint64_t pat : kPatterns) {
            std::set<uint64_t> ref;
            for (uint8_t p = 0; p < 4; ++p) {
                uint64_t var[4];
                ayther::ayther_plane_tile_hash_variants(cell_hash(pat, p), p, var);
                std::set<uint64_t> got(var, var + 4);
                if (p == 0) ref = got;
                else if (got != ref) ok = false;
            }
        }
        check(ok, "el conjunto no depende de la línea desde la que se mira");
    }

    std::printf("\n%s (%d fallo%s)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
