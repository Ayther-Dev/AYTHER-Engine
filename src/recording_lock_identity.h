#pragma once

#include <filesystem>
#include <string>

namespace ayther::recording_detail {

// Stable, process-independent identity used by the recording writer lock.
// Existing files are identified by their filesystem object rather than by the
// caller's path spelling, so Win32 aliases and hard links serialize together.
[[nodiscard]] std::string
destination_lock_identity(const std::filesystem::path &destination);

// Persistent POSIX flock file derived from destination_lock_identity(). It is
// separate from the spelling-specific sidecar: the identity lock joins hard
// links, while the sidecar keeps one pathname serialized across atomic inode
// replacement. Exposed only so the integration test can hold the exact lock.
[[nodiscard]] std::filesystem::path
destination_identity_lock_path(const std::filesystem::path &destination);

} // namespace ayther::recording_detail
