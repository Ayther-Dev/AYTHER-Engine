#pragma once
// ObservationSource: the one surface the Engine reads an emulator through
// (spec 008, plan §1.4, TP-301.1). Private to the Engine: not in
// cmake/AytherPublicHeaders.txt.
//
// The primitives mirror `ayther_interface_v1` (ABI 1.10), which is also the
// shape of Tessera API 1.1 (the same calls with the session as first
// parameter). RetroRunner provides them from the core's descriptor;
// TesseraBackend provides them from a native session. Everything the Engine
// builds from them —snapshot reads validated by generation, forward-compatible
// sprite reads, the audio poll policy, controls— lives once, in this class,
// so a FrameView built from either provider goes through the same code (D-3
// by construction).
//
// The legacy accessors at the bottom are the deprecated memory ids of a stock
// libretro core. A source without them returns null and zero; only a core
// without the observation surface ever needs them.

#include "../libretro_host/ayther_api.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ayther {

/// The functions of the observation surface. A source answers `provides()`
/// for each one; calling a function it does not provide returns
/// AYTHER_STATUS_UNSUPPORTED.
enum class ObservationFn : std::uint8_t {
  query_region,
  read_region,
  write_control,
  capture_snapshot,
  recompose_frame,
  poll_audio_events,
  get_audio_transport_stats,
  get_subscriptions,
  set_subscriptions,
  poll_frame_delta,
  get_recompose_stats,
  recompose_multilayer,
  frame_delta_since,
};

/// Descriptor facts of the surface: what `ayther_interface_v1` carries besides
/// its functions. The sizes are the ones the provider declares, not this
/// Engine's sizeof.
struct ObservationInfo {
  std::uint32_t abi_version = 0;
  std::uint64_t capabilities = 0;
  std::string_view build_id; ///< not NUL-terminated
  std::uint32_t region_info_size = 0;
  std::uint32_t frame_snapshot_size = 0;
  std::uint32_t sprite_size = 0;
  std::uint32_t audio_write_size = 0;
  std::uint32_t audio_event_size = 0;
  std::uint32_t audio_transport_stats_size = 0;
  std::uint32_t subscription_state_size = 0;
  std::uint32_t frame_delta_size = 0;
  std::uint32_t recompose_stats_size = 0;
};

class ObservationSource {
public:
  virtual ~ObservationSource() = default;

  // --- Primitives -----------------------------------------------------------
  /// Null without an observation surface (a stock libretro core): every
  /// primitive then returns AYTHER_STATUS_UNSUPPORTED and the caller follows
  /// the legacy path.
  [[nodiscard]] virtual const ObservationInfo *observation_info() const = 0;
  [[nodiscard]] virtual bool provides(ObservationFn fn) const = 0;

  virtual std::int32_t query_region(std::uint32_t region_id,
                                    ayther_region_info_v1 *out,
                                    std::uint32_t out_size) const = 0;
  virtual std::int32_t read_region(std::uint32_t region_id,
                                   std::uint32_t offset, void *out,
                                   std::uint32_t byte_count,
                                   std::uint64_t expected_generation,
                                   std::uint64_t *actual_generation) const = 0;
  virtual std::int32_t write_control(std::uint32_t region_id,
                                     std::uint32_t offset, const void *data,
                                     std::uint32_t byte_count,
                                     std::uint64_t expected_generation,
                                     std::uint64_t *new_generation) const = 0;
  virtual std::int32_t capture_snapshot(ayther_frame_snapshot_v1 *out,
                                        std::uint32_t out_size) const = 0;
  virtual std::int32_t recompose_frame(std::uint16_t *out_pixels,
                                       std::uint32_t pixel_capacity,
                                       std::uint32_t flags,
                                       std::uint32_t *out_width,
                                       std::uint32_t *out_height) const = 0;
  virtual std::int32_t
  poll_audio_events(ayther_audio_event_v1 *out, std::uint32_t event_capacity,
                    std::uint32_t *out_event_count) const = 0;
  virtual std::int32_t
  get_audio_transport_stats(ayther_audio_transport_stats_v1 *out,
                            std::uint32_t out_size) const = 0;
  virtual std::int32_t get_subscriptions(ayther_subscription_state_v1 *out,
                                         std::uint32_t out_size) const = 0;
  virtual std::int32_t
  set_subscriptions(std::uint32_t requested_mask) const = 0;
  virtual std::int32_t poll_frame_delta(ayther_frame_delta_v1 *out,
                                        std::uint32_t out_size) const = 0;
  virtual std::int32_t get_recompose_stats(ayther_recompose_stats_v1 *out,
                                           std::uint32_t out_size) const = 0;
  virtual std::int32_t
  recompose_multilayer(std::uint16_t *out_bg_a, std::uint16_t *out_bg_b,
                       std::uint16_t *out_window, std::uint16_t *out_sprites,
                       std::uint16_t *out_composite,
                       std::uint32_t pixel_capacity, std::uint32_t flags,
                       std::uint32_t *out_width,
                       std::uint32_t *out_height) const = 0;
  virtual std::int32_t frame_delta_since(std::uint64_t generation_from,
                                         ayther_frame_delta_v1 *out,
                                         std::uint32_t out_size) const = 0;

  // --- Descriptor shorthands ------------------------------------------------
  /// True if the emulator has the observation surface.
  [[nodiscard]] bool has_ayther_v1() const {
    return observation_info() != nullptr;
  }
  /// The ABI version the provider declares (0 without the surface). The rule
  /// is major == 1 and minor >= whatever is needed, never `==`.
  [[nodiscard]] std::uint32_t ayther_abi_version() const {
    const ObservationInfo *i = observation_info();
    return i ? i->abi_version : 0u;
  }
  [[nodiscard]] std::uint64_t observation_capabilities() const {
    const ObservationInfo *i = observation_info();
    return i ? i->capabilities : 0u;
  }
  [[nodiscard]] bool has_capability(std::uint64_t cap) const {
    return (observation_capabilities() & cap) != 0;
  }
  /// The provider's build id; empty without the surface. Not NUL-terminated.
  [[nodiscard]] std::string_view observation_build_id() const {
    const ObservationInfo *i = observation_info();
    return i ? i->build_id : std::string_view{};
  }

  // --- Subscriptions --------------------------------------------------------
  /// The subscriptions this Engine consumes — and only those (see
  /// docs/EMULATOR_EXTENSION_ABI.md#subscriptions). Every active bit moves
  /// the core renderer to the observed clone and pays for what it captures,
  /// so a bit is added here together with whoever reads its region.
  static constexpr std::uint32_t kEngineSubscriptions =
      AYTHER_SUB_VDP_MEMORY | AYTHER_SUB_SPRITE_CAPTURE |
      AYTHER_SUB_RENDER_CONTROLS | AYTHER_SUB_RASTER_TRACKING |
      AYTHER_SUB_AUDIO_WRITES | AYTHER_SUB_RECOMPOSITION |
      AYTHER_SUB_AUDIO_EVENTS | AYTHER_SUB_LINE_STATE;
  /// Bits of `fallback_reasons` (snapshot) / 0x10E (legacy). OVERFLOW = the
  /// journal passed 256 events and `recompose_multilayer` returns
  /// RC_JOURNAL_OVERFLOW instead of a plausible prefix; UNSUPPORTED_CONTROLS =
  /// a control we asked for does not apply in this mode.
  static constexpr std::uint32_t kRasterReasonJournalOverflow = UINT32_C(1)
                                                                << 7;
  static constexpr std::uint32_t kRasterReasonUnsupportedControls = UINT32_C(1)
                                                                    << 8;
  /// Asks for what the Engine consumes, bounded by what the provider
  /// supports. Returns the enabled mask (0 without the surface).
  ///
  /// Nothing is instrumented until somebody declares it: without this the
  /// chip write log comes back empty, the audio probe emits nothing and the
  /// mute mask is ignored SILENTLY.
  std::uint32_t subscribe_all_supported() const;

  // --- Reads (validated by the snapshot generation) -------------------------
  struct AytherReadResult {
    std::int32_t status = AYTHER_STATUS_UNSUPPORTED;
    std::uint32_t count = 0;      ///< elements read
    std::uint64_t generation = 0; ///< generation the provider returned
    bool ok() const { return status == AYTHER_STATUS_OK; }
  };
  /// Snapshot of the CURRENT frame. Call it after the frame ran: the surface
  /// resets its counters at the frame boundary.
  AytherReadResult capture_frame_snapshot(ayther_frame_snapshot_v1 &out) const;
  /// `SYSTEM` (ABI 1.5): VDP mode, h40, interlace, S/H, PAL and the viewport
  /// of the emitted frame. No subscription; filled on read. UNSUPPORTED
  /// without the capability or with a layout this Engine does not know.
  AytherReadResult read_system_v1(ayther_system_v1 &out) const;
  /// A whole region into the caller's buffer, validating the generation.
  AytherReadResult read_region_v1(std::uint32_t region, void *out,
                                  std::uint32_t bytes,
                                  std::uint64_t generation) const;
  /// How many bytes the provider says a region measures (0 if there is no
  /// surface or it does not know it). The VDP `read_*_v1` calls write THAT
  /// size.
  std::size_t abi_region_bytes(std::uint32_t region) const;
  AytherReadResult read_vram_v1(void *out,
                                const ayther_frame_snapshot_v1 &s) const;
  AytherReadResult read_cram_v1(void *out,
                                const ayther_frame_snapshot_v1 &s) const;
  AytherReadResult read_vdp_regs_v1(void *out,
                                    const ayther_frame_snapshot_v1 &s) const;
  AytherReadResult read_vsram_v1(void *out,
                                 const ayther_frame_snapshot_v1 &s) const;
  /// Parsed sprites (AYTHER_SUB_SPRITE_CAPTURE), forward compatible: an
  /// `element_size` larger than the struct this Engine knows is read with the
  /// provider's stride and only the known fields are copied.
  AytherReadResult
  read_parsed_sprites_v1(ayther_sprite_v1 *out, std::uint32_t max,
                         const ayther_frame_snapshot_v1 &s) const;
  /// The frame's chip writes (AYTHER_SUB_AUDIO_WRITES).
  AytherReadResult
  read_audio_writes_v1(ayther_audio_write_v1 *out, std::uint32_t max,
                       const ayther_frame_snapshot_v1 &s) const;
  /// Raster fallback reasons; 0 without the surface.
  std::uint32_t
  read_raster_fallback_v1(const ayther_frame_snapshot_v1 &s) const;

  // --- Typed audio events ---------------------------------------------------
  // CONSUME-ON-POLL over an SPSC queue: a single consumer, once per frame.
  struct AudioPollObservation {
    enum class Outcome {
      no_api,
      invalid_output,
      unsupported,
      layout_mismatch,
      poll_error,
      polled
    };
    Outcome outcome = Outcome::no_api;
    bool stats_attempted = false;
    bool poll_attempted = false;
    std::int32_t stats_status = AYTHER_STATUS_UNSUPPORTED;
    std::int32_t poll_status = AYTHER_STATUS_UNSUPPORTED;
    ayther_audio_transport_stats_v1 stats{};
    std::uint32_t count = 0;
  };
  /// The poll policy with its branch results retained: a failed stats query
  /// does not block polling, a layout mismatch does. No extra poll,
  /// subscription change or transport reset.
  AudioPollObservation read_audio_poll(ayther_audio_event_v1 *out,
                                       std::uint32_t capacity) const;
  /// Number of events written into `out` (0 without the surface, without a
  /// subscription, or with nothing pending).
  std::uint32_t poll_audio_events_v1(ayther_audio_event_v1 *out,
                                     std::uint32_t max) const;
  /// Same consuming operation and return value; optionally retains the read
  /// result and the transport snapshot that operation queried.
  std::uint32_t
  poll_audio_events_observed_v1(ayther_audio_event_v1 *out, std::uint32_t max,
                                AudioPollObservation *observation) const;
  /// Events the transport dropped for lack of polling, accumulated.
  std::uint32_t audio_events_dropped() const;

  struct SubscriptionObservation {
    bool attempted = false;
    std::int32_t status = AYTHER_STATUS_UNSUPPORTED;
    ayther_subscription_state_v1 state{};
  };
  /// Read-only subscription state. An unavailable or failed query is not an
  /// active source; nothing is requested here.
  SubscriptionObservation read_subscriptions() const;

  // --- Controls (between frames) --------------------------------------------
  struct AytherWriteResult {
    std::int32_t status = AYTHER_STATUS_UNSUPPORTED;
    std::uint64_t new_generation = 0;
    bool ok() const { return status == AYTHER_STATUS_OK; }
  };
  /// A write into a CONTROL region. `AYTHER_GENERATION_ANY` skips generation
  /// validation, which is right for a control set outside a snapshot.
  AytherWriteResult write_control_v1(
      std::uint32_t region, const void *data, std::uint32_t bytes,
      std::uint64_t expected_generation = AYTHER_GENERATION_ANY) const;

  static constexpr std::uint32_t kSpriteSuppressBytes = 16; // 128 SAT slots
  static constexpr std::uint32_t kTileSuppressBytes = 512;  // 64×64 cells
  static constexpr std::uint32_t kPlaneTileSuppressBytes = 3 * 1024; // 3 planes
  /// Visible layer mask: 1 byte with the A/B/Window/Sprites bits, read per
  /// line by the renderer.
  AytherWriteResult set_layer_mask_v1(std::uint8_t mask) const {
    return write_control_v1(AYTHER_REGION_LAYER_MASK, &mask, 1);
  }
  AytherWriteResult set_layer_dim_v1(std::uint8_t on) const {
    return write_control_v1(AYTHER_REGION_LAYER_DIM, &on, 1);
  }
  AytherWriteResult set_sprite_suppress_v1(const std::uint8_t *bits16) const {
    return write_control_v1(AYTHER_REGION_SPRITE_SUPPRESS, bits16,
                            kSpriteSuppressBytes);
  }
  AytherWriteResult set_tile_suppress_v1(const std::uint8_t *bits,
                                         std::uint32_t n) const {
    return write_control_v1(AYTHER_REGION_TILE_SUPPRESS, bits, n);
  }
  AytherWriteResult set_plane_tile_suppress_v1(const std::uint8_t *bits,
                                               std::uint32_t n) const {
    return write_control_v1(AYTHER_REGION_PLANE_TILE_SUPPRESS, bits, n);
  }
  static constexpr std::uint32_t audio_mute_fm(int ch) {
    return std::uint32_t(1u << ch);
  }
  static constexpr std::uint32_t audio_mute_psg(int ch) {
    return std::uint32_t(1u << (6 + ch));
  }
  static constexpr std::uint32_t audio_mute_pcm(int ch) {
    return std::uint32_t(1u << (10 + ch));
  }
  AytherWriteResult set_audio_mute_v1(std::uint32_t mask) const {
    return write_control_v1(AYTHER_REGION_AUDIO_MUTE, &mask, sizeof(mask));
  }

  // --- Legacy memory (stock libretro core only) -----------------------------
  // The deprecated memory ids. A source with the observation surface never
  // needs them and the defaults say "absent".
  [[nodiscard]] virtual const std::uint8_t *video_ram() const {
    return nullptr;
  }
  [[nodiscard]] virtual std::size_t video_ram_size() const { return 0; }
  [[nodiscard]] virtual const std::uint8_t *color_ram() const {
    return nullptr;
  }
  [[nodiscard]] virtual std::size_t color_ram_size() const { return 0; }
  [[nodiscard]] virtual const std::uint8_t *vdp_regs() const { return nullptr; }
  [[nodiscard]] virtual std::size_t vdp_regs_size() const { return 0; }
  [[nodiscard]] virtual const std::uint8_t *vsram() const { return nullptr; }
  [[nodiscard]] virtual std::size_t vsram_size() const { return 0; }
  [[nodiscard]] virtual std::uint32_t raster_dirty() const { return 0; }
  [[nodiscard]] virtual const std::uint8_t *parsed_sprites() const {
    return nullptr;
  }
  [[nodiscard]] virtual std::uint8_t parsed_sprite_count() const { return 0; }
  /// Same layout as `ayther_audio_write_v1` (cycle u32, addr u16, data u8,
  /// chip u8).
  struct AudioWrite {
    std::uint32_t cycle;
    std::uint16_t addr;
    std::uint8_t data;
    std::uint8_t chip;
  };
  static constexpr std::uint8_t kAudioChipFM = 0;  // YM2612
  static constexpr std::uint8_t kAudioChipPSG = 1; // SN76489
  [[nodiscard]] virtual const AudioWrite *audio_writes() const {
    return nullptr;
  }
  [[nodiscard]] virtual std::uint32_t audio_write_count() const { return 0; }
};

/// An ObservationSource over a borrowed `ayther_interface_v1` descriptor:
/// the libretro fork's table, or a test's. Null means no surface.
class InterfaceObservationSource : public ObservationSource {
public:
  InterfaceObservationSource() = default;
  explicit InterfaceObservationSource(const ayther_interface_v1 *api) {
    bind_interface(api);
  }

  [[nodiscard]] const ObservationInfo *observation_info() const override {
    return api_ ? &info_ : nullptr;
  }
  [[nodiscard]] bool provides(ObservationFn fn) const override;

  std::int32_t query_region(std::uint32_t region_id, ayther_region_info_v1 *out,
                            std::uint32_t out_size) const override;
  std::int32_t read_region(std::uint32_t region_id, std::uint32_t offset,
                           void *out, std::uint32_t byte_count,
                           std::uint64_t expected_generation,
                           std::uint64_t *actual_generation) const override;
  std::int32_t write_control(std::uint32_t region_id, std::uint32_t offset,
                             const void *data, std::uint32_t byte_count,
                             std::uint64_t expected_generation,
                             std::uint64_t *new_generation) const override;
  std::int32_t capture_snapshot(ayther_frame_snapshot_v1 *out,
                                std::uint32_t out_size) const override;
  std::int32_t recompose_frame(std::uint16_t *out_pixels,
                               std::uint32_t pixel_capacity,
                               std::uint32_t flags, std::uint32_t *out_width,
                               std::uint32_t *out_height) const override;
  std::int32_t poll_audio_events(ayther_audio_event_v1 *out,
                                 std::uint32_t event_capacity,
                                 std::uint32_t *out_event_count) const override;
  std::int32_t get_audio_transport_stats(ayther_audio_transport_stats_v1 *out,
                                         std::uint32_t out_size) const override;
  std::int32_t get_subscriptions(ayther_subscription_state_v1 *out,
                                 std::uint32_t out_size) const override;
  std::int32_t set_subscriptions(std::uint32_t requested_mask) const override;
  std::int32_t poll_frame_delta(ayther_frame_delta_v1 *out,
                                std::uint32_t out_size) const override;
  std::int32_t get_recompose_stats(ayther_recompose_stats_v1 *out,
                                   std::uint32_t out_size) const override;
  std::int32_t
  recompose_multilayer(std::uint16_t *out_bg_a, std::uint16_t *out_bg_b,
                       std::uint16_t *out_window, std::uint16_t *out_sprites,
                       std::uint16_t *out_composite,
                       std::uint32_t pixel_capacity, std::uint32_t flags,
                       std::uint32_t *out_width,
                       std::uint32_t *out_height) const override;
  std::int32_t frame_delta_since(std::uint64_t generation_from,
                                 ayther_frame_delta_v1 *out,
                                 std::uint32_t out_size) const override;

  /// The borrowed descriptor; nullptr without the surface.
  const ayther_interface_v1 *ayther_api() const { return api_; }

protected:
  void bind_interface(const ayther_interface_v1 *api);

private:
  const ayther_interface_v1 *api_ = nullptr;
  ObservationInfo info_{};
};

} // namespace ayther
