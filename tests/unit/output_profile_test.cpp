// ---------------------------------------------------------------------------
// output_profile_test.cpp — #296: perfiles de SALIDA.
//
// Los dos checks que justifican el archivo:
//
//   · **recomendar no es imponer** — la elección del usuario gana siempre sobre
//     la recomendación del pack, y si no ganara, «recomendar» sería una
//     imposición con otro nombre;
//   · **pixel-perfect es entero de verdad** — con un escalado no entero, dos
//     filas de píxeles idénticas salen de distinto grosor y el dibujo se ve
//     tembloroso. Un «pixel-perfect» que en realidad hace fit no se nota en una
//     captura y sí en movimiento.
// ---------------------------------------------------------------------------
#include "output_profile.h"

#include <cstdio>
#include <initializer_list>
#include <cstring>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

}  // namespace

int main() {
    using namespace ayther;
    std::printf("== output_profile_test (#296) ==\n");

    // -- 1. Los del alcance ---------------------------------------------------
    std::printf("\n[1] los perfiles del alcance\n");
    {
        uint32_t n = 0;
        output_profiles(&n);
        // Los cinco de #296 más NTSC (#230 EM-7.2). El número está acá y no
        // derivado del array a propósito: agregar un perfil es una decisión de
        // producto —lo que el jugador ve en la lista— y este test es donde se
        // nota que se tomó.
        check(n == 6, "seis perfiles");
        for (const char* id : { "crt", "lcd", "pixel", "smooth", "cinema", "ntsc" })
            check(output_profile_by_id(id) != nullptr, id);
        // Un id que este build no conoce da null, no el default: un pack de
        // mañana puede recomendar uno nuevo, y confundir «no lo conozco» con
        // «usá el default» haría imposible detectarlo.
        check(output_profile_by_id("holograma") == nullptr,
              "un perfil desconocido da null, no el default");
        check(output_profile_by_id("") == nullptr && output_profile_by_id(nullptr) == nullptr,
              "y vacio o nulo, tambien");
    }

    // -- 2. Recomendar no es imponer -----------------------------------------
    std::printf("\n[2] la eleccion del usuario gana sobre la del pack\n");
    {
        // Quien mira la pantalla es el que sabe si tiene un OLED o un proyector.
        check(std::strcmp(output_profile_resolve("pixel", "crt").id, "pixel") == 0,
              "el usuario eligio: gana el usuario");
        check(std::strcmp(output_profile_resolve("", "crt").id, "crt") == 0,
              "sin eleccion, se usa lo que el pack recomienda");
        check(std::strcmp(output_profile_resolve("", "").id,
                          output_profile_default().id) == 0,
              "sin nada, el default");
        // Un id desconocido de cualquiera de los dos se IGNORA en vez de
        // fallar, y se cae al siguiente en la precedencia.
        check(std::strcmp(output_profile_resolve("holograma", "crt").id, "crt") == 0,
              "una eleccion que no existe cae a la recomendacion");
        check(std::strcmp(output_profile_resolve("", "holograma").id,
                          output_profile_default().id) == 0,
              "y una recomendacion que no existe, al default");
    }

    // -- 3. El default no es CRT ---------------------------------------------
    std::printf("\n[3] el default es LCD, no CRT\n");
    {
        // En una pantalla moderna el CRT es un EFECTO. Arrancar con un efecto
        // puesto haria que el primer vistazo de la remasterizacion fuera el
        // shader y no el arte.
        check(std::strcmp(output_profile_default().id, "lcd") == 0, "arranca en LCD");
        check(output_profile_default().crt_scale == 0.0f, "sin curvatura");
    }

    // -- 4. Pixel-perfect es entero de verdad --------------------------------
    std::printf("\n[4] pixel-perfect escala por multiplo entero\n");
    {
        const OutputProfile& px = *output_profile_by_id("pixel");
        // 320x240 en 1280x720: entero maximo = 3 (960x720).
        const auto r = output_rect(px, 320, 240, 1280, 720);
        check(r.w == 960 && r.h == 720, "×3 exacto");
        check(r.w % 320 == 0 && r.h % 240 == 0, "…multiplo del nativo, sin resto");
        check(r.x == 160 && r.y == 0, "centrado");

        // Y el par: el mismo tamaño con «fit» NO da un múltiplo entero, que es
        // lo que hace que el check de arriba signifique algo.
        const auto f = output_rect(*output_profile_by_id("lcd"), 320, 240, 1000, 700);
        check(f.h % 240 != 0 || f.w % 320 != 0,
              "fit NO garantiza multiplo entero (por eso pixel-perfect existe)");

        // Una ventana en la que no entra ni ×1: recortar seria peor que
        // escalar mal, asi que cae a fit.
        const auto tiny = output_rect(px, 320, 240, 200, 150);
        check(tiny.w <= 200 && tiny.h <= 150 && tiny.w > 0,
              "si no entra ni ×1, cae a fit en vez de recortar");
    }

    // -- 5. Los shaders son multiplicadores, no valores --------------------
    std::printf("\n[5] el perfil ESCALA lo que el pack pide, no lo pisa\n");
    {
        // El autor ya eligio cuanta curvatura queda bien con su arte; el perfil
        // de salida decide cuanto de eso llega a esta pantalla. Un valor
        // absoluto borraria esa decision.
        const OutputProfile& crt = *output_profile_by_id("crt");
        const OutputProfile& lcd = *output_profile_by_id("lcd");
        const float pack_crt = 0.4f;   // lo que el pack pidio
        check(crt.crt_scale * pack_crt > 0.39f && crt.crt_scale * pack_crt < 0.41f,
              "CRT deja pasar lo que el pack pidio, no un valor fijo");
        check(lcd.crt_scale * pack_crt == 0.0f, "LCD lo apaga");
        // «Cinematografica» es una presentacion, no una imitacion de un
        // televisor: viñeta y algo de curvatura, SIN lineas de barrido.
        const OutputProfile& cin = *output_profile_by_id("cinema");
        check(cin.scan_scale == 0.0f && cin.vignette_scale > 0.0f,
              "cinematografica: viñeta si, lineas de barrido no");
        check(cin.crt_scale > 0.0f && cin.crt_scale < crt.crt_scale,
              "…y menos curvatura que el CRT");
    }

    // -- 6. El suavizado --------------------------------------------------
    std::printf("\n[6] el suavizado es del perfil\n");
    {
        check(!output_profile_by_id("pixel")->smoothing,
              "pixel-perfect NO suaviza (seria contradictorio)");
        check(output_profile_by_id("smooth")->smoothing, "suavizado, si");
        check(!output_profile_by_id("lcd")->smoothing,
              "LCD nativo tampoco: «nativo» es sin filtro");
    }

    // -- NTSC: el cable, no el tubo (#230 EM-7.2) -----------------------------
    std::printf("\n[7] NTSC compuesto\n");
    {
        const OutputProfile* ntsc = output_profile_by_id("ntsc");
        const OutputProfile* crt  = output_profile_by_id("crt");
        check(ntsc && ntsc->ntsc > 0.0f, "NTSC trae sangrado de croma");
        // Y es el UNICO: si otro perfil lo trajera, elegir «pixel-perfect» daria
        // una imagen con el color corrido, que es lo contrario de lo que ese
        // perfil promete.
        uint32_t n7 = 0;
        const OutputProfile* all7 = output_profiles(&n7);
        int with_bleed = 0;
        for (uint32_t i = 0; i < n7; ++i) if (all7[i].ntsc > 0.0f) ++with_bleed;
        check(with_bleed == 1, "y es el unico que lo trae");
        // NTSC es CRT + cable: comparte la grilla de fosforo.
        check(crt && ntsc->crt_scale == crt->crt_scale &&
              ntsc->scan_scale == crt->scan_scale,
              "hereda el tubo del CRT: la diferencia es el cable");
    }

    // -- 8. El perfil se ve TAMBIEN sin pack ---------------------------------
    std::printf("\n[8] sin pack, manda el piso del perfil\n");
    {
        // EL DEFECTO: los tres efectos eran multiplicadores de lo que autora el
        // pack, y sin pack eso es cero. «CRT simulado» salia identico a «LCD
        // nativo» — 0 x 1 = 0— y el ajuste no cambiaba un solo pixel en el 100%
        // de los juegos que todavia no tienen pack.
        const OutputProfile& crt = *output_profile_by_id("crt");
        const OutputProfile& lcd = *output_profile_by_id("lcd");

        // OJO CON LOS ARGUMENTOS: sin pack el entorno de script NO devuelve
        // ceros. Devuelve crt_strength=0.0 pero scan_strength=0.5 y
        // vignette=0.2, que son sus defaults. Por eso el caso REAL se prueba
        // con esos numeros y no con tres ceros: una regla que mire los tres los
        // lee como autoria del pack, deja el gate en 0 —el unico que el pack no
        // puso— y el shader vuelve a ser passthrough.
        const OutputShader sin_pack = output_shader(crt, 0.0f, 0.5f, 0.2f);
        check(sin_pack.crt > 0.0f && sin_pack.scan > 0.0f,
              "CRT sin pack SI se ve (era identico a LCD)");
        // El gate es lo que decide, y es el unico que el pack no puso.
        check(output_shader(crt, 0.0f, 0.0f, 0.0f).crt == sin_pack.crt,
              "…y da igual con que defaults venga el interprete: manda el gate");
        const OutputShader lcd_sin = output_shader(lcd, 0.0f, 0.5f, 0.2f);
        check(lcd_sin.crt == 0.0f && lcd_sin.scan == 0.0f && lcd_sin.vignette == 0.0f,
              "…y LCD sigue sin poner nada, que es lo que «nativo» significa");

        // Con pack, no cambia la regla existente: manda el autor, escalado.
        const OutputShader con_pack = output_shader(crt, 0.4f, 0.5f, 0.25f);
        check(con_pack.crt > 0.39f && con_pack.crt < 0.41f,
              "con pack manda el pack, escalado por el perfil");
        check(output_shader(lcd, 0.4f, 0.5f, 0.25f).crt == 0.0f,
              "…y LCD lo sigue apagando");

        // La decision es POR PACK y no por parametro: un autor que pide
        // curvatura y deja las lineas en cero las dejo en cero a proposito.
        const OutputShader parcial = output_shader(crt, 0.4f, 0.0f, 0.0f);
        check(parcial.scan == 0.0f,
              "autoria parcial: el hueco NO se rellena con el piso del perfil");

        // Cinematografica sigue sin lineas de barrido tambien con su piso: es
        // una presentacion, no una imitacion de un televisor.
        const OutputShader cine = output_shader(*output_profile_by_id("cinema"),
                                                0.0f, 0.5f, 0.2f);
        check(cine.scan == 0.0f && cine.vignette > 0.0f,
              "cinematografica sin pack: viñeta si, lineas no");

        // NTSC hereda el tubo del CRT tambien en el piso: la diferencia sigue
        // siendo el cable.
        const OutputShader n = output_shader(*output_profile_by_id("ntsc"),
                                             0.0f, 0.5f, 0.2f);
        check(n.crt == sin_pack.crt && n.scan == sin_pack.scan,
              "NTSC hereda el tubo del CRT en el piso");
        check(n.ntsc > 0.0f && sin_pack.ntsc == 0.0f,
              "…y el sangrado sigue siendo solo suyo");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
