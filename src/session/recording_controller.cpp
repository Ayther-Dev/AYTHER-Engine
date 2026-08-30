#include "session/recording_controller.h"

#include <utility>

namespace ayther::session {

void RecordingController::start(std::vector<uint8_t> initial_state) {
    initial_state_ = std::move(initial_state);
    inputs_.clear();
    stats_.clear();
    sprite_hashes_.clear();
    sprite_offsets_.assign(1, 0);
    audio_hashes_.clear();
    audio_offsets_.assign(1, 0);
    keyframes_.clear();
    active_ = true;
}

void RecordingController::capture_input() {
    if (active_) inputs_.push_back(input_);
}

void RecordingController::capture_frame(const FrameStat& stats,
                                        std::span<const AytherSpriteOccurrence> sprites,
                                        std::span<const AytherAudioOccurrence> audio) {
    if (!active_) return;

    stats_.push_back(stats);
    for (const AytherSpriteOccurrence& sprite : sprites) sprite_hashes_.push_back(sprite.hash);
    sprite_offsets_.push_back(static_cast<uint32_t>(sprite_hashes_.size()));
    for (const AytherAudioOccurrence& occurrence : audio) audio_hashes_.push_back(occurrence.hash);
    audio_offsets_.push_back(static_cast<uint32_t>(audio_hashes_.size()));
}

bool RecordingController::keyframe_due(uint32_t interval) const noexcept {
    return active_ && interval != 0 && frame_count() != 0 && frame_count() % interval == 0;
}

void RecordingController::add_keyframe(uint32_t frame, std::vector<uint8_t> state) {
    if (active_ && !state.empty()) keyframes_.emplace_back(frame, std::move(state));
}

AytherRecording RecordingController::take(std::string game_id) {
    AytherRecording recording;
    recording.game_id = std::move(game_id);
    recording.initial_state = initial_state_;
    recording.inputs = inputs_;
    recording.stats = stats_;
    recording.sprite_hashes = sprite_hashes_;
    recording.hash_offsets = sprite_offsets_;
    recording.audio_hashes = audio_hashes_;
    recording.audio_offsets = audio_offsets_;
    recording.trim_in = 0;
    recording.trim_out = recording.frame_count();
    for (const auto& [frame, state] : keyframes_) recording.add_keyframe(frame, state);

    keyframes_.clear();
    active_ = false;
    return recording;
}

}  // namespace ayther::session
