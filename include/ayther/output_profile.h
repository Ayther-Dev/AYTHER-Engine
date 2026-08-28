#pragma once
// ---------------------------------------------------------------------------
// output_profile.h — perfiles de SALIDA ().
//
// NO son los perfiles de remasterización (), y confundirlos sería el peor
// resultado de este issue. Los de  dicen **qué se sustituye** y los decide
// el autor del pack; éstos dicen **cómo se ve en TU pantalla** y los decide
// quien juega. Un CRT no cambia qué assets entran, y un perfil «Fiel» no cambia
// si tenés un plasma o un portátil.
//
// Por eso viven en headers distintos y por eso el vocabulario los separa:
// «perfil de remasterización» contra «perfil de salida».
//
// El perfil configura tres cosas: el ESCALADO, el SUAVIZADO y los SHADERS de
// presentación. Es todo lo que hay entre el frame ya compuesto y el monitor.
//
// Header-only y sin Vulkan: se testea sin GPU.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>

namespace ayther {

/// Cómo se lleva el frame al tamaño de la ventana.
enum class OutputScaling : uint8_t {
    /// El rect más grande que preserva el aspecto. Lo de siempre.
    Fit,
    /// Múltiplo ENTERO del alto nativo, centrado. Es lo que hace que cada píxel
    /// del juego ocupe exactamente los mismos píxeles de pantalla; con un
    /// escalado no entero, dos filas de píxeles idénticas salen de distinto
    /// grosor y el dibujo se ve tembloroso.
    Integer,
};

struct OutputProfile {
    const char*   id;
    const char*   name;
    OutputScaling scaling = OutputScaling::Fit;
    /// Filtro al escalar. false = nearest (cada píxel duro), true = lineal.
    bool  smoothing      = false;
    /// Multiplicadores sobre lo que el pack pide en `shader_params`. NO son
    /// valores absolutos: el autor ya eligió cuánta curvatura queda bien con su
    /// arte, y el perfil de salida decide cuánto de eso llega a esta pantalla.
    /// Pisarlo con un valor fijo borraría esa decisión.
    float crt_scale      = 0.0f;
    float scan_scale     = 0.0f;
    float vignette_scale = 0.0f;
    ///  EM-7.2: sangrado de croma de señal compuesta [0,1].
    ///
    /// A diferencia de los tres de arriba éste es ABSOLUTO y no un
    /// multiplicador: el pack no tiene un `ntsc` que escalar, porque el
    /// sangrado no es una decisión del autor sobre su arte sino sobre qué
    /// televisor imita quien mira. Escalarlo contra un valor que nadie autora
    /// daría siempre cero.
    float ntsc           = 0.0f;
};

/// Los perfiles que el Engine conoce. En orden de presentación.
inline const OutputProfile* output_profiles(uint32_t* count) {
    static const OutputProfile kAll[] = {
        // El default es LCD y no CRT: en una pantalla moderna el CRT es un
        // efecto, y arrancar con un efecto puesto haría que el primer vistazo
        // de la remasterización fuera el shader y no el arte.
        { "lcd",     "LCD nativo",     OutputScaling::Fit,     false, 0.0f, 0.0f, 0.0f, 0.0f },
        { "crt",     "CRT simulado",   OutputScaling::Fit,     true,  1.0f, 1.0f, 1.0f, 0.0f },
        { "pixel",   "Pixel-perfect",  OutputScaling::Integer, false, 0.0f, 0.0f, 0.0f, 0.0f },
        { "smooth",  "Suavizado",      OutputScaling::Fit,     true,  0.0f, 0.0f, 0.0f, 0.0f },
        // Cinematográfica: sin líneas de barrido pero con viñeta y un poco de
        // curvatura. Es una presentación, no una imitación de un televisor.
        { "cinema",  "Cinematográfica", OutputScaling::Fit,    true,  0.35f, 0.0f, 1.0f, 0.0f },
        //  EM-7.2: NTSC. Es el CRT más el sangrado de croma de una señal
        // compuesta — la mayoría de los juegos de esta época se vieron así, con
        // los degradados de un solo color que el compuesto mezclaba. Va aparte
        // de «CRT simulado» porque son dos cosas distintas: uno es el tubo (la
        // grilla de fósforo) y el otro, el cable.
        { "ntsc",    "NTSC compuesto", OutputScaling::Fit,     true,  1.0f, 1.0f, 1.0f, 1.0f },
    };
    if (count) *count = static_cast<uint32_t>(sizeof(kAll) / sizeof(kAll[0]));
    return kAll;
}

/// Busca por id. nullptr = este build no lo conoce, que es distinto de «no hay
/// perfil»: un pack de mañana puede recomendar uno que no existe todavía.
inline const OutputProfile* output_profile_by_id(const char* id) {
    if (!id || !*id) return nullptr;
    uint32_t n = 0;
    const OutputProfile* all = output_profiles(&n);
    for (uint32_t i = 0; i < n; ++i)
        if (std::strcmp(all[i].id, id) == 0) return &all[i];
    return nullptr;
}

/// El perfil por defecto — el que se usa cuando nadie dijo nada.
inline const OutputProfile& output_profile_default() { return output_profiles(nullptr)[0]; }

/// RECOMENDAR NO ES IMPONER, y esto es esa regla escrita.
///
/// Precedencia: lo que ELIGIÓ el usuario > lo que RECOMIENDA el pack > el
/// default. Un pack puede sugerir «CRT» porque su arte se diseñó con eso en
/// mente, pero quien mira la pantalla es el que sabe si tiene un OLED o un
/// proyector — y si su elección no ganara, la recomendación sería una
/// imposición con otro nombre.
///
/// Un id desconocido (de cualquiera de los dos) se ignora en vez de fallar: un
/// pack de mañana puede recomendar un perfil que este build no tiene, y eso no
/// invalida el resto.
inline const OutputProfile& output_profile_resolve(const char* user_choice,
                                                   const char* pack_recommends) {
    if (const OutputProfile* p = output_profile_by_id(user_choice))    return *p;
    if (const OutputProfile* p = output_profile_by_id(pack_recommends)) return *p;
    return output_profile_default();
}

/// El rect de destino para este perfil. `Integer` busca el múltiplo entero más
/// grande que entre; si ni ×1 entra —una ventana más chica que el frame— cae a
/// `Fit`, porque recortar la imagen sería peor que escalarla mal.
struct OutputRect { int32_t x, y, w, h; };

inline OutputRect output_rect(const OutputProfile& p,
                              uint32_t src_w, uint32_t src_h,
                              uint32_t dst_w, uint32_t dst_h) {
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        return { 0, 0, static_cast<int32_t>(dst_w), static_cast<int32_t>(dst_h) };

    if (p.scaling == OutputScaling::Integer) {
        const uint32_t k = (dst_w / src_w) < (dst_h / src_h) ? (dst_w / src_w)
                                                             : (dst_h / src_h);
        if (k >= 1) {
            const int32_t w = static_cast<int32_t>(src_w * k);
            const int32_t h = static_cast<int32_t>(src_h * k);
            return { (static_cast<int32_t>(dst_w) - w) / 2,
                     (static_cast<int32_t>(dst_h) - h) / 2, w, h };
        }
        // Cae a Fit: no entra ni una vez.
    }

    const double src_a = static_cast<double>(src_w) / src_h;
    const double dst_a = static_cast<double>(dst_w) / dst_h;
    int32_t w, h;
    if (dst_a > src_a) {
        h = static_cast<int32_t>(dst_h);
        w = static_cast<int32_t>(dst_h * src_a + 0.5);
    } else {
        w = static_cast<int32_t>(dst_w);
        h = static_cast<int32_t>(dst_w / src_a + 0.5);
    }
    return { (static_cast<int32_t>(dst_w) - w) / 2,
             (static_cast<int32_t>(dst_h) - h) / 2, w, h };
}

}  // namespace ayther
