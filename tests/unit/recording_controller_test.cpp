#include "session/recording_controller.h"

#include <array>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) ++failures;
}

}  // namespace

int main() {
    using ayther::FrameStat;
    using ayther::session::RecordingController;

    RecordingController controller;
    controller.capture_input();
    check(controller.frame_count() == 0, "an idle controller ignores frame input");

    controller.start({1, 2, 3, 4});
    controller.set_input(0x1234);
    controller.capture_input();

    std::array<AytherSpriteOccurrence, 2> sprites{};
    sprites[0].hash = 11;
    sprites[1].hash = 22;
    std::array<AytherAudioOccurrence, 1> audio{};
    audio[0].hash = 33;
    controller.capture_frame(FrameStat{2, 7, 1, 8, 9, 10}, sprites, audio);

    controller.set_input(0x5678);
    controller.capture_input();
    controller.capture_frame(FrameStat{0, 1, 0, 2, 3, 4}, {}, {});
    check(controller.keyframe_due(2), "keyframe cadence is derived from captured inputs");
    controller.add_keyframe(2, {9, 8, 7});

    ayther::AytherRecording recording = controller.take("game-id");
    check(!controller.active(), "take closes the recording state machine");
    check(controller.frame_count() == 2,
          "take preserves the completed take's frame count until the next start");
    check(recording.game_id == "game-id" && recording.initial_state.size() == 4,
          "take transfers identity and initial state");
    check(recording.inputs == std::vector<uint16_t>({0x1234, 0x5678}),
          "input order is preserved");
    check(recording.stats.size() == 2 && recording.stats[0].plane_w == 10,
          "per-frame statistics are preserved");
    check(recording.sprite_hashes == std::vector<uint64_t>({11, 22}) &&
              recording.hash_offsets == std::vector<uint32_t>({0, 2, 2}),
          "sprite history uses valid CSR boundaries");
    check(recording.audio_hashes == std::vector<uint64_t>({33}) &&
              recording.audio_offsets == std::vector<uint32_t>({0, 1, 1}),
          "audio history uses valid CSR boundaries");
    check(recording.trim_out == 2 && recording.keyframes.size() == 1,
          "take finalizes trim and baked keyframes");

    controller.start({5});
    check(controller.frame_count() == 0, "starting a new take resets prior frame state");
    controller.stop();
    controller.capture_input();
    check(controller.frame_count() == 0, "a stopped controller ignores new input");

    std::printf("recording_controller_test: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
