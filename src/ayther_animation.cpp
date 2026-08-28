// ---------------------------------------------------------------------------
// ayther_animation.cpp — AnimationPlayer (see ayther_animation.h).
//
// STATUS: contract/skeleton, NOT yet wired into the root CMakeLists.txt. Depends
// only on the ayther_geo_tween_* FFI (done + tested in C-S1) and std — no Vulkan
// — so it type-checks standalone:
//   clang-cl /std:c++20 /Zs -I include/ayther src/ayther_animation.cpp
// Left out of the engine build until VkSprite gains draw_anim() and produce_frame()
// drives it.
// ---------------------------------------------------------------------------

#include "ayther_animation.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace ayther {

struct AnimationPlayer::Impl {
    struct ClipDef {
        std::string                 sheet;
        int                         tween_level = 0;
        std::vector<HdPose>         poses;                 ///< keyframe order
        std::map<uint64_t, uint32_t> pose_index;          ///< pose hash → index
        std::vector<uint32_t>       cum_start;             ///< cumulative start tick per pose
        AytherGeometricTween*       tween = nullptr;       ///< Level-1 (else null)

        // runtime phase (Level 1): where we are in the cycle
        int32_t  last_pose        = -1;
        uint64_t pose_start_frame = 0;
    };

    std::map<uint64_t, ClipDef> clips;   ///< clip_id (autoría) → HD animation
    // Índice de PLAYBACK: pose hash → clip. El hash de la pose es la identidad
    // ESTABLE (content-based); el anim_group_id de las occurrences NO sirve para
    // resolver — es el hash del SET de la ventana rodante del grouper y cambia
    // cuando el set de poses visible muta (p. ej. Sonic acelerando). El clip_id
    // queda como handle de autoría (define/undefine).
    std::map<uint64_t, uint64_t> pose_to_clip;
    std::vector<AnimHdFrame>    out;

    void reindex() {
        pose_to_clip.clear();
        for (const auto& [id, c] : clips)
            for (const HdPose& p : c.poses) pose_to_clip[p.pose] = id;
    }

    ~Impl() { for (auto& [id, c] : clips) if (c.tween) ayther_geo_tween_free(c.tween); }
};

AnimationPlayer::AnimationPlayer() : impl_(std::make_unique<Impl>()) {}
AnimationPlayer::~AnimationPlayer() = default;
AnimationPlayer::AnimationPlayer(AnimationPlayer&&) noexcept            = default;
AnimationPlayer& AnimationPlayer::operator=(AnimationPlayer&&) noexcept = default;

void AnimationPlayer::define(uint64_t clip_id, const std::string& sheet,
                             const HdPose* poses, uint32_t pose_count, int tween_level) {
    undefine(clip_id);                       // replace
    if (!poses || pose_count == 0) return;

    Impl::ClipDef c;
    c.sheet       = sheet;
    c.tween_level = tween_level;
    c.poses.assign(poses, poses + pose_count);
    c.cum_start.resize(pose_count);
    uint32_t acc = 0;
    for (uint32_t i = 0; i < pose_count; ++i) {
        c.pose_index[poses[i].pose] = i;
        c.cum_start[i] = acc;
        acc += poses[i].duration_ticks ? poses[i].duration_ticks : 1u;
    }

    if (tween_level >= 1) {
        std::vector<AytherTransform> tf(pose_count);
        std::vector<uint16_t>        du(pose_count);
        for (uint32_t i = 0; i < pose_count; ++i) { tf[i] = poses[i].anchor; du[i] = poses[i].duration_ticks; }
        c.tween = ayther_geo_tween_new(tf.data(), du.data(), pose_count, /*looping*/ true);
    }
    impl_->clips.emplace(clip_id, std::move(c));
    impl_->reindex();
}

void AnimationPlayer::undefine(uint64_t clip_id) {
    auto it = impl_->clips.find(clip_id);
    if (it == impl_->clips.end()) return;
    if (it->second.tween) ayther_geo_tween_free(it->second.tween);
    impl_->clips.erase(it);
    impl_->reindex();
}

void AnimationPlayer::clear() noexcept {
    for (auto& [id, c] : impl_->clips) if (c.tween) ayther_geo_tween_free(c.tween);
    impl_->clips.clear();
    impl_->pose_to_clip.clear();
    impl_->out.clear();
}

size_t AnimationPlayer::clip_count() const noexcept { return impl_->clips.size(); }

std::vector<AnimationDef> AnimationPlayer::definitions() const {
    std::vector<AnimationDef> out;
    out.reserve(impl_->clips.size());
    for (const auto& [id, c] : impl_->clips)          // std::map → ordenado por clip_id
        out.push_back(AnimationDef{ id, c.sheet, c.tween_level, c.poses });
    return out;
}

uint32_t AnimationPlayer::resolve(const AytherSpriteOccurrence* occs, uint32_t n,
                                  uint64_t frame_index) {
    auto& im = *impl_;
    im.out.clear();
    if (!occs) return 0;

    for (uint32_t i = 0; i < n; ++i) {
        const AytherSpriteOccurrence& o = occs[i];
        // Resolver por el HASH de la pose (identidad estable, content-based) —
        // NO por o.anim_group_id: el id del grouper es el hash del set de la
        // ventana rodante y cambia cuando el set de poses visible muta.
        auto pc = im.pose_to_clip.find(o.hash);
        if (pc == im.pose_to_clip.end()) continue;        // pose sin animación HD
        auto ci = im.clips.find(pc->second);
        if (ci == im.clips.end()) continue;
        Impl::ClipDef& c = ci->second;

        auto pi = c.pose_index.find(o.hash);
        if (pi == c.pose_index.end()) continue;           // this pose has no HD frame
        const uint32_t idx = pi->second;
        const HdPose&  p   = c.poses[idx];

        AnimHdFrame f{};
        std::snprintf(f.asset, sizeof(f.asset), "%s", c.sheet.c_str());
        f.src_x = p.src_x; f.src_y = p.src_y; f.src_w = p.src_w; f.src_h = p.src_h;

        if (c.tween_level >= 1 && c.tween) {
            // Advance the cycle phase: reset when the observed pose (re)starts a
            // keyframe, otherwise carry the ticks held so far → interpolate the
            // anchor from this keyframe toward the next as it is held.
            if (int32_t(idx) != c.last_pose) { c.pose_start_frame = frame_index; c.last_pose = int32_t(idx); }
            const uint32_t within = uint32_t(frame_index - c.pose_start_frame);
            const uint32_t tick   = c.cum_start[idx] + within;
            const AytherTransform t = ayther_geo_tween_sample(c.tween, tick);
            f.dst_x = t.x; f.dst_y = t.y; f.dst_w = t.w; f.dst_h = t.h;
        } else {
            // Level 0 (Pop): draw at the observed metasprite bbox.
            f.dst_x = float(o.screen_x);
            f.dst_y = float(o.screen_y);
            f.dst_w = float(int(o.w_tiles) * 8);
            f.dst_h = float(int(o.h_tiles) * 8);
        }
        im.out.push_back(f);
    }
    return uint32_t(im.out.size());
}

const AnimHdFrame* AnimationPlayer::frames() const noexcept { return impl_->out.data(); }
uint32_t AnimationPlayer::frame_count() const noexcept { return uint32_t(impl_->out.size()); }

}  // namespace ayther
