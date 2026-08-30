#include "session/emulation_observer.h"
#include "log.h"

#include "runtime_options.h"

#include <algorithm>
#include <cstdio>

namespace ayther::session {

namespace {

#if defined(__clang__)
#define AYTHER_OBSERVER_LEGACY_BEGIN \
    _Pragma("clang diagnostic push") \
    _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define AYTHER_OBSERVER_LEGACY_END _Pragma("clang diagnostic pop")
#else
#define AYTHER_OBSERVER_LEGACY_BEGIN
#define AYTHER_OBSERVER_LEGACY_END
#endif

}  // namespace

void EmulationObserver::activate_subscriptions(RetroRunner& runner) {
    if (!runner.has_ayther_v1()) return;
    const ayther_interface_v1* api = runner.ayther_api();
    if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1)) return;

    ayther_subscription_state_v1 state{};
    state.struct_size = sizeof(state);
    if (api->get_subscriptions(&state, sizeof(state)) != AYTHER_STATUS_OK) {
        ayther::log::write(ayther::log::Severity::Error,
            "session", "get_subscriptions_fallo_sin",
            "get_subscriptions fallo — sin suscripciones");
        return;
    }
    const uint32_t wanted = RetroRunner::kEngineSubscriptions & state.supported_mask;
    const int32_t result = api->set_subscriptions(wanted);
    if (result != AYTHER_STATUS_OK) {
        ayther::log::write(ayther::log::Severity::Error,
            "session", "set_subscriptions_fallo",
            "set_subscriptions fallo: %d",
            result);
        return;
    }
    requested_ = wanted;
    subscriptions_verified_ = false;
    ayther::log::write(ayther::log::Severity::Info,
        "session", "suscripciones_ayther_pedidas_x",
        "suscripciones AYTHER pedidas: 0x%08X "
                 "(soportadas: 0x%08X)",
        wanted,
        state.supported_mask);
}

void EmulationObserver::verify_subscriptions(RetroRunner& runner) {
    if (subscriptions_verified_ || !requested_ || !runner.has_ayther_v1()) return;
    const ayther_interface_v1* api = runner.ayther_api();
    if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1)) return;

    ayther_subscription_state_v1 state{};
    state.struct_size = sizeof(state);
    if (api->get_subscriptions(&state, sizeof(state)) != AYTHER_STATUS_OK) return;
    if (state.active_mask == requested_) {
        ayther::log::write(ayther::log::Severity::Info,
            "session", "suscripciones_ayther_activas_x",
            "suscripciones AYTHER activas: 0x%08X",
            state.active_mask);
    } else {
        ayther::log::write(ayther::log::Severity::Warning,
            "session", "suscripciones_desalineadas_activas_x",
            "suscripciones DESALINEADAS — activas=0x%08X "
                     "pedidas=0x%08X",
            state.active_mask,
            requested_);
    }
    subscriptions_verified_ = true;
}

void EmulationObserver::reapply_subscriptions(RetroRunner& runner) {
    if (!requested_ || !runner.has_ayther_v1()) return;
    subscriptions_verified_ = false;
    runner.ayther_api()->set_subscriptions(requested_);
}

void EmulationObserver::initialize_system(RetroRunner& runner) {
    if (runner.has_ayther_v1()) system_ok_ = runner.read_system_v1(system_).ok();
}

bool EmulationObserver::mirror_enabled() {
    static const bool enabled = [] {
        return RuntimeOptions::process().abi_mirror();
    }();
    return enabled;
}

void EmulationObserver::refresh(RetroRunner& runner) {
    snapshot_ok_ = false;
    sprites_.clear();
    sprite_count_ = 0;
    audio_.clear();
    if (!mirror_enabled() || !runner.has_ayther_v1() ||
        !runner.capture_frame_snapshot(snapshot_).ok()) {
        return;
    }

    system_ok_ = runner.read_system_v1(system_).ok();
    if (system_ok_ && !system_logged_ && system_.vdp_mode != 0) {
        system_logged_ = true;
        ayther::log::write(ayther::log::Severity::Info,
            "session", "system_hw_x_vdp",
            "SYSTEM: hw=0x%02X vdp_mode=%u h40=%u interlace=%u "
                     "sh=%u %s lines=%u viewport=%ux%u@(%u,%u) geometry_pending=%u",
            system_.system_hw,
            system_.vdp_mode,
            system_.h40,
            system_.interlace,
            system_.shadow_highlight,
            system_.region_pal ? "PAL" : "NTSC",
            system_.lines_per_frame,
            system_.viewport_w,
            system_.viewport_h,
            system_.viewport_x,
            system_.viewport_y,
            static_cast<unsigned>(system_.flags & AYTHER_SYSTEM_GEOMETRY_PENDING));
    }

    auto read_region = [&](std::vector<uint8_t>& destination, size_t legacy_size,
                           uint32_t region,
                           RetroRunner::AytherReadResult (RetroRunner::*read)(
                               void*, const ayther_frame_snapshot_v1&) const) {
        const size_t abi_size = runner.abi_region_bytes(region);
        const size_t size = (std::max)(abi_size, legacy_size);
        if (!size) {
            destination.clear();
            return;
        }
        destination.resize(size);
        const auto result = (runner.*read)(destination.data(), snapshot_);
        if (!result.ok() || (result.count && result.count < size)) destination.clear();
    };
    read_region(vram_, runner.video_ram_size(), AYTHER_REGION_VRAM, &RetroRunner::read_vram_v1);
    read_region(cram_, runner.color_ram_size(), AYTHER_REGION_CRAM, &RetroRunner::read_cram_v1);
    read_region(regs_, runner.vdp_regs_size(), AYTHER_REGION_VDP_REGS,
                &RetroRunner::read_vdp_regs_v1);
    read_region(vsram_, runner.vsram_size(), AYTHER_REGION_VSRAM, &RetroRunner::read_vsram_v1);
    snapshot_ok_ = true;
}

AYTHER_OBSERVER_LEGACY_BEGIN
const uint8_t* EmulationObserver::vram(const RetroRunner& runner) const {
    return !vram_.empty() ? vram_.data() : runner.video_ram();
}

const uint8_t* EmulationObserver::cram(const RetroRunner& runner) const {
    return !cram_.empty() ? cram_.data() : runner.color_ram();
}

const uint8_t* EmulationObserver::regs(const RetroRunner& runner) const {
    return !regs_.empty() ? regs_.data() : runner.vdp_regs();
}

const uint8_t* EmulationObserver::vsram(const RetroRunner& runner) const {
    return !vsram_.empty() ? vsram_.data() : runner.vsram();
}
AYTHER_OBSERVER_LEGACY_END

EmulationObserver::AudioWritesView EmulationObserver::audio_writes(RetroRunner& runner) {
    if (!snapshot_ok_) {
        AYTHER_OBSERVER_LEGACY_BEGIN
        return {runner.audio_writes(), runner.audio_write_count(), false};
        AYTHER_OBSERVER_LEGACY_END
    }
    return audio_writes(runner, snapshot_, true);
}

EmulationObserver::AudioWritesView EmulationObserver::audio_writes(
    RetroRunner& runner, const ayther_frame_snapshot_v1& snapshot, bool legacy_fallback) {
    if (runner.has_ayther_v1()) {
        audio_.resize(snapshot.audio_write_count);
        const auto result = runner.read_audio_writes_v1(
            audio_.data(), static_cast<uint32_t>(audio_.size()), snapshot);
        if (result.ok()) {
            return {reinterpret_cast<const RetroRunner::AudioWrite*>(audio_.data()),
                    result.count, true};
        }
        if (result.status == AYTHER_STATUS_NOT_SUBSCRIBED && !audio_warned_) {
            audio_warned_ = true;
            ayther::log::write(ayther::log::Severity::Warning,
                "session", "audio_writes_sin_suscripcion",
                "AUDIO_WRITES sin suscripcion — "
                         "las escrituras siguen por el camino legacy");
        }
        if (!legacy_fallback) return {};
    }
    AYTHER_OBSERVER_LEGACY_BEGIN
    return {runner.audio_writes(), runner.audio_write_count(), false};
    AYTHER_OBSERVER_LEGACY_END
}

EmulationObserver::ParsedSpritesView EmulationObserver::parsed_sprites(RetroRunner& runner) {
    if (snapshot_ok_) {
        sprites_.resize(snapshot_.parsed_sprite_count);
        const auto result = runner.read_parsed_sprites_v1(
            sprites_.data(), static_cast<uint32_t>(sprites_.size()), snapshot_);
        if (result.ok()) {
            sprite_count_ = result.count;
            return {reinterpret_cast<const uint8_t*>(sprites_.data()), result.count, true};
        }
        if (result.status == AYTHER_STATUS_NOT_SUBSCRIBED && !sprites_warned_) {
            sprites_warned_ = true;
            ayther::log::write(ayther::log::Severity::Warning,
                "session", "sprite_capture_sin_suscripcion",
                "SPRITE_CAPTURE sin suscripcion — "
                         "los sprites siguen por el camino legacy");
        }
    }
    AYTHER_OBSERVER_LEGACY_BEGIN
    return {runner.parsed_sprites(), runner.parsed_sprite_count(), false};
    AYTHER_OBSERVER_LEGACY_END
}

const uint8_t* EmulationObserver::cached_parsed_sprites(uint8_t* count) const noexcept {
    if (!snapshot_ok_ || !sprite_count_) return nullptr;
    if (count) *count = static_cast<uint8_t>((std::min)(sprite_count_, 255u));
    return reinterpret_cast<const uint8_t*>(sprites_.data());
}

bool EmulationObserver::mark_raster_overflow_logged() noexcept {
    if (raster_overflow_logged_) return false;
    raster_overflow_logged_ = true;
    return true;
}

bool EmulationObserver::mark_raster_unsupported_logged() noexcept {
    if (raster_unsupported_logged_) return false;
    raster_unsupported_logged_ = true;
    return true;
}

}  // namespace ayther::session
