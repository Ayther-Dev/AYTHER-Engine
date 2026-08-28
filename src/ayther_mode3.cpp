// ---------------------------------------------------------------------------
// ayther_mode3.cpp — Mode3Resolver implementation (see ayther_mode3.h).
//
// STATUS: contract/skeleton, NOT yet wired into the root CMakeLists.txt. It only
// depends on the ayther_game_profile_* FFI (done + validated in
// tools/mode3_spike) and std — no Vulkan/SDL — so it type-checks standalone
// (`clang-cl -fsyntax-only -I include/ayther`), but it is left out of the engine
// build until produce_frame() calls it, to keep the env-blocked engine target
// green. The geometry it drives is exactly the mode3_spike path.
// ---------------------------------------------------------------------------

#include "ayther_mode3.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ayther {

struct Mode3Resolver::Impl {
    AytherGameProfile* profile = nullptr;

    std::map<std::string, std::string> asset_by_name;   ///< kind name → HD asset
    std::vector<std::string>           asset_by_kind;   ///< kind index → HD asset (cache)

    std::vector<EntityInstance>  instances;
    std::vector<AytherSpriteSub> subs;

    // resolve() scratch (kept to avoid per-frame allocation)
    std::vector<int16_t>  sx, sy;
    std::vector<uint64_t> owner;

    // Resolve each kind's name → assigned asset once, indexed by kind index, so
    // resolve() maps an instance's kind directly to its asset.
    void rebuild_kind_cache() {
        asset_by_kind.clear();
        if (!profile) return;
        const size_t n = ayther_game_profile_kind_count(profile);
        asset_by_kind.resize(n);
        char buf[128];
        for (size_t i = 0; i < n; ++i) {
            if (ayther_game_profile_kind_name(profile, i, buf, sizeof(buf)) == 0) continue;
            auto it = asset_by_name.find(std::string(buf));
            if (it != asset_by_name.end()) asset_by_kind[i] = it->second;
        }
    }
};

Mode3Resolver::Mode3Resolver() noexcept : impl_(std::make_unique<Impl>()) {}

Mode3Resolver::~Mode3Resolver() {
    if (impl_ && impl_->profile) ayther_game_profile_free(impl_->profile);
}

Mode3Resolver::Mode3Resolver(Mode3Resolver&&) noexcept            = default;
Mode3Resolver& Mode3Resolver::operator=(Mode3Resolver&&) noexcept = default;

Result<void> Mode3Resolver::load_profile(const std::string& path) {
    if (impl_->profile) { ayther_game_profile_free(impl_->profile); impl_->profile = nullptr; }
    impl_->instances.clear();
    impl_->subs.clear();
    if (path.empty()) { impl_->rebuild_kind_cache(); return Result<void>::ok(); }

    impl_->profile = ayther_game_profile_load(path.c_str());
    if (!impl_->profile)
        return Result<void>::fail(ErrorCode::BadFormat,
                                  "Mode 3 profile: cannot load/parse " + path);
    impl_->rebuild_kind_cache();
    return Result<void>::ok();
}

Result<void> Mode3Resolver::load_profile_str(const std::string& toml) {
    if (impl_->profile) { ayther_game_profile_free(impl_->profile); impl_->profile = nullptr; }
    impl_->instances.clear();
    impl_->subs.clear();
    if (toml.empty()) { impl_->rebuild_kind_cache(); return Result<void>::ok(); }

    impl_->profile = ayther_game_profile_load_str(toml.c_str());
    if (!impl_->profile)
        return Result<void>::fail(ErrorCode::BadFormat,
                                  "Mode 3 profile: cannot parse TOML string");
    impl_->rebuild_kind_cache();
    return Result<void>::ok();
}

bool Mode3Resolver::has_profile() const noexcept { return impl_->profile != nullptr; }

void Mode3Resolver::assign_kind(const std::string& name, const std::string& asset) {
    if (asset.empty()) impl_->asset_by_name.erase(name);
    else               impl_->asset_by_name[name] = asset;
    impl_->rebuild_kind_cache();
}

void Mode3Resolver::clear_assignments() noexcept {
    impl_->asset_by_name.clear();
    impl_->asset_by_kind.clear();
}

void Mode3Resolver::resolve(const uint8_t* ram, size_t ram_size,
                            int32_t scroll_x, int32_t scroll_y,
                            int32_t plane_w, int32_t plane_h,
                            const AytherSpriteOccurrence* sprites, uint32_t n) {
    auto& im = *impl_;
    im.instances.clear();
    im.subs.clear();
    if (!im.profile || !ram || n == 0) return;

    // SAT sprite screen positions (top-left) → assign each to its entity.
    im.sx.resize(n);
    im.sy.resize(n);
    im.owner.assign(n, 0);
    for (uint32_t i = 0; i < n; ++i) { im.sx[i] = sprites[i].screen_x; im.sy[i] = sprites[i].screen_y; }

    ayther_game_profile_assign(im.profile, ram, ram_size,
                               scroll_x, scroll_y, plane_w, plane_h,
                               im.sx.data(), im.sy.data(), n, im.owner.data());

    // Aggregate the sprites each owner claimed into a screen bbox.
    struct Acc { int minx, miny, maxx, maxy; uint16_t count; };
    std::map<uint64_t, Acc> by_id;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t id = im.owner[i];
        if (id == 0) continue;                                  // unassigned sprite
        const int x0 = sprites[i].screen_x, y0 = sprites[i].screen_y;
        const int x1 = x0 + int(sprites[i].w_tiles) * 8;
        const int y1 = y0 + int(sprites[i].h_tiles) * 8;
        auto it = by_id.find(id);
        if (it == by_id.end()) {
            by_id.emplace(id, Acc{ x0, y0, x1, y1, 1 });
        } else {
            Acc& a = it->second;
            a.minx = std::min(a.minx, x0); a.miny = std::min(a.miny, y0);
            a.maxx = std::max(a.maxx, x1); a.maxy = std::max(a.maxy, y1);
            ++a.count;
        }
    }

    // Build instances (stable order by id) + per-instance HD substitutions.
    im.instances.reserve(by_id.size());
    for (const auto& [id, a] : by_id) {
        const int kind = ayther_game_profile_kind_of_id(im.profile, id);

        EntityInstance e;
        e.id           = id;
        e.kind         = kind;
        e.screen_x     = int16_t(a.minx);
        e.screen_y     = int16_t(a.miny);
        e.w_px         = uint16_t(std::max(0, a.maxx - a.minx));
        e.h_px         = uint16_t(std::max(0, a.maxy - a.miny));
        e.sprite_count = a.count;
        im.instances.push_back(e);

        if (kind >= 0 && size_t(kind) < im.asset_by_kind.size()
            && !im.asset_by_kind[size_t(kind)].empty()) {
            AytherSpriteSub s{};
            std::snprintf(s.asset_path, sizeof(s.asset_path), "%s",
                          im.asset_by_kind[size_t(kind)].c_str());
            s.screen_x = e.screen_x;
            s.screen_y = e.screen_y;
            s.w_tiles  = uint8_t(std::min(255, (int(e.w_px) + 7) / 8));
            s.h_tiles  = uint8_t(std::min(255, (int(e.h_px) + 7) / 8));
            s.palette  = 0xFF;   // sin ancla de paleta (anclado por RAM, no por occ)
            im.subs.push_back(s);
        }
    }
}

const EntityInstance* Mode3Resolver::instances() const noexcept {
    return impl_->instances.data();
}
uint32_t Mode3Resolver::instance_count() const noexcept {
    return static_cast<uint32_t>(impl_->instances.size());
}
const AytherSpriteSub* Mode3Resolver::subs() const noexcept {
    return impl_->subs.data();
}
uint32_t Mode3Resolver::sub_count() const noexcept {
    return static_cast<uint32_t>(impl_->subs.size());
}

}  // namespace ayther
