// parity_bench dump (spec 008, TP-005.15, D-3): the FrameView of every frame
// of a session, reduced to one hash and one element count per subplane.
// One JSON object per line (JSON Lines), keys in a fixed order.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ayther::parity {

class IdentityCollector;

/// Fixed order of the subplanes in every dump line.
inline constexpr const char *kSubplaneNames[] = {
    "fb",          "scene",       "sprites",     "tiles",
    "plane_cells", "raster",      "chip_writes", "chip_seq",
    "palette",     "attribution", "pcm"};

/// Runs the core with the given port-0 inputs (one frame per entry) and
/// returns the dump, or nullopt when the session cannot be created.
/// `ids`, when given, also receives every frame (TP-209.2).
std::optional<std::string> dump_session(const std::string &core_path,
                                        const std::string &rom_path,
                                        const std::vector<std::uint16_t> &pads,
                                        IdentityCollector *ids = nullptr);

/// The same dump from the native megadrive machine (provider tessera_native)
/// with `rom` as its content and 64 KiB of zeroed persistent memory; nullopt
/// when Tessera is unavailable or the session cannot be created.
std::optional<std::string>
dump_native_session(const std::vector<std::uint8_t> &rom,
                    const std::vector<std::uint16_t> &pads,
                    IdentityCollector *ids = nullptr);

} // namespace ayther::parity
