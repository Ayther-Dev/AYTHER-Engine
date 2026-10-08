#include "session/emulation_observer.h"
#include "log.h"

#include "runtime_options.h"
#include "session/parsed_sprite_join.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ayther::session {

namespace {

#if defined(__clang__)
#define AYTHER_OBSERVER_LEGACY_BEGIN                                           \
  _Pragma("clang diagnostic push")                                             \
      _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define AYTHER_OBSERVER_LEGACY_END _Pragma("clang diagnostic pop")
#else
#define AYTHER_OBSERVER_LEGACY_BEGIN
#define AYTHER_OBSERVER_LEGACY_END
#endif

void normalize_native_sprites(const ayther_sprite_v1 *sprites, uint32_t count,
                              std::vector<uint8_t> &out) {
  out.resize(static_cast<size_t>(count) * kParsedSpriteRecordSize);
  for (uint32_t index = 0; index < count; ++index) {
    const auto bytes = parsed_sprite_le_bytes(sprites[index]);
    std::copy(bytes.begin(), bytes.end(),
              out.begin() +
                  static_cast<size_t>(index) * kParsedSpriteRecordSize);
  }
}

void normalize_legacy_sprites(const uint8_t *bytes, uint32_t count,
                              std::vector<uint8_t> &out) {
  out.resize(static_cast<size_t>(count) * kParsedSpriteRecordSize);
  for (uint32_t index = 0; index < count; ++index) {
    ayther_sprite_v1 native{};
    std::memcpy(&native,
                bytes + static_cast<size_t>(index) * kParsedSpriteRecordSize,
                sizeof(native));
    const auto normalized = parsed_sprite_le_bytes(native);
    std::copy(normalized.begin(), normalized.end(),
              out.begin() +
                  static_cast<size_t>(index) * kParsedSpriteRecordSize);
  }
}

} // namespace

void EmulationObserver::activate_subscriptions(RetroRunner &runner) {
  if (!runner.has_ayther_v1())
    return;
  const ayther_interface_v1 *api = runner.ayther_api();
  if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1))
    return;

  ayther_subscription_state_v1 state{};
  state.struct_size = sizeof(state);
  if (api->get_subscriptions(&state, sizeof(state)) != AYTHER_STATUS_OK) {
    ayther::log::write(ayther::log::Severity::Error, "session",
                       "get_subscriptions_fallo_sin",
                       "get_subscriptions fallo — sin suscripciones");
    return;
  }
  const uint32_t wanted =
      RetroRunner::kEngineSubscriptions & state.supported_mask;
  const int32_t result = api->set_subscriptions(wanted);
  if (result != AYTHER_STATUS_OK) {
    ayther::log::write(ayther::log::Severity::Error, "session",
                       "set_subscriptions_fallo", "set_subscriptions fallo: %d",
                       result);
    return;
  }
  requested_ = wanted;
  subscriptions_verified_ = false;
  ayther::log::write(ayther::log::Severity::Info, "session",
                     "suscripciones_ayther_pedidas_x",
                     "suscripciones AYTHER pedidas: 0x%08X "
                     "(soportadas: 0x%08X)",
                     wanted, state.supported_mask);
}

void EmulationObserver::verify_subscriptions(RetroRunner &runner) {
  if (subscriptions_verified_ || !requested_ || !runner.has_ayther_v1())
    return;
  const ayther_interface_v1 *api = runner.ayther_api();
  if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1))
    return;

  ayther_subscription_state_v1 state{};
  state.struct_size = sizeof(state);
  if (api->get_subscriptions(&state, sizeof(state)) != AYTHER_STATUS_OK)
    return;
  if (state.active_mask == requested_) {
    ayther::log::write(ayther::log::Severity::Info, "session",
                       "suscripciones_ayther_activas_x",
                       "suscripciones AYTHER activas: 0x%08X",
                       state.active_mask);
  } else {
    ayther::log::write(ayther::log::Severity::Warning, "session",
                       "suscripciones_desalineadas_activas_x",
                       "suscripciones DESALINEADAS — activas=0x%08X "
                       "pedidas=0x%08X",
                       state.active_mask, requested_);
  }
  subscriptions_verified_ = true;
}

void EmulationObserver::reapply_subscriptions(RetroRunner &runner) {
  if (!requested_ || !runner.has_ayther_v1())
    return;
  subscriptions_verified_ = false;
  runner.ayther_api()->set_subscriptions(requested_);
}

void EmulationObserver::initialize_system(RetroRunner &runner) {
  if (runner.has_ayther_v1())
    system_ok_ = runner.read_system_v1(system_).ok();
}

bool EmulationObserver::mirror_enabled() {
  static const bool enabled = [] {
    return RuntimeOptions::process().abi_mirror();
  }();
  return enabled;
}

void EmulationObserver::refresh(RetroRunner &runner) {
  snapshot_ok_ = false;
  system_ok_ = false;
  system_current_for_frame_ = !runner.has_ayther_v1();
  vram_.clear();
  cram_.clear();
  regs_.clear();
  vsram_.clear();
  sprites_.clear();
  sprite_bytes_le_.clear();
  legacy_sprite_bytes_le_.clear();
  sprite_count_ = 0;
  sprite_cache_valid_ = false;
  sprite_cache_complete_ = false;
  audio_.clear();
  if (!mirror_enabled() || !runner.has_ayther_v1() ||
      !runner.capture_frame_snapshot(snapshot_).ok()) {
    return;
  }

  system_ok_ = runner.read_system_v1(system_).ok();
  system_current_for_frame_ = system_ok_;
  if (system_ok_ && !system_logged_ && system_.vdp_mode != 0) {
    system_logged_ = true;
    ayther::log::write(
        ayther::log::Severity::Info, "session", "system_hw_x_vdp",
        "SYSTEM: hw=0x%02X vdp_mode=%u h40=%u interlace=%u "
        "sh=%u %s lines=%u viewport=%ux%u@(%u,%u) geometry_pending=%u",
        system_.system_hw, system_.vdp_mode, system_.h40, system_.interlace,
        system_.shadow_highlight, system_.region_pal ? "PAL" : "NTSC",
        system_.lines_per_frame, system_.viewport_w, system_.viewport_h,
        system_.viewport_x, system_.viewport_y,
        static_cast<unsigned>(system_.flags & AYTHER_SYSTEM_GEOMETRY_PENDING));
  }

  auto read_region = [&](std::vector<uint8_t> &destination, size_t legacy_size,
                         uint32_t region,
                         RetroRunner::AytherReadResult (RetroRunner::*read)(
                             void *, const ayther_frame_snapshot_v1 &) const) {
    const size_t abi_size = runner.abi_region_bytes(region);
    const size_t size = (std::max)(abi_size, legacy_size);
    if (!size) {
      destination.clear();
      return;
    }
    destination.resize(size);
    const auto result = (runner.*read)(destination.data(), snapshot_);
    if (!result.ok() || (result.count && result.count < size))
      destination.clear();
  };
  read_region(vram_, runner.video_ram_size(), AYTHER_REGION_VRAM,
              &RetroRunner::read_vram_v1);
  read_region(cram_, runner.color_ram_size(), AYTHER_REGION_CRAM,
              &RetroRunner::read_cram_v1);
  read_region(regs_, runner.vdp_regs_size(), AYTHER_REGION_VDP_REGS,
              &RetroRunner::read_vdp_regs_v1);
  read_region(vsram_, runner.vsram_size(), AYTHER_REGION_VSRAM,
              &RetroRunner::read_vsram_v1);
  snapshot_ok_ = true;
}

AYTHER_OBSERVER_LEGACY_BEGIN
const uint8_t *EmulationObserver::vram(const RetroRunner &runner) const {
  return !vram_.empty() ? vram_.data() : runner.video_ram();
}

const uint8_t *EmulationObserver::cram(const RetroRunner &runner) const {
  return !cram_.empty() ? cram_.data() : runner.color_ram();
}

const uint8_t *EmulationObserver::regs(const RetroRunner &runner) const {
  return !regs_.empty() ? regs_.data() : runner.vdp_regs();
}

const uint8_t *EmulationObserver::vsram(const RetroRunner &runner) const {
  return !vsram_.empty() ? vsram_.data() : runner.vsram();
}
AYTHER_OBSERVER_LEGACY_END

EmulationObserver::AudioWritesView
EmulationObserver::audio_writes(RetroRunner &runner) {
  if (!snapshot_ok_) {
    AYTHER_OBSERVER_LEGACY_BEGIN
    return {runner.audio_writes(), runner.audio_write_count(), false};
    AYTHER_OBSERVER_LEGACY_END
  }
  return audio_writes(runner, snapshot_, true);
}

EmulationObserver::AudioWritesView
EmulationObserver::audio_writes(RetroRunner &runner,
                                const ayther_frame_snapshot_v1 &snapshot,
                                bool legacy_fallback) {
  std::optional<RetroRunner::AytherReadResult> abi_read;
  if (runner.has_ayther_v1()) {
    audio_.resize(snapshot.audio_write_count);
    const auto result = runner.read_audio_writes_v1(
        audio_.data(), static_cast<uint32_t>(audio_.size()), snapshot);
    abi_read = result;
    if (result.ok()) {
      return {reinterpret_cast<const RetroRunner::AudioWrite *>(audio_.data()),
              result.count, true, result};
    }
    if (result.status == AYTHER_STATUS_NOT_SUBSCRIBED && !audio_warned_) {
      audio_warned_ = true;
      ayther::log::write(ayther::log::Severity::Warning, "session",
                         "audio_writes_sin_suscripcion",
                         "AUDIO_WRITES sin suscripcion — "
                         "las escrituras siguen por el camino legacy");
    }
    if (!legacy_fallback)
      return {nullptr, 0, false, abi_read};
  }
  AYTHER_OBSERVER_LEGACY_BEGIN
  return {runner.audio_writes(), runner.audio_write_count(), false, abi_read};
  AYTHER_OBSERVER_LEGACY_END
}

EmulationObserver::ParsedSpritesView
EmulationObserver::parsed_sprites(RetroRunner &runner) {
  if (snapshot_ok_) {
    if (!parsed_sprite_capture_complete(snapshot_)) {
      sprites_.clear();
      sprite_bytes_le_.clear();
      sprite_count_ = 0;
      sprite_cache_valid_ = true;
      sprite_cache_complete_ = false;
      if (!sprites_overflow_warned_) {
        sprites_overflow_warned_ = true;
        ayther::log::write(
            ayther::log::Severity::Warning, "session",
            "parsed_sprites_capture_overflow",
            "PARSED_SPRITES capture overflowed; the truncated prefix is "
            "discarded and HD composition is disabled for the frame");
      }
      return {nullptr, 0, true, false};
    }
    sprites_.resize(snapshot_.parsed_sprite_count);
    const auto result = runner.read_parsed_sprites_v1(
        sprites_.data(), static_cast<uint32_t>(sprites_.size()), snapshot_);
    if (result.ok()) {
      sprite_count_ = result.count;
      normalize_native_sprites(sprites_.data(), result.count, sprite_bytes_le_);
      sprite_cache_valid_ = true;
      sprite_cache_complete_ = true;
      return {sprite_bytes_le_.empty() ? nullptr : sprite_bytes_le_.data(),
              result.count, true, true};
    }
    if (!sprites_warned_) {
      sprites_warned_ = true;
      ayther::log::write(
          ayther::log::Severity::Warning, "session",
          "sprite_capture_lectura_fallo",
          "PARSED_SPRITES ABI fallo (status=%d) — el frame queda "
          "incompleto sin usar memoria legacy",
          result.status);
    }
    sprites_.clear();
    sprite_bytes_le_.clear();
    sprite_count_ = 0;
    sprite_cache_valid_ = true;
    sprite_cache_complete_ = false;
    return {nullptr, 0, true, false};
  }
  AYTHER_OBSERVER_LEGACY_BEGIN
  const uint8_t *legacy = runner.parsed_sprites();
  const uint32_t legacy_count = runner.parsed_sprite_count();
  AYTHER_OBSERVER_LEGACY_END
  if (legacy == nullptr || legacy_count == 0) {
    legacy_sprite_bytes_le_.clear();
    return {nullptr, 0, false, true};
  }
  normalize_legacy_sprites(legacy, legacy_count, legacy_sprite_bytes_le_);
  return {legacy_sprite_bytes_le_.data(), legacy_count, false, true};
}

std::optional<EmulationObserver::ParsedSpritesView>
EmulationObserver::cached_parsed_sprites() const noexcept {
  if (!snapshot_ok_ || !sprite_cache_valid_)
    return std::nullopt;
  return ParsedSpritesView{sprite_count_ ? sprite_bytes_le_.data() : nullptr,
                           sprite_count_, true, sprite_cache_complete_};
}

EmulationObserver::ParsedSpritesView
EmulationObserver::cached_or_legacy_parsed_sprites(
    const RetroRunner &runner) const {
  const auto cached = cached_parsed_sprites();
  ParsedSpritesView legacy{};
  if (!cached) {
    AYTHER_OBSERVER_LEGACY_BEGIN
    const uint8_t *data = runner.parsed_sprites();
    const uint32_t count = runner.parsed_sprite_count();
    AYTHER_OBSERVER_LEGACY_END
    if (data != nullptr && count != 0) {
      normalize_legacy_sprites(data, count, legacy_sprite_bytes_le_);
      legacy = {legacy_sprite_bytes_le_.data(), count, false, true};
    }
  }
  return prefer_snapshot_parsed_sprites(cached, legacy);
}

bool EmulationObserver::mark_raster_overflow_logged() noexcept {
  if (raster_overflow_logged_)
    return false;
  raster_overflow_logged_ = true;
  return true;
}

bool EmulationObserver::mark_raster_unsupported_logged() noexcept {
  if (raster_unsupported_logged_)
    return false;
  raster_unsupported_logged_ = true;
  return true;
}

} // namespace ayther::session
