// parity_bench trace-from-arp (spec 008, TP-005.14, D-7): converts the port-0
// inputs of a take (.arp) into a Tessera TSTRACE1 input trace.
//
// TSTRACE1 carries 8 Mega Drive controls per frame, in this order:
// up, down, left, right, A, B, C, start. The take stores a libretro RetroPad
// bitmask; the mapping is the one the gpgx core uses (A=JOYPAD_Y, B=JOYPAD_B,
// C=JOYPAD_A), the same as Tessera's differential bench.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ayther::parity {

/// RetroPad bits that have a Mega Drive control
/// (B,Y,start,up,down,left,right,A).
inline constexpr std::uint16_t kMappedPadMask = 0x1FB;

std::array<std::int32_t, 8> controls_from_pad(std::uint16_t pad);
std::uint16_t pad_from_controls(const std::array<std::int32_t, 8> &controls);

std::vector<std::uint8_t>
encode_trace(const std::vector<std::uint16_t> &pads,
             const std::array<std::uint8_t, 32> &content,
             const std::array<std::uint8_t, 32> &config);

struct Trace {
  std::string content_sha_hex;
  std::string config_sha_hex;
  std::vector<std::uint16_t>
      pads; ///< RetroPad bitmask per frame (mapped bits only)
};

/// Strict decoder; nullopt on any malformed input.
std::optional<Trace> decode_trace(std::span<const std::uint8_t> bytes);

/// Returns 0 on success, 1 when the take cannot be read, 2 on bad arguments
/// (hashes must be 64 hex digits), 4 when the output cannot be written.
int trace_from_arp_command(const std::string &arp_path,
                           const std::string &out_path,
                           const std::string &content_sha_hex,
                           const std::string &config_sha_hex);

} // namespace ayther::parity
