#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ayther::engine::audio_observation {

enum class AudioInitializationMode : std::uint8_t { unknown, fresh, restored };

struct AudioInitialVoice {
  std::uint64_t occurrence = 0;
  std::uint64_t business_key = 0;
  std::uint64_t source_position = 0;
  std::uint64_t source_limit = 0;
  std::uint64_t output_start = 0;
  float gain = 1.0F;
  bool looping = false;
  bool event = false;
  std::uint64_t end_frame = 0;
  std::uint64_t cut_frame = 0;
  std::uint64_t loop_begin = 0;
  std::uint64_t loop_end = 0;
  std::uint32_t fade_remaining = 0;
};

enum class AudioInitialWindowKind : std::uint8_t { sequence, live_sequence };

struct AudioInitialWindow {
  AudioInitialWindowKind kind = AudioInitialWindowKind::sequence;
  std::uint64_t key = 0;
  std::uint64_t signature = 0;
  std::uint64_t start_frame = 0;
  std::uint64_t end_frame = 0;
  std::uint32_t channel_mask = 0;
};

enum class AudioInitialPendingKind : std::uint8_t {
  main_staging,
  main_batch,
  main_stream,
  synth_stream,
  auxiliary_submission
};

struct AudioInitialPending {
  AudioInitialPendingKind kind = AudioInitialPendingKind::main_staging;
  std::uint64_t identity = 0;
  std::uint64_t position = 0;
  std::uint64_t frames = 0;
  std::uint32_t sample_rate = 44100;
};

struct AudioInitialRequest {
  std::uint64_t business_key = 0;
  std::string asset;
  std::uint64_t start_frame = 0;
  std::uint64_t end_frame = 0;
  std::uint64_t cut_frame = 0;
  std::uint32_t channel_mask = 0;
  float gain = 1.0F;
  bool looping = false;
  bool sequence_substitution = false;
  std::uint64_t occurrence = 0;
};

enum class AudioInitialFiredRequestKind : std::uint8_t { sequence, event };

struct AudioInitialFiredRequest {
  AudioInitialFiredRequestKind kind = AudioInitialFiredRequestKind::sequence;
  std::uint64_t business_key = 0;
  std::uint32_t start_frame_plus_one = 0;
};

struct AudioInitialSnapshot {
  AudioInitializationMode initialization = AudioInitializationMode::unknown;
  std::uint64_t emulation_frame = 0;
  std::vector<AudioInitialVoice> voices;
  std::vector<AudioInitialWindow> windows;
  std::vector<AudioInitialRequest> requests;
  std::vector<AudioInitialFiredRequest> fired_requests;
  std::vector<AudioInitialPending> pending_audio;
  bool complete = true;
};

} // namespace ayther::engine::audio_observation
