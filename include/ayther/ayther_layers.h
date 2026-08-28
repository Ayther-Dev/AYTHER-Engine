#pragma once
// ---------------------------------------------------------------------------
// AytherLayerStack is the engine's first-class layer model.
//
// The stack combines the VDP planes and renderer lanes into one explicit draw
// order. Each layer has independent visibility, and custom layers can be
// inserted at any position to support effects such as parallax.
//
// Element-level visibility is deliberately not represented here. It belongs
// to the session inventory and is controlled through
// AytherSession::set_hidden_elements().
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

enum class AytherLayerKind : uint8_t {
    VdpPlaneB = 0,   ///< VDP plane B.
    VdpPlaneA,       ///< VDP plane A.
    VdpWindow,       ///< VDP window plane.
    VdpSprites,      ///< VDP sprites.
    TileSubs,        ///< HD tile substitutions.
    Video,           ///< Cinematic video.
    Picture,         ///< Full-screen replacement.
    Panorama,        ///< Level-wide HD strip.
    PlaneTilesLo,    ///< Low-priority HD plane tiles.
    SpritesHd,       ///< HD sprite poses.
    Mode3,           ///< Instance replacement anchored to emulated RAM.
    Anim,            ///< In-phase sprite-sheet animations.
    /// Original high-priority VDP content drawn after HD background and
    /// sprite layers. It is used only for composed frames because the fallback
    /// VDP blit already contains this content.
    VdpFrente,
    PlaneTilesHi,    ///< High-priority HD plane tiles.
    Custom,          ///< User-inserted layer, including parallax content.
};

/// Describes the content of a custom layer.
///
/// The asset may come from disk or an Ayther pack. Its dimensions are supplied
/// by the caller so the renderer does not decode the image merely to measure
/// it. An empty `asset` denotes an ordering slot with no drawable content.
struct AytherLayerContent {
    char     asset[256] = {};
    uint16_t img_w = 0, img_h = 0;   ///< Native emulator pixels, scaled to the canvas.
    int16_t  y     = 0;              ///< Vertical position in native emulator pixels.
    uint8_t  anchor = 0;             ///< 0: plane B, 1: plane A, 2: level camera.
    float    factor = 0.5f;          ///< Parallax ratio relative to the anchor.
    /// Multiplies the PNG alpha without requiring the asset to be re-exported.
    float    opacity = 1.0f;
    /// Blend mode: 0 alpha, 1 additive, 2 multiply, 3 screen, 4 subtract,
    /// 5 darken, or 6 lighten.
    uint8_t  blend   = 0;
    /// When nonzero, stretch the quad to the visible frame. This mode ignores
    /// anchor, factor, tiling, and drift and is intended for fixed overlays.
    uint8_t  fit = 0;
    /// Deterministic opacity flicker. Zero disables the effect.
    float    flicker_amp   = 0.0f;
    uint16_t flicker_ticks = 8;
    /// Tiling mode: 0 repeats in X only; 1 repeats in both X and Y.
    uint8_t  tile_mode = 0;
    /// Per-frame deterministic drift added to the parallax offset. The
    /// renderer derives time from the captured frame, never a wall clock.
    float    drift_x = 0.0f;
    float    drift_y = 0.0f;
    /// Palette-following tint. `pal_line` is the source CRAM line; 0xFF
    /// disables tinting. `ref_rgb` stores its normal-state average color.
    uint8_t  pal_line   = 0xFF;
    uint8_t  ref_rgb[3] = { 0, 0, 0 };
    /// Selects palette entries used for tinting; bit `i` selects entry `i`.
    uint16_t tint_mask  = 0xFFFE;
    /// Optional screen-presence gate. An empty list makes the layer global.
    static constexpr uint32_t kMaxScreens = 8;
    uint64_t screen_ids[kMaxScreens] = {};
    uint8_t  screen_count = 0;
    [[nodiscard]] bool gated() const noexcept { return screen_count != 0; }
    [[nodiscard]] bool has_screen(uint64_t id) const noexcept {
        for (uint32_t i = 0; i < screen_count; ++i)
            if (screen_ids[i] == id) return true;
        return false;
    }
    /// Adds a screen idempotently. Returns false for zero or a full list.
    [[nodiscard]] bool add_screen(uint64_t id) noexcept {
        if (!id || has_screen(id)) return id != 0;
        if (screen_count >= kMaxScreens) return false;
        screen_ids[screen_count++] = id;
        return true;
    }
    void remove_screen(uint64_t id) noexcept {
        uint32_t w = 0;
        for (uint32_t i = 0; i < screen_count; ++i)
            if (screen_ids[i] != id) screen_ids[w++] = screen_ids[i];
        for (uint32_t i = w; i < screen_count; ++i) screen_ids[i] = 0;
        screen_count = static_cast<uint8_t>(w);
    }
    void clear_screens() noexcept { for (auto& s : screen_ids) s = 0; screen_count = 0; }
    /// Gate mode: 0 matches the exact screen signature; 1 matches any
    /// requested screen found in the current presence set.
    uint8_t  gate_presence = 0;
    /// Animation sequence. `asset` is frame zero and `anim` contains up to
    /// three additional deterministic frames.
    uint8_t  anim_count = 0;          ///< Additional frames; zero is static.
    uint16_t anim_ticks = 8;
    char     anim[3][256] = {};
};

/// Returns whether a custom layer's screen gate is open for this frame.
/// This pure helper is testable without a GPU. `presence_ids` must address at
/// least `presence_count` elements when presence mode is enabled.
[[nodiscard]] inline bool overlay_gate_open(const AytherLayerContent& cc, uint64_t match_id,
                                            const uint64_t* presence_ids,
                                            uint32_t presence_count) noexcept {
    if (!cc.gated()) return true;
    if (!cc.gate_presence) return cc.has_screen(match_id);
    for (uint32_t gi = 0; gi < presence_count; ++gi)
        if (cc.has_screen(presence_ids[gi])) return true;
    return false;
}
/// Maps an integer step to deterministic flicker noise in [0, 1).
[[nodiscard]] inline float overlay_flicker_factor(uint32_t step) noexcept {
    uint32_t z = step + 0x9E3779B9u;
    z ^= z >> 16; z *= 0x21F0AAADu;
    z ^= z >> 15; z *= 0x735A2D97u;
    z ^= z >> 15;
    return static_cast<float>(z >> 8) / 16777216.0f;
}

/// Returns the animation frame selected by the captured frame number.
[[nodiscard]] inline uint32_t overlay_animation_step(uint32_t frame_index, uint16_t ticks,
                                                uint8_t extra_frames) noexcept {
    const uint32_t total = 1u + extra_frames;
    if (total <= 1 || !ticks) return 0;
    return (frame_index / ticks) % total;
}

struct AytherLayer {
    uint32_t        id      = 0;      ///< Stable for the lifetime of this stack.
    AytherLayerKind kind    = AytherLayerKind::Custom;
    char            name[24] = {};
    bool            visible = true;
    bool            reorderable = true;   ///< False when hardware fixes relative order.
    AytherLayerContent content;           ///< Used only when `kind` is Custom.
};

class AytherLayerStack {
public:
    AytherLayerStack() {
        struct D { AytherLayerKind k; const char* n; bool r; };
        static constexpr D kDefaults[] = {
            { AytherLayerKind::VdpPlaneB,    "Plano B",         false },
            { AytherLayerKind::VdpPlaneA,    "Plano A",         false },
            { AytherLayerKind::VdpWindow,    "Window",          false },
            { AytherLayerKind::VdpSprites,   "Sprites",         false },
            { AytherLayerKind::TileSubs,     "Tiles HD",        true  },
            { AytherLayerKind::Video,        "Video",           true  },
            { AytherLayerKind::Picture,      "Picture",         true  },
            { AytherLayerKind::Panorama,     "Panorámica",      true  },
            { AytherLayerKind::PlaneTilesLo, "Fondo HD",        true  },
            { AytherLayerKind::SpritesHd,    "Sprites HD",      true  },
            { AytherLayerKind::Mode3,        "Entidades",       true  },
            { AytherLayerKind::Anim,         "Animaciones",     true  },
            { AytherLayerKind::VdpFrente,    "Primer plano",    true  },
            { AytherLayerKind::PlaneTilesHi, "Primer plano HD", true  },
        };
        for (const D& d : kDefaults) {
            AytherLayer l;
            l.id = next_id_++;
            l.kind = d.k;
            l.reorderable = d.r;
            std::snprintf(l.name, sizeof(l.name), "%s", d.n);
            layers_.push_back(l);
        }
    }

    [[nodiscard]] const std::vector<AytherLayer>& layers() const noexcept { return layers_; }

    /// Returns a borrowed pointer that remains valid until the stack is mutated.
    [[nodiscard]] const AytherLayer* find(uint32_t id) const noexcept {
        for (const AytherLayer& l : layers_) if (l.id == id) return &l;
        return nullptr;
    }

    [[nodiscard]] bool set_visible(uint32_t id, bool v) noexcept {
        for (AytherLayer& l : layers_) if (l.id == id) { l.visible = v; return true; }
        return false;
    }

    /// Moves a reorderable layer to `index` in back-to-front draw order.
    /// Hardware VDP layers cannot be moved, but custom layers may be inserted
    /// between them because composition follows the complete stack order.
    [[nodiscard]]
    bool move(uint32_t id, size_t index) {
        const size_t cur = index_of(id);
        if (cur == kNone || !layers_[cur].reorderable) return false;
        if (index >= layers_.size()) index = layers_.size() - 1;
        if (index == cur) return true;
        AytherLayer l = layers_[cur];
        layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(cur));
        layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(index), l);
        return true;
    }

    /// Inserts an empty custom layer at `index` and returns its stable id.
    [[nodiscard]]
    uint32_t insert_custom(const char* name, size_t index) {
        AytherLayer l;
        l.id = next_id_++;
        l.kind = AytherLayerKind::Custom;
        std::snprintf(l.name, sizeof(l.name), "%s", name && *name ? name : "Capa");
        if (index > layers_.size()) index = layers_.size();
        layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(index), l);
        return l.id;
    }

    /// Replaces custom-layer content. Returns false for an unknown or non-custom id.
    [[nodiscard]]
    bool set_content(uint32_t id, const AytherLayerContent& c) {
        const size_t i = index_of(id);
        if (i == kNone || layers_[i].kind != AytherLayerKind::Custom) return false;
        layers_[i].content = c;
        return true;
    }

    /// Removes a custom layer. Built-in layers must be hidden instead.
    [[nodiscard]]
    bool remove(uint32_t id) {
        const size_t i = index_of(id);
        if (i == kNone || layers_[i].kind != AytherLayerKind::Custom) return false;
        layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }

    /// Derives the VDP visibility mask (A=0x01, B=0x02, Window=0x04,
    /// Sprites=0x08) from the built-in layers.
    [[nodiscard]] uint8_t vdp_mask() const noexcept {
        uint8_t m = 0;
        for (const AytherLayer& l : layers_) {
            if (!l.visible) continue;
            switch (l.kind) {
                case AytherLayerKind::VdpPlaneA:  m |= 0x01; break;
                case AytherLayerKind::VdpPlaneB:  m |= 0x02; break;
                case AytherLayerKind::VdpWindow:  m |= 0x04; break;
                case AytherLayerKind::VdpSprites: m |= 0x08; break;
                default: break;
            }
        }
        return m;
    }

    [[nodiscard]] static const char* kind_name(AytherLayerKind k) noexcept {
        switch (k) {
            case AytherLayerKind::VdpPlaneB:    return "vdp_plane_b";
            case AytherLayerKind::VdpPlaneA:    return "vdp_plane_a";
            case AytherLayerKind::VdpWindow:    return "vdp_window";
            case AytherLayerKind::VdpSprites:   return "vdp_sprites";
            case AytherLayerKind::TileSubs:     return "tile_subs";
            case AytherLayerKind::Video:        return "video";
            case AytherLayerKind::Picture:      return "picture";
            case AytherLayerKind::Panorama:     return "panorama";
            case AytherLayerKind::PlaneTilesLo: return "plane_tiles_lo";
            case AytherLayerKind::SpritesHd:    return "sprites_hd";
            case AytherLayerKind::Mode3:        return "mode3";
            case AytherLayerKind::Anim:         return "anim";
            case AytherLayerKind::VdpFrente:    return "vdp_frente";
            case AytherLayerKind::PlaneTilesHi: return "plane_tiles_hi";
            case AytherLayerKind::Custom:       return "custom";
        }
        return "?";
    }

private:
    // Parentheses prevent the Windows `max` macro from rewriting this call.
    static constexpr size_t kNone = (std::numeric_limits<size_t>::max)();

    [[nodiscard]] size_t index_of(uint32_t id) const noexcept {
        for (size_t i = 0; i < layers_.size(); ++i)
            if (layers_[i].id == id) return i;
        return kNone;
    }
    std::vector<AytherLayer> layers_;
    uint32_t                 next_id_ = 1;
};
