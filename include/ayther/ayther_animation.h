#pragma once
// ---------------------------------------------------------------------------
// ayther_animation.h — C-S2: play an HD animation **in phase** with the game.
//
// ## What this is
//
// An action is an ordered set of timed poses (AnimationClip, C-S1). C-S2
// draws it in HD: for whatever pose the game shows **this frame**, draw that
// pose's HD frame at the metasprite's on-screen bbox — so the HD animation runs
// exactly in sync with gameplay (no free-running loop), driven by the observed
// sprite occurrences (each carries `anim_group_id` = the clip, `hash` = the pose,
// and its screen bbox).
//
// Tween levels (authoring model section 2.3):
//   0 · Pop           — draw the pose's HD frame at the observed bbox (the game
//                       already moves the bbox every frame → smooth position).
//   1 · Geometric     — interpolate the *transform* (bbox pos/size) between the
//                       pose keyframes across the ticks each is held, via the
//                       GeometricTween (C-S1, ayther_geo_tween_*), so a single HD
//                       per pose glides between keyframes instead of popping.
//
// ## Renderer contract
//
// VkSprite today draws a whole texture per AytherSpriteSub. An HD **sheet** (one
// image, many pose frames) needs a **sub-rect (UV)** and a float dst rect for
// sub-pixel-smooth tweening — this header's `AnimHdFrame`. The renderer-side work
// is a `VkSprite::draw_anim(const AnimHdFrame*, n, pack)` variant that samples
// the sheet with these UVs (the existing whole-texture path is unchanged).
//
// ## Integration status
//
// The source is part of `Ayther::engine`; `AytherSession` resolves frames,
// `AytherRenderer` submits the animation lane, and `VkSprite::draw_anim`
// samples sheet sub-rectangles. Pack authoring writes animations.toml and the
// animation smoke tool exercises the complete path.
//
// This component is a pose-hash-to-frame map, not an independent timeline. The
// game supplies the cursor. Level-1 phase state must be reset after detection
// loss or non-sequential seeks; callers must not assume backward-seek safety
// until that reset contract is enforced.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ayther_core_ffi.h"   // AytherSpriteOccurrence, AytherGeometricTween, AytherTransform

namespace ayther {

// One HD animation frame to draw: a sub-rect of a sheet at a float screen rect.
// (dst float → sub-pixel-smooth tween; src px, src_w <= 0 → the whole texture.)
struct AnimHdFrame {
    char  asset[256] = {0};   ///< sheet path in the .ay pack
    float dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;   ///< screen px
    float src_x = 0, src_y = 0, src_w = 0, src_h = 0;   ///< sheet sub-rect px
};

// One pose of an HD animation: the game pose hash it maps to, its frame in the
// sheet, and (for Level-1 tween) its authored anchor transform + hold duration.
struct HdPose {
    uint64_t        pose = 0;                  ///< == AytherSpriteOccurrence.hash
    float           src_x = 0, src_y = 0, src_w = 0, src_h = 0;   ///< sub-rect in the sheet (px)
    AytherTransform anchor = {0, 0, 0, 0};     ///< keyframe dst (px) — Level-1 tween
    uint16_t        duration_ticks = 1;        ///< ticks this pose is held
};

// Complete definition used when serializing `animations.toml`.
struct AnimationDef {
    uint64_t            clip_id     = 0;
    std::string         sheet;
    int                 tween_level = 0;
    std::vector<HdPose> poses;
};

class AnimationPlayer {
public:
    AnimationPlayer();
    ~AnimationPlayer();
    AnimationPlayer(const AnimationPlayer&)            = delete;
    AnimationPlayer& operator=(const AnimationPlayer&) = delete;
    AnimationPlayer(AnimationPlayer&&) noexcept;
    AnimationPlayer& operator=(AnimationPlayer&&) noexcept;

    // -- Author an HD animation for a clip (anim_group_id) --------------------
    /// Define the sheet + pose→frame map + tween level (0 Pop / 1 Geometric).
    /// Re-defining a clip replaces it. `tween_level >= 1` builds a GeometricTween
    /// from the poses' anchor transforms + durations.
    void   define(uint64_t clip_id, const std::string& sheet_asset,
                  const HdPose* poses, uint32_t pose_count, int tween_level);
    void   undefine(uint64_t clip_id);
    void   clear() noexcept;
    size_t clip_count() const noexcept;
    /// Returns active definitions sorted by clip ID for serialization.
    std::vector<AnimationDef> definitions() const;

    // -- Per-frame resolve ----------------------------------------------------
    /// Emit the HD frames to draw: for each on-screen occurrence whose pose
    /// `hash` belongs to a defined animation, the pose's frame at the observed
    /// bbox (Level 0) or the tweened anchor (Level 1). Resolution is keyed by
    /// the POSE HASH (stable, content-based) — NOT by `anim_group_id`, which is
    /// the grouper's rolling-window set hash and churns as the visible pose set
    /// changes; the clip id is only the authoring handle. `frame_index` drives
    /// the tween phase. Returns the count.
    uint32_t resolve(const AytherSpriteOccurrence* occs, uint32_t occ_count,
                     uint64_t frame_index);

    // -- Results (borrowed; valid until the next resolve()) -------------------
    const AnimHdFrame* frames()      const noexcept;
    uint32_t           frame_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ayther
