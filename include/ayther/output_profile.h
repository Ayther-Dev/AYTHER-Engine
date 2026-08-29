#pragma once
// ---------------------------------------------------------------------------
// output_profile.h — OUTPUT profiles.
//
// These are NOT the remastering profiles, and confusing the two would be the
// worst outcome here. The remastering ones say **what gets substituted** and
// the pack author decides them; these say **how it looks on YOUR screen** and
// whoever is playing decides them. A CRT does not change which assets come in,
// and a "Faithful" profile does not change whether you own a plasma or a
// laptop.
//
// That is why they live in different headers and why the vocabulary keeps them
// apart: "remastering profile" versus "output profile".
//
// The profile configures three things: SCALING, SMOOTHING and the presentation
// SHADERS. That is everything between the composed frame and the monitor.
//
// Header-only and Vulkan-free: it is tested without a GPU.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>

namespace ayther {

/// How the frame is brought to the window size.
enum class OutputScaling : uint8_t {
    /// The largest rect that preserves the aspect ratio. The usual one.
    Fit,
    /// INTEGER multiple of the native height, centred. This is what makes every
    /// game pixel occupy exactly the same screen pixels; with a non-integer
    /// scale, two identical pixel rows come out at different thicknesses and
    /// the artwork looks shimmery.
    Integer,
};

struct OutputProfile {
    const char*   id;
    const char*   name;
    OutputScaling scaling = OutputScaling::Fit;
    /// Filter used when scaling. false = nearest (every pixel hard),
    /// true = linear.
    bool  smoothing      = false;
    /// Multipliers over what the pack asks for in `shader_params`. They are NOT
    /// absolute values: the author already chose how much curvature suits their
    /// art, and the output profile decides how much of that reaches this
    /// screen. Overriding it with a fixed value would erase that decision.
    float crt_scale      = 0.0f;
    float scan_scale     = 0.0f;
    float vignette_scale = 0.0f;
    /// EM-7.2: composite-signal chroma bleed [0,1].
    ///
    /// Unlike the three above, this one is ABSOLUTE and not a multiplier: the
    /// pack has no `ntsc` to scale, because bleed is not the author's decision
    /// about their art but the viewer's decision about which television set to
    /// imitate. Scaling it against a value nobody authors would always give
    /// zero.
    float ntsc           = 0.0f;
};

/// The profiles the Engine knows about, in presentation order.
inline const OutputProfile* output_profiles(uint32_t* count) {
    static const OutputProfile kAll[] = {
        // The default is LCD and not CRT: on a modern screen the CRT is an
        // effect, and starting with an effect enabled would make the first look
        // at the remaster be the shader rather than the art.
        { "lcd",     "LCD nativo",     OutputScaling::Fit,     false, 0.0f, 0.0f, 0.0f, 0.0f },
        { "crt",     "CRT simulado",   OutputScaling::Fit,     true,  1.0f, 1.0f, 1.0f, 0.0f },
        { "pixel",   "Pixel-perfect",  OutputScaling::Integer, false, 0.0f, 0.0f, 0.0f, 0.0f },
        { "smooth",  "Suavizado",      OutputScaling::Fit,     true,  0.0f, 0.0f, 0.0f, 0.0f },
        // Cinematic: no scanlines, but with vignette and a little curvature.
        // It is a presentation, not an imitation of a television set.
        { "cinema",  "Cinematográfica", OutputScaling::Fit,    true,  0.35f, 0.0f, 1.0f, 0.0f },
        // EM-7.2: NTSC. It is the CRT plus the chroma bleed of a composite
        // signal — most games of this era were seen this way, with the
        // single-colour gradients that composite blended. It is kept apart from
        // "CRT simulado" because they are two different things: one is the tube
        // (the phosphor grid) and the other is the cable.
        { "ntsc",    "NTSC compuesto", OutputScaling::Fit,     true,  1.0f, 1.0f, 1.0f, 1.0f },
    };
    if (count) *count = static_cast<uint32_t>(sizeof(kAll) / sizeof(kAll[0]));
    return kAll;
}

/// Looks one up by id. nullptr = this build does not know it, which is not the
/// same as "there is no profile": a pack from tomorrow may recommend one that
/// does not exist yet.
inline const OutputProfile* output_profile_by_id(const char* id) {
    if (!id || !*id) return nullptr;
    uint32_t n = 0;
    const OutputProfile* all = output_profiles(&n);
    for (uint32_t i = 0; i < n; ++i)
        if (std::strcmp(all[i].id, id) == 0) return &all[i];
    return nullptr;
}

/// The default profile — the one used when nobody said anything.
inline const OutputProfile& output_profile_default() { return output_profiles(nullptr)[0]; }

/// RECOMMENDING IS NOT IMPOSING, and this is that rule written down.
///
/// Precedence: what the user CHOSE > what the pack RECOMMENDS > the default. A
/// pack may suggest "CRT" because its art was designed with that in mind, but
/// the person looking at the screen is the one who knows whether they have an
/// OLED or a projector — and if their choice did not win, the recommendation
/// would be an imposition under another name.
///
/// An unknown id (from either side) is ignored rather than treated as an error:
/// a pack from tomorrow may recommend a profile this build does not have, and
/// that does not invalidate the rest.
inline const OutputProfile& output_profile_resolve(const char* user_choice,
                                                   const char* pack_recommends) {
    if (const OutputProfile* p = output_profile_by_id(user_choice))    return *p;
    if (const OutputProfile* p = output_profile_by_id(pack_recommends)) return *p;
    return output_profile_default();
}

/// The destination rect for this profile. `Integer` looks for the largest
/// integer multiple that fits; if not even ×1 fits —a window smaller than the
/// frame— it falls back to `Fit`, because cropping the image would be worse
/// than scaling it badly.
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
        // Falls back to Fit: it does not fit even once.
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
