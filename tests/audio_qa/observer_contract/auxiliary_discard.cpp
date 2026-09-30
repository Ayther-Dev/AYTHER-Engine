#include <ayther/audio_player.h>

#include <array>
#include <atomic>

namespace {

struct Capture {
  std::atomic<std::size_t> count{0};
  AudioPlayer::AuxiliaryLoss loss{};

  static void output(void *, const AudioPlayer::MainOutputBlock &) noexcept {}

  static void receive_loss(void *value,
                           const AudioPlayer::AuxiliaryLoss &loss) noexcept {
    auto &capture = *static_cast<Capture *>(value);
    capture.loss = loss;
    capture.count.fetch_add(1, std::memory_order_release);
  }
};

} // namespace

int main() try {
  constexpr std::size_t frames = 16384;
  std::array<float, frames * 2> pcm{};
  Capture capture;
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}) ||
      !player.set_main_output_observer(&capture, Capture::output))
    return 2;
  player.set_auxiliary_loss_observer(&capture, Capture::receive_loss);
  player.feed_synth(pcm.data(), frames);
  player.clear_synth();
  player.clear_synth();
  (void)player.set_main_output_observer(nullptr, nullptr);

  return capture.count.load(std::memory_order_acquire) == 1 &&
                 capture.loss.reason ==
                     AudioPlayer::AuxiliaryLossReason::discarded &&
                 capture.loss.input_begin < capture.loss.input_end &&
                 capture.loss.input_end == frames
             ? 0
             : 1;
} catch (...) {
  return 3;
}
