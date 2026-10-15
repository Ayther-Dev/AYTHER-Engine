#pragma once

#include "ayther_recording.h"
#include "ayther_core_ffi.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ayther::session {

// Owns the live-recording state machine. The session facade supplies already
// observed frame data; this controller does not depend on the emulator host.
class RecordingController {
public:
    void set_input(uint16_t buttons) noexcept { input_ = buttons; }

    /// `fm_regs` is the audio event detector's YM2612 shadow at this same
    /// instant (512 bytes, or empty when there is no detector): the take
    /// carries it so replay and analysis can seed their detectors (.arp v9).
    void start(std::vector<uint8_t> initial_state,
               std::vector<uint8_t> fm_regs = {});
    void stop() noexcept { active_ = false; }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] size_t frame_count() const noexcept { return inputs_.size(); }

    void capture_input();
    void capture_frame(const FrameStat& stats,
                       std::span<const AytherSpriteOccurrence> sprites,
                       std::span<const AytherAudioOccurrence> audio);

    [[nodiscard]] bool keyframe_due(uint32_t interval) const noexcept;
    void add_keyframe(uint32_t frame, std::vector<uint8_t> state);

    AytherRecording take(std::string game_id);

private:
    bool active_ = false;
    uint16_t input_ = 0;
    std::vector<uint8_t> initial_state_;
    std::vector<uint8_t> fm_regs_;
    std::vector<uint16_t> inputs_;
    std::vector<FrameStat> stats_;
    std::vector<uint64_t> sprite_hashes_;
    std::vector<uint32_t> sprite_offsets_;
    std::vector<uint64_t> audio_hashes_;
    std::vector<uint32_t> audio_offsets_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> keyframes_;
};

}  // namespace ayther::session
