// ---------------------------------------------------------------------------
// ayther_recording.cpp — .arp save/load. See ayther_recording.h.
//
// File layout (all integers little-endian):
//   magic[4]            "ARP1"
//   u32 version         = 1
//   u32 game_id_len     + game_id bytes (UTF-8)
//   u32 name_len        + name bytes (UTF-8)
//   u32 frame_count
//   u32 raw_state_size  (uncompressed initial savestate size)
//   u32 comp_state_size + zstd blob
//   frame_count × u16   input stream
//   --- v2 (R7b) ---
//   u32 trim_in
//   u32 trim_out
//   frame_count × (u16 sprites, u16 tiles, u16 audio     occurrence summary
//                  [, u16 plane_a, u16 plane_b]  — v5+
//                  [, u16 plane_w])              — v6+ (Window)
//   --- v3 (R7c) ---
//   u32 total_hashes
//   total_hashes × u64                                  flat sprite-hash
//   history frame_count × u32                                   per-frame hash
//   counts (CSR)
//   --- v4 (R7e) ---
//   u32 kf_count
//   kf_count × (u32 frame, u32 raw_size, u32 comp_size, comp blob)   baked
//   keyframes
//   --- v7 (audio por sonido) ---
//   u32 total_audio_hashes
//   total_audio_hashes × u64                            flat audio-hash history
//   frame_count × u32                                   per-frame audio counts
//   (CSR)
//   --- v8 (algo de hash de sprites) ---
//   u32 hash_algo        (kSpriteHashAlgo con que se capturó la historia; <v8 =
//   0)
// ---------------------------------------------------------------------------
#include "ayther_recording.h"
#include "log.h"
#include "recording_lock_identity.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/xattr.h>
#endif
#endif

namespace ayther {

namespace recording_detail {

namespace {

#if !defined(_WIN32)
std::uint64_t stable_path_hash(const std::string &value) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string normalized_path_identity(const std::filesystem::path &destination) {
  std::error_code error;
  std::filesystem::path normalized =
      std::filesystem::weakly_canonical(destination, error);
  if (error) {
    error.clear();
    normalized = std::filesystem::absolute(destination, error);
    if (error)
      normalized = destination;
    normalized = normalized.lexically_normal();
  }

  const std::uint64_t hash = stable_path_hash(normalized.generic_string());
  char identity[40]{};
  (void)std::snprintf(identity, sizeof(identity), "path-%016llX",
                      static_cast<unsigned long long>(hash));
  return identity;
}
#endif

} // namespace

std::string
destination_lock_identity(const std::filesystem::path &destination) {
#if defined(_WIN32)
  const HANDLE file = ::CreateFileW(
      destination.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    BY_HANDLE_FILE_INFORMATION information{};
    const bool identified =
        ::GetFileInformationByHandle(file, &information) != FALSE;
    (void)::CloseHandle(file);
    if (identified) {
      char identity[96]{};
      (void)std::snprintf(
          identity, sizeof(identity), "file-%08lX-%08lX%08lX",
          static_cast<unsigned long>(information.dwVolumeSerialNumber),
          static_cast<unsigned long>(information.nFileIndexHigh),
          static_cast<unsigned long>(information.nFileIndexLow));
      return identity;
    }
  }

  const DWORD required =
      ::GetFullPathNameW(destination.c_str(), 0, nullptr, nullptr);
  std::wstring absolute;
  if (required != 0U) {
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
    const DWORD written = ::GetFullPathNameW(destination.c_str(),
                                             static_cast<DWORD>(buffer.size()),
                                             buffer.data(), nullptr);
    if (written != 0U && written < buffer.size())
      absolute.assign(buffer.data(), written);
  }
  if (absolute.empty())
    absolute = destination.native();

  // Win32 normalizes case and ignores trailing dots/spaces in path
  // components. Mirror those rules for a destination that does not exist yet.
  std::wstring normalized;
  std::wstring component;
  const auto append_component = [&] {
    while (!component.empty() &&
           (component.back() == L'.' || component.back() == L' '))
      component.pop_back();
    normalized.append(component);
    component.clear();
  };
  for (wchar_t character : absolute) {
    if (character == L'\\' || character == L'/') {
      append_component();
      normalized.push_back(L'\\');
    } else {
      component.push_back(static_cast<wchar_t>(std::towlower(character)));
    }
  }
  append_component();

  std::uint64_t hash = 1469598103934665603ULL;
  for (wchar_t character : normalized) {
    hash ^= static_cast<std::uint16_t>(character);
    hash *= 1099511628211ULL;
  }
  char identity[40]{};
  (void)std::snprintf(identity, sizeof(identity), "path-%016llX",
                      static_cast<unsigned long long>(hash));
  return identity;
#else
  struct stat information {};
  if (::lstat(destination.c_str(), &information) == 0 &&
      S_ISREG(information.st_mode)) {
    char identity[96]{};
    (void)std::snprintf(identity, sizeof(identity), "file-%llX-%llX",
                        static_cast<unsigned long long>(information.st_dev),
                        static_cast<unsigned long long>(information.st_ino));
    return identity;
  }
  return normalized_path_identity(destination);
#endif
}

std::filesystem::path
destination_identity_lock_path(const std::filesystem::path &destination) {
  std::error_code error;
#if defined(_WIN32)
  std::filesystem::path root = std::filesystem::temp_directory_path(error);
  if (error)
    return {};
  root /= "AytherRecordingLocks";
  (void)std::filesystem::create_directory(root, error);
  if (error)
    return {};
#else
  std::filesystem::path root =
      "/tmp/ayther-recording-locks-" +
      std::to_string(static_cast<unsigned long long>(::geteuid()));
  if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST)
    return {};
#endif
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(root, error);
  if (error || status.type() != std::filesystem::file_type::directory)
    return {};

#if !defined(_WIN32)
  int flags = O_RDONLY;
#if defined(O_DIRECTORY)
  flags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
  flags |= O_NOFOLLOW;
#endif
  const int directory = ::open(root.c_str(), flags);
  struct stat information {};
  const bool trusted =
      directory >= 0 && ::fstat(directory, &information) == 0 &&
      S_ISDIR(information.st_mode) && information.st_uid == ::geteuid() &&
      ::fchmod(directory, 0700) == 0;
  if (directory >= 0)
    (void)::close(directory);
  if (!trusted)
    return {};
#endif

  std::filesystem::path lock = root;
  lock /= destination_lock_identity(destination) + ".lock";
  return lock;
}

} // namespace recording_detail

namespace {

constexpr char kMagic[4] = {'A', 'R', 'P', '1'};
constexpr uint32_t kVersion =
    8; // v8: + hash_algo (algoritmo de la historia de sprites)
constexpr int kZstdLevel = 9; // takes are saved rarely — favour ratio
constexpr std::size_t kMaxRecordingBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxInitialStateBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaxRecordingFrames = 54'000U;

class RecordingCursor {
public:
  explicit RecordingCursor(const std::vector<char> &bytes) : bytes_(bytes) {}

  [[nodiscard]] std::size_t remaining() const {
    return bytes_.size() - offset_;
  }

  [[nodiscard]] bool empty() const { return offset_ == bytes_.size(); }

  bool read_bytes(void *destination, std::size_t count) {
    if (count > remaining())
      return false;
    if (count != 0)
      std::memcpy(destination, bytes_.data() + offset_, count);
    offset_ += count;
    return true;
  }

  bool read_string(std::string &value) {
    std::uint32_t length = 0;
    if (!read_u32(length) || length > remaining())
      return false;
    value.assign(bytes_.data() + offset_, length);
    offset_ += length;
    return true;
  }

  bool read_u16(std::uint16_t &value) {
    std::uint8_t bytes[2]{};
    if (!read_bytes(bytes, sizeof(bytes)))
      return false;
    value = std::uint16_t(bytes[0]) | (std::uint16_t(bytes[1]) << 8U);
    return true;
  }

  bool read_u32(std::uint32_t &value) {
    std::uint8_t bytes[4]{};
    if (!read_bytes(bytes, sizeof(bytes)))
      return false;
    value = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8U) |
            (std::uint32_t(bytes[2]) << 16U) | (std::uint32_t(bytes[3]) << 24U);
    return true;
  }

  bool read_u64(std::uint64_t &value) {
    std::uint8_t bytes[8]{};
    if (!read_bytes(bytes, sizeof(bytes)))
      return false;
    value = 0;
    for (unsigned int shift = 0; shift < 64U; shift += 8U)
      value |= std::uint64_t(bytes[shift / 8U]) << shift;
    return true;
  }

private:
  const std::vector<char> &bytes_;
  std::size_t offset_ = 0;
};

bool read_hash_history(RecordingCursor &cursor, std::uint32_t frames,
                       std::vector<std::uint64_t> &hashes,
                       std::vector<std::uint32_t> &offsets) {
  std::uint32_t total = 0;
  if (!cursor.read_u32(total) ||
      static_cast<std::uint64_t>(total) * sizeof(std::uint64_t) >
          cursor.remaining())
    return false;

  hashes.resize(total);
  for (std::uint64_t &hash : hashes) {
    if (!cursor.read_u64(hash))
      return false;
  }

  if (static_cast<std::uint64_t>(frames) * sizeof(std::uint32_t) >
      cursor.remaining())
    return false;
  offsets.assign(static_cast<std::size_t>(frames) + 1U, 0U);
  std::uint32_t current = 0;
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    std::uint32_t count = 0;
    if (!cursor.read_u32(count) || count > total - current)
      return false;
    current += count;
    offsets[static_cast<std::size_t>(frame) + 1U] = current;
  }
  return current == total;
}

bool valid_hash_history(std::size_t frames,
                        const std::vector<std::uint64_t> &hashes,
                        const std::vector<std::uint32_t> &offsets) {
  if (hashes.empty() && offsets.empty())
    return true;
  if (hashes.size() > (std::numeric_limits<std::uint32_t>::max)() ||
      offsets.size() != frames + 1U || offsets.empty() ||
      offsets.front() != 0U || offsets.back() != hashes.size())
    return false;
  for (std::size_t index = 1; index < offsets.size(); ++index) {
    if (offsets[index] < offsets[index - 1U] || offsets[index] > hashes.size())
      return false;
  }
  return true;
}

bool decode_keyframe(std::uint32_t raw_size,
                     const std::vector<std::uint8_t> &compressed,
                     std::vector<std::uint8_t> &decoded) {
  if (raw_size == 0U || raw_size > kMaxInitialStateBytes || compressed.empty())
    return false;

  const unsigned long long encoded_size =
      ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (encoded_size == ZSTD_CONTENTSIZE_ERROR ||
      encoded_size == ZSTD_CONTENTSIZE_UNKNOWN || encoded_size != raw_size)
    return false;

  const std::size_t frame_size =
      ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
  if (ZSTD_isError(frame_size) || frame_size != compressed.size())
    return false;

  std::vector<std::uint8_t> candidate(raw_size);
  const std::size_t decompressed_size = ZSTD_decompress(
      candidate.data(), candidate.size(), compressed.data(), compressed.size());
  if (ZSTD_isError(decompressed_size) || decompressed_size != raw_size)
    return false;
  decoded = std::move(candidate);
  return true;
}

bool valid_keyframe_encoding(std::uint32_t raw_size,
                             const std::vector<std::uint8_t> &compressed) {
  std::vector<std::uint8_t> decoded;
  return decode_keyframe(raw_size, compressed, decoded);
}

void put_u32(std::ostream &o, uint32_t v) {
  uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16),
                  uint8_t(v >> 24)};
  o.write(reinterpret_cast<const char *>(b), 4);
}
void put_u16(std::ostream &o, uint16_t v) {
  uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
  o.write(reinterpret_cast<const char *>(b), 2);
}
void put_u64(std::ostream &o, uint64_t v) {
  uint8_t b[8];
  for (int i = 0; i < 8; ++i)
    b[i] = uint8_t(v >> (8 * i));
  o.write(reinterpret_cast<const char *>(b), 8);
}
std::atomic<std::uint64_t> next_sibling_id{0};

std::filesystem::path
unique_sibling_path(const std::filesystem::path &destination,
                    const char *marker) {
  std::filesystem::path sibling = destination;
#if defined(_WIN32)
  const auto process_id = static_cast<unsigned long>(::GetCurrentProcessId());
#else
  const auto process_id = static_cast<unsigned long>(::getpid());
#endif
  sibling +=
      marker + std::to_string(process_id) + "." +
      std::to_string(next_sibling_id.fetch_add(1, std::memory_order_relaxed));
  return sibling;
}

std::filesystem::path
destination_lock_path(const std::filesystem::path &destination) {
  std::filesystem::path lock = destination;
  lock += ".ayther.lock";
  return lock;
}

#if !defined(_WIN32)
int acquire_lock_file(const std::filesystem::path &path) {
  int flags = O_RDWR | O_CREAT;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
  flags |= O_NOFOLLOW;
#endif
  const int descriptor = ::open(path.c_str(), flags, 0600);
  if (descriptor < 0)
    return -1;

  struct stat information {};
  if (::fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode)) {
    (void)::close(descriptor);
    return -1;
  }
  while (::flock(descriptor, LOCK_EX) != 0) {
    if (errno == EINTR)
      continue;
    (void)::close(descriptor);
    return -1;
  }
  return descriptor;
}

void release_lock_file(int &descriptor) noexcept {
  if (descriptor < 0)
    return;
  (void)::flock(descriptor, LOCK_UN);
  (void)::close(descriptor);
  descriptor = -1;
}
#endif

class DestinationLock {
public:
  explicit DestinationLock(const std::filesystem::path &destination) {
    const std::filesystem::path path = destination_lock_path(destination);
#if defined(_WIN32)
    const std::string identity =
        recording_detail::destination_lock_identity(destination);
    std::wstring mutex_name = L"Local\\AytherRecording.";
    mutex_name.append(identity.begin(), identity.end());
    mutex_ = ::CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (mutex_ == nullptr)
      return;
    const DWORD wait_result = ::WaitForSingleObject(mutex_, INFINITE);
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
      (void)::CloseHandle(mutex_);
      mutex_ = nullptr;
      return;
    }
    mutex_owned_ = true;

    handle_ = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
      return;

    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle_, &information) == FALSE ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        ::LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                     &overlapped_) == FALSE) {
      (void)::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
#else
    const std::filesystem::path identity_path =
        recording_detail::destination_identity_lock_path(destination);
    identity_descriptor_ = acquire_lock_file(identity_path);
    if (identity_descriptor_ < 0)
      return;
    descriptor_ = acquire_lock_file(path);
    if (descriptor_ < 0) {
      release_lock_file(identity_descriptor_);
      return;
    }
#endif
  }

  ~DestinationLock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      (void)::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
      (void)::CloseHandle(handle_);
    }
    if (mutex_owned_)
      (void)::ReleaseMutex(mutex_);
    if (mutex_ != nullptr)
      (void)::CloseHandle(mutex_);
#else
    release_lock_file(descriptor_);
    release_lock_file(identity_descriptor_);
#endif
  }

  DestinationLock(const DestinationLock &) = delete;
  DestinationLock &operator=(const DestinationLock &) = delete;

  [[nodiscard]] explicit operator bool() const {
#if defined(_WIN32)
    return mutex_owned_ && handle_ != INVALID_HANDLE_VALUE;
#else
    return identity_descriptor_ >= 0 && descriptor_ >= 0;
#endif
  }

private:
#if defined(_WIN32)
  HANDLE mutex_ = nullptr;
  bool mutex_owned_ = false;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  OVERLAPPED overlapped_{};
#else
  int identity_descriptor_ = -1;
  int descriptor_ = -1;
#endif
};

#if defined(_WIN32)

class UniqueHandle {
public:
  explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE)
      : handle_(handle) {}
  ~UniqueHandle() {
    if (handle_ != INVALID_HANDLE_VALUE)
      (void)::CloseHandle(handle_);
  }

  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;

  [[nodiscard]] HANDLE get() const { return handle_; }

  [[nodiscard]] bool close() {
    if (handle_ == INVALID_HANDLE_VALUE)
      return true;
    const HANDLE handle = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return ::CloseHandle(handle) != FALSE;
  }

private:
  HANDLE handle_;
};

bool is_regular_non_reparse_handle(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION information{};
  return ::GetFileInformationByHandle(file, &information) != FALSE &&
         (information.dwFileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool read_regular_file(const std::filesystem::path &path,
                       std::vector<char> &bytes) {
  bytes.clear();
  UniqueHandle file(
      ::CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                        FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr));
  if (file.get() == INVALID_HANDLE_VALUE ||
      !is_regular_non_reparse_handle(file.get()))
    return false;

  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) >
          (std::numeric_limits<std::size_t>::max)() ||
      static_cast<unsigned long long>(size.QuadPart) > kMaxRecordingBytes)
    return false;

  bytes.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>((std::min)(
        bytes.size() - offset,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read = 0;
    if (::ReadFile(file.get(), bytes.data() + offset, chunk, &read, nullptr) ==
            FALSE ||
        read == 0)
      return false;
    offset += read;
  }

  // Catch a concurrent extension as well as OS-level short/failed reads.
  // ReadFile failures are the native equivalent of an iostream badbit.
  char extra = 0;
  DWORD extra_size = 0;
  if (::ReadFile(file.get(), &extra, 1, &extra_size, nullptr) == FALSE ||
      extra_size != 0)
    return false;
  return file.close();
}

bool delete_windows_file(const std::filesystem::path &path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return ::GetLastError() == ERROR_FILE_NOT_FOUND;

  const bool read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
  if (read_only &&
      ::SetFileAttributesW(path.c_str(),
                           attributes & ~FILE_ATTRIBUTE_READONLY) == FALSE)
    return false;
  if (::DeleteFileW(path.c_str()) != FALSE)
    return true;
  if (read_only)
    (void)::SetFileAttributesW(path.c_str(), attributes);
  return false;
}

#else

class UniqueDescriptor {
public:
  explicit UniqueDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
  ~UniqueDescriptor() {
    if (descriptor_ >= 0)
      (void)::close(descriptor_);
  }

  UniqueDescriptor(const UniqueDescriptor &) = delete;
  UniqueDescriptor &operator=(const UniqueDescriptor &) = delete;

  [[nodiscard]] int get() const { return descriptor_; }

  [[nodiscard]] bool close() {
    if (descriptor_ < 0)
      return true;
    const int descriptor = descriptor_;
    descriptor_ = -1;
    return ::close(descriptor) == 0;
  }

private:
  int descriptor_;
};

int open_without_following(const std::filesystem::path &path, int access) {
  int flags = access;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
  flags |= O_NOFOLLOW;
#endif
  return ::open(path.c_str(), flags);
}

bool read_regular_file(const std::filesystem::path &path,
                       std::vector<char> &bytes) {
  bytes.clear();
  UniqueDescriptor file(open_without_following(path, O_RDONLY));
  struct stat information {};
  if (file.get() < 0 || ::fstat(file.get(), &information) != 0 ||
      !S_ISREG(information.st_mode))
    return false;
  if (information.st_size < 0)
    return false;

  const auto expected_size =
      static_cast<unsigned long long>(information.st_size);
  if (expected_size > (std::numeric_limits<std::size_t>::max)() ||
      expected_size > kMaxRecordingBytes)
    return false;
  bytes.reserve(static_cast<std::size_t>(expected_size));

  std::array<char, 64U * 1024U> chunk{};
  for (;;) {
    const ssize_t read = ::read(file.get(), chunk.data(), chunk.size());
    if (read > 0) {
      bytes.insert(bytes.end(), chunk.data(), chunk.data() + read);
      continue;
    }
    if (read == 0) {
      struct stat final_information {};
      if (::fstat(file.get(), &final_information) != 0 ||
          final_information.st_size != information.st_size ||
          bytes.size() != static_cast<std::size_t>(expected_size))
        return false;
      return file.close();
    }
    if (errno != EINTR)
      return false;
  }
}

#if defined(__linux__)
bool copy_preserved_xattrs(int source, int destination) {
  const ssize_t names_size = ::flistxattr(source, nullptr, 0);
  if (names_size < 0)
    return errno == ENOTSUP || errno == EOPNOTSUPP;
  if (names_size == 0)
    return true;

  std::vector<char> names(static_cast<std::size_t>(names_size));
  if (::flistxattr(source, names.data(), names.size()) != names_size)
    return false;

  const char *name = names.data();
  const char *const end = names.data() + names.size();
  while (name < end) {
    const std::size_t length = std::strlen(name);
    if (length == 0 || name + length >= end)
      return false;

    const bool user_attribute = std::strncmp(name, "user.", 5) == 0;
    const bool access_acl = std::strcmp(name, "system.posix_acl_access") == 0;
    if (user_attribute || access_acl) {
      const ssize_t value_size = ::fgetxattr(source, name, nullptr, 0);
      if (value_size < 0)
        return false;
      std::vector<char> value(
          static_cast<std::size_t>((std::max)(value_size, ssize_t{1})));
      const ssize_t copied = ::fgetxattr(source, name, value.data(),
                                         static_cast<std::size_t>(value_size));
      if (copied != value_size ||
          ::fsetxattr(destination, name, value.data(),
                      static_cast<std::size_t>(value_size), 0) != 0)
        return false;
    }
    name += length + 1;
  }
  return true;
}
#endif

#endif

bool write_replacement(const std::filesystem::path &destination,
                       const std::vector<char> &bytes) {
  constexpr unsigned int kCreateAttempts = 64;
  for (unsigned int attempt = 0; attempt < kCreateAttempts; ++attempt) {
    const std::filesystem::path temporary =
        unique_sibling_path(destination, ".tmp.");
#if defined(_WIN32)
    UniqueHandle file(::CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
      if (::GetLastError() == ERROR_FILE_EXISTS)
        continue;
      return false;
    }

    bool complete = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const DWORD chunk = static_cast<DWORD>((std::min)(
          bytes.size() - offset,
          static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
      DWORD written = 0;
      if (::WriteFile(file.get(), bytes.data() + offset, chunk, &written,
                      nullptr) == FALSE ||
          written == 0) {
        complete = false;
        break;
      }
      offset += written;
    }
    if (complete && ::FlushFileBuffers(file.get()) == FALSE)
      complete = false;
    if (!file.close())
      complete = false;
    if (!complete) {
      (void)::DeleteFileW(temporary.c_str());
      return false;
    }

    // Inspect the final component without following a reparse point. A missing
    // destination uses a no-overwrite move; an existing regular file uses
    // ReplaceFile so its DACL, attributes and named streams survive.
    UniqueHandle current(::CreateFileW(
        destination.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (current.get() == INVALID_HANDLE_VALUE) {
      const DWORD open_error = ::GetLastError();
      if (open_error != ERROR_FILE_NOT_FOUND) {
        ayther::log::write(ayther::log::Severity::Error, "recording",
                           "destination_probe_failed",
                           "cannot inspect %s (win32=%lu)",
                           destination.string().c_str(), open_error);
        (void)::DeleteFileW(temporary.c_str());
        return false;
      }
      if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE)
        return true;
      const DWORD move_error = ::GetLastError();
      ayther::log::write(ayther::log::Severity::Error, "recording",
                         "create_publish_failed",
                         "cannot publish new %s (win32=%lu)",
                         destination.string().c_str(), move_error);
      (void)::DeleteFileW(temporary.c_str());
      return false;
    }
    if (!is_regular_non_reparse_handle(current.get()) || !current.close()) {
      (void)::DeleteFileW(temporary.c_str());
      return false;
    }

    std::filesystem::path backup;
    for (unsigned int backup_attempt = 0; backup_attempt < kCreateAttempts;
         ++backup_attempt) {
      backup = unique_sibling_path(destination, ".bak.");
      if (::GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES &&
          ::GetLastError() == ERROR_FILE_NOT_FOUND)
        break;
      backup.clear();
    }
    if (backup.empty()) {
      (void)::DeleteFileW(temporary.c_str());
      return false;
    }

    if (::ReplaceFileW(destination.c_str(), temporary.c_str(), backup.c_str(),
                       0, nullptr, nullptr) != FALSE)
      return delete_windows_file(backup);

    const DWORD replace_error = ::GetLastError();
    if (replace_error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
      // ReplaceFile moved the confirmed file to backup but could not publish
      // the replacement. Restore the confirmed name before removing the temp.
      if (::MoveFileExW(backup.c_str(), destination.c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE)
        (void)delete_windows_file(temporary);
      return false;
    }

    // For all other documented failures the confirmed destination retains its
    // name when a backup path was supplied.
    if (::GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES)
      (void)delete_windows_file(temporary);
    return false;
#else
    const int source_descriptor = open_without_following(destination, O_RDONLY);
    const int source_error = source_descriptor < 0 ? errno : 0;
    UniqueDescriptor source(source_descriptor);
    const bool destination_exists = source.get() >= 0;
    struct stat destination_stat {};
    if ((!destination_exists && source_error != ENOENT) ||
        (destination_exists && (::fstat(source.get(), &destination_stat) != 0 ||
                                !S_ISREG(destination_stat.st_mode))))
      return false;

    int create_flags = O_WRONLY | O_CREAT | O_EXCL;
#if defined(O_CLOEXEC)
    create_flags |= O_CLOEXEC;
#endif
    const mode_t create_mode = destination_exists ? 0600 : 0666;
    UniqueDescriptor file(::open(temporary.c_str(), create_flags, create_mode));
    if (file.get() < 0) {
      if (errno == EEXIST)
        continue;
      return false;
    }

    bool complete = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      constexpr std::size_t kWriteChunk = 1U << 30U;
      const std::size_t chunk = (std::min)(bytes.size() - offset, kWriteChunk);
      const ssize_t written = ::write(file.get(), bytes.data() + offset, chunk);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0) {
        complete = false;
        break;
      }
      offset += static_cast<std::size_t>(written);
    }
    if (complete && destination_exists &&
        (::fchown(file.get(), destination_stat.st_uid,
                  destination_stat.st_gid) != 0 ||
         ::fchmod(file.get(), destination_stat.st_mode & 07777) != 0))
      complete = false;
#if defined(__linux__)
    if (complete && destination_exists &&
        !copy_preserved_xattrs(source.get(), file.get()))
      complete = false;
#endif
    if (complete && ::fsync(file.get()) != 0)
      complete = false;
    if (!file.close())
      complete = false;
    if (!complete) {
      (void)::unlink(temporary.c_str());
      return false;
    }

    // Refuse to replace a path that changed after its metadata was captured,
    // or to create over a path that appeared after the initial no-follow open.
    // Cooperating writers cannot race here because they share the sidecar lock.
    struct stat current_stat {};
    if (destination_exists) {
      if (::lstat(destination.c_str(), &current_stat) != 0 ||
          !S_ISREG(current_stat.st_mode) ||
          current_stat.st_dev != destination_stat.st_dev ||
          current_stat.st_ino != destination_stat.st_ino) {
        (void)::unlink(temporary.c_str());
        return false;
      }
    } else if (::lstat(destination.c_str(), &current_stat) == 0 ||
               errno != ENOENT) {
      (void)::unlink(temporary.c_str());
      return false;
    }
    const std::filesystem::path parent = destination.has_parent_path()
                                             ? destination.parent_path()
                                             : std::filesystem::path{"."};
    int directory_flags = O_RDONLY;
#if defined(O_DIRECTORY)
    directory_flags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
    directory_flags |= O_CLOEXEC;
#endif
    UniqueDescriptor directory(::open(parent.c_str(), directory_flags));
    if (directory.get() < 0) {
      (void)::unlink(temporary.c_str());
      return false;
    }
    if (destination_exists) {
      if (::rename(temporary.c_str(), destination.c_str()) != 0) {
        (void)::unlink(temporary.c_str());
        return false;
      }
    } else {
      // link() provides no-overwrite creation while exposing only the already
      // flushed inode; removing its temporary name completes publication.
      if (::link(temporary.c_str(), destination.c_str()) != 0) {
        (void)::unlink(temporary.c_str());
        return false;
      }
      if (::unlink(temporary.c_str()) != 0)
        return false;
    }

    // The file was durable before rename; persist the directory entry too.
    // A failure here means publication happened but its survival after a
    // crash is unknown, so the boolean deliberately reports unconfirmed.
    const bool directory_synced = ::fsync(directory.get()) == 0;
    const bool directory_closed = directory.close();
    return directory_synced && directory_closed;
#endif
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------
bool AytherRecording::save(const std::string &path) const {
  const bool text_sizes_valid =
      game_id.size() <= (std::numeric_limits<std::uint32_t>::max)() &&
      name.size() <= (std::numeric_limits<std::uint32_t>::max)();
  const bool histories_valid =
      valid_hash_history(inputs.size(), sprite_hashes, hash_offsets) &&
      valid_hash_history(inputs.size(), audio_hashes, audio_offsets);
  const std::size_t effective_trim_out =
      trim_out == 0U ? inputs.size() : trim_out;
  const bool trim_valid =
      trim_in <= effective_trim_out && effective_trim_out <= inputs.size();
  bool keyframes_valid =
      keyframes.size() <= inputs.size() + 1U &&
      keyframes.size() <= (std::numeric_limits<std::uint32_t>::max)();
  std::uint32_t previous_keyframe = 0;
  bool first_keyframe = true;
  for (const Keyframe &keyframe : keyframes) {
    keyframes_valid =
        keyframes_valid && keyframe.frame <= inputs.size() &&
        (first_keyframe || keyframe.frame > previous_keyframe) &&
        valid_keyframe_encoding(keyframe.raw_size, keyframe.comp) &&
        keyframe.comp.size() <= (std::numeric_limits<std::uint32_t>::max)();
    previous_keyframe = keyframe.frame;
    first_keyframe = false;
  }
  if (empty() || inputs.size() > kMaxRecordingFrames ||
      initial_state.size() > kMaxInitialStateBytes || !text_sizes_valid ||
      !histories_valid || !trim_valid || !keyframes_valid) {
    ayther::log::write(ayther::log::Severity::Warning, "recording",
                       "refuse_save_invalid_take",
                       "refuse to save empty, oversized or invalid take");
    return false;
  }

  // Keep save() and patch_name() from publishing snapshots derived from
  // different generations of the same destination, including across
  // processes. The identity lock joins hard links; the spelling-specific
  // sidecar remains stable across atomic inode replacement. Both are
  // intentionally persistent because unlinking a lock file lets old and new
  // open handles protect different inodes.
  const DestinationLock destination_lock{std::filesystem::path(path)};
  if (!destination_lock) {
    ayther::log::write(ayther::log::Severity::Error, "recording", "cannot_lock",
                       "cannot lock %s", path.c_str());
    return false;
  }

  // Serialize away from the destination. No failure before publication may
  // truncate the last confirmed take.
  std::ostringstream f(std::ios::binary | std::ios::out);

  // Compress the initial savestate.
  const size_t bound = ZSTD_compressBound(initial_state.size());
  std::vector<uint8_t> comp(bound);
  const size_t n = ZSTD_compress(comp.data(), bound, initial_state.data(),
                                 initial_state.size(), kZstdLevel);
  if (ZSTD_isError(n)) {
    ayther::log::write(ayther::log::Severity::Error, "recording",
                       "compress_failed", "compress failed: %s",
                       ZSTD_getErrorName(n));
    return false;
  }
  comp.resize(n);

  f.write(kMagic, 4);
  put_u32(f, kVersion);
  put_u32(f, static_cast<uint32_t>(game_id.size()));
  f.write(game_id.data(), game_id.size());
  put_u32(f, static_cast<uint32_t>(name.size()));
  f.write(name.data(), name.size());
  put_u32(f, frame_count());
  put_u32(f, static_cast<uint32_t>(initial_state.size()));
  put_u32(f, static_cast<uint32_t>(comp.size()));
  f.write(reinterpret_cast<const char *>(comp.data()), comp.size());
  for (uint16_t in : inputs)
    put_u16(f, in);

  // v2: trim marks + per-frame occurrence stats.
  put_u32(f, trim_in);
  put_u32(f, trim_out ? trim_out : frame_count());
  for (uint32_t i = 0; i < frame_count(); ++i) {
    const FrameStat s = (i < stats.size()) ? stats[i] : FrameStat{};
    put_u16(f, s.sprites);
    put_u16(f, s.tiles);
    put_u16(f, s.audio);
    put_u16(f, s.plane_a);
    put_u16(f, s.plane_b); // v5
    put_u16(f, s.plane_w); // v6
  }

  // v3: per-frame sprite-hash history (CSR). Absent (all-zero counts) when not
  // captured — the flat array is empty and every per-frame count is 0.
  const bool have_hashes = hash_offsets.size() == frame_count() + 1;
  put_u32(f, have_hashes ? static_cast<uint32_t>(sprite_hashes.size()) : 0u);
  if (have_hashes) {
    for (uint64_t h : sprite_hashes)
      put_u64(f, h);
    for (uint32_t i = 0; i < frame_count(); ++i)
      put_u32(f, hash_offsets[i + 1] - hash_offsets[i]);
  } else {
    for (uint32_t i = 0; i < frame_count(); ++i)
      put_u32(f, 0u);
  }

  // v4: baked replay keyframes (ya comprimidos en memoria).
  put_u32(f, static_cast<uint32_t>(keyframes.size()));
  for (const Keyframe &kf : keyframes) {
    put_u32(f, kf.frame);
    put_u32(f, kf.raw_size);
    put_u32(f, static_cast<uint32_t>(kf.comp.size()));
    f.write(reinterpret_cast<const char *>(kf.comp.data()), kf.comp.size());
  }

  // v7: per-frame audio-hash history (CSR). Mismo patrón que v3 (sprites):
  // ausente (counts en cero) cuando no se capturó.
  const bool have_audio = audio_offsets.size() == frame_count() + 1;
  put_u32(f, have_audio ? static_cast<uint32_t>(audio_hashes.size()) : 0u);
  if (have_audio) {
    for (uint64_t h : audio_hashes)
      put_u64(f, h);
    for (uint32_t i = 0; i < frame_count(); ++i)
      put_u32(f, audio_offsets[i + 1] - audio_offsets[i]);
  } else {
    for (uint32_t i = 0; i < frame_count(); ++i)
      put_u32(f, 0u);
  }

  // v8: algoritmo de hash con que se capturó la historia de sprites.
  put_u32(f, hash_algo);

  if (!f.good()) {
    ayther::log::write(ayther::log::Severity::Error, "recording",
                       "serialize_failed", "cannot serialize %s", path.c_str());
    return false;
  }
  const std::string serialized = f.str();
  if (serialized.size() > kMaxRecordingBytes) {
    ayther::log::write(ayther::log::Severity::Error, "recording",
                       "take_too_large", "serialized take exceeds %zu bytes",
                       kMaxRecordingBytes);
    return false;
  }
  const std::vector<char> image(serialized.begin(), serialized.end());
  if (!write_replacement(std::filesystem::path(path), image)) {
    ayther::log::write(ayther::log::Severity::Error, "recording",
                       "cannot_publish", "cannot publish %s", path.c_str());
    return false;
  }

  ayther::log::write(
      ayther::log::Severity::Info, "recording", "saved_frames_state_b",
      "saved %s  (%u frames, state %zu→%zu B, %zu sprite-hashes, %zu "
      "keyframes)",
      path.c_str(), frame_count(), initial_state.size(), comp.size(),
      have_hashes ? sprite_hashes.size() : 0u, keyframes.size());
  return true;
}

// ---------------------------------------------------------------------------
// patch_name — reescritura del nombre visible en el header, resto intacto
// ---------------------------------------------------------------------------
bool AytherRecording::patch_name(const std::string &path,
                                 const std::string &new_name) {
  const std::filesystem::path destination(path);
  const DestinationLock destination_lock(destination);
  if (!destination_lock)
    return false;

  // The native readers open the final component without following it and
  // treat any failed/short read as an error (the equivalent of checking
  // std::istream::bad() after iterator-based input).
  std::vector<char> buf;
  if (!read_regular_file(destination, buf))
    return false;

  auto u32_at = [&buf](size_t off) {
    return uint32_t(uint8_t(buf[off])) |
           (uint32_t(uint8_t(buf[off + 1])) << 8) |
           (uint32_t(uint8_t(buf[off + 2])) << 16) |
           (uint32_t(uint8_t(buf[off + 3])) << 24);
  };
  // Header: magic[4] + u32 version + (u32,game_id) + (u32,name) + …
  if (buf.size() < 16 || std::memcmp(buf.data(), kMagic, 4) != 0)
    return false;
  const uint32_t version = u32_at(4);
  if (version < 2 || version > kVersion)
    return false;
  const size_t gid_len = u32_at(8);
  if (gid_len > buf.size() - 12)
    return false;
  const size_t name_pos = 12 + gid_len;
  if (buf.size() - name_pos < 4)
    return false;
  const size_t name_len = u32_at(name_pos);
  if (name_len > buf.size() - name_pos - 4)
    return false;
  const size_t rest = name_pos + 4 + name_len;
  if (name_len == new_name.size() &&
      std::memcmp(buf.data() + name_pos + 4, new_name.data(), name_len) == 0)
    return true;
  if (new_name.size() > (std::numeric_limits<std::uint32_t>::max)())
    return false;

  const std::size_t prefix_size = name_pos + 4U;
  const std::size_t suffix_size = buf.size() - rest;
  if (prefix_size > kMaxRecordingBytes ||
      suffix_size > kMaxRecordingBytes - prefix_size ||
      new_name.size() > kMaxRecordingBytes - prefix_size - suffix_size)
    return false;

  // Construct a complete replacement image before opening any output. Each
  // publisher then writes and flushes an exclusive sibling temporary; there
  // is no shared `<destination>.tmp` for concurrent writers to truncate.
  std::vector<char> replacement;
  replacement.reserve(prefix_size + new_name.size() + suffix_size);
  replacement.insert(replacement.end(), buf.begin(),
                     buf.begin() + static_cast<std::ptrdiff_t>(name_pos));
  const auto encoded_name_size = static_cast<std::uint32_t>(new_name.size());
  replacement.push_back(static_cast<char>(encoded_name_size));
  replacement.push_back(static_cast<char>(encoded_name_size >> 8U));
  replacement.push_back(static_cast<char>(encoded_name_size >> 16U));
  replacement.push_back(static_cast<char>(encoded_name_size >> 24U));
  replacement.insert(replacement.end(), new_name.begin(), new_name.end());
  replacement.insert(replacement.end(),
                     buf.begin() + static_cast<std::ptrdiff_t>(rest),
                     buf.end());

  return write_replacement(destination, replacement);
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------
std::optional<AytherRecording> AytherRecording::load(const std::string &path) {
  try {
    std::vector<char> image;
    if (!read_regular_file(std::filesystem::path(path), image))
      return std::nullopt;

    RecordingCursor cursor(image);
    char magic[4]{};
    std::uint32_t version = 0;
    if (!cursor.read_bytes(magic, sizeof(magic)) ||
        std::memcmp(magic, kMagic, sizeof(magic)) != 0 ||
        !cursor.read_u32(version) || version < 2U || version > kVersion)
      return std::nullopt;

    AytherRecording rec;
    if (!cursor.read_string(rec.game_id) || !cursor.read_string(rec.name))
      return std::nullopt;

    std::uint32_t frames = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t compressed_size = 0;
    if (!cursor.read_u32(frames) || !cursor.read_u32(raw_size) ||
        !cursor.read_u32(compressed_size) || frames == 0U ||
        frames > kMaxRecordingFrames || raw_size == 0U ||
        raw_size > kMaxInitialStateBytes ||
        compressed_size > cursor.remaining())
      return std::nullopt;

    std::vector<std::uint8_t> compressed_state(compressed_size);
    if (!cursor.read_bytes(compressed_state.data(), compressed_state.size()))
      return std::nullopt;

    rec.initial_state.resize(raw_size);
    const std::size_t decompressed_size =
        ZSTD_decompress(rec.initial_state.data(), rec.initial_state.size(),
                        compressed_state.data(), compressed_state.size());
    if (ZSTD_isError(decompressed_size) || decompressed_size != raw_size) {
      ayther::log::write(ayther::log::Severity::Error, "recording",
                         "decompress_failed", "decompress failed for %s",
                         path.c_str());
      return std::nullopt;
    }

    if (static_cast<std::uint64_t>(frames) * sizeof(std::uint16_t) >
        cursor.remaining())
      return std::nullopt;
    rec.inputs.resize(frames);
    for (std::uint16_t &input : rec.inputs) {
      if (!cursor.read_u16(input))
        return std::nullopt;
    }

    // v2: trim marks and the mandatory per-frame occurrence summary.
    if (!cursor.read_u32(rec.trim_in) || !cursor.read_u32(rec.trim_out))
      return std::nullopt;
    const std::size_t stat_bytes = version >= 6U   ? 12U
                                   : version >= 5U ? 10U
                                                   : 6U;
    if (static_cast<std::uint64_t>(frames) * stat_bytes > cursor.remaining())
      return std::nullopt;
    rec.stats.resize(frames);
    for (FrameStat &stat : rec.stats) {
      if (!cursor.read_u16(stat.sprites) || !cursor.read_u16(stat.tiles) ||
          !cursor.read_u16(stat.audio))
        return std::nullopt;
      if (version >= 5U &&
          (!cursor.read_u16(stat.plane_a) || !cursor.read_u16(stat.plane_b)))
        return std::nullopt;
      if (version >= 6U && !cursor.read_u16(stat.plane_w))
        return std::nullopt;
    }
    if (rec.trim_in > frames || rec.trim_out > frames)
      return std::nullopt;
    if (rec.trim_out == 0U)
      rec.trim_out = frames;
    if (rec.trim_in > rec.trim_out)
      return std::nullopt;

    // Every section introduced by the declared version is mandatory. An
    // absent history is encoded as total=0 plus one zero count per frame.
    if (version >= 3U &&
        !read_hash_history(cursor, frames, rec.sprite_hashes, rec.hash_offsets))
      return std::nullopt;

    if (version >= 4U) {
      std::uint32_t keyframe_count = 0;
      if (!cursor.read_u32(keyframe_count) || keyframe_count > frames + 1U)
        return std::nullopt;
      rec.keyframes.reserve(keyframe_count);
      std::uint32_t previous_keyframe = 0;
      for (std::uint32_t index = 0; index < keyframe_count; ++index) {
        AytherRecording::Keyframe keyframe;
        std::uint32_t keyframe_compressed_size = 0;
        if (!cursor.read_u32(keyframe.frame) ||
            !cursor.read_u32(keyframe.raw_size) ||
            !cursor.read_u32(keyframe_compressed_size) ||
            keyframe.frame > frames || keyframe.raw_size == 0U ||
            (index != 0U && keyframe.frame <= previous_keyframe) ||
            keyframe.raw_size > kMaxInitialStateBytes ||
            keyframe_compressed_size > cursor.remaining())
          return std::nullopt;
        keyframe.comp.resize(keyframe_compressed_size);
        if (!cursor.read_bytes(keyframe.comp.data(), keyframe.comp.size()))
          return std::nullopt;
        if (!valid_keyframe_encoding(keyframe.raw_size, keyframe.comp))
          return std::nullopt;
        previous_keyframe = keyframe.frame;
        rec.keyframes.push_back(std::move(keyframe));
      }
    }

    if (version >= 7U &&
        !read_hash_history(cursor, frames, rec.audio_hashes, rec.audio_offsets))
      return std::nullopt;

    rec.hash_algo = 0;
    if (version >= 8U && !cursor.read_u32(rec.hash_algo))
      return std::nullopt;
    if (!cursor.empty())
      return std::nullopt;

    ayther::log::write(ayther::log::Severity::Info, "recording",
                       "loaded_frames_keyframes_hash",
                       "loaded %s  (%u frames%s, %zu keyframes, hash_algo %u)",
                       path.c_str(), frames,
                       rec.hash_offsets.empty() ? "" : ", +hash history",
                       rec.keyframes.size(), rec.hash_algo);
    return rec;
  } catch (const std::bad_alloc &) {
    ayther::log::write(ayther::log::Severity::Error, "recording",
                       "allocation_failed", "cannot allocate while loading %s",
                       path.c_str());
  } catch (const std::length_error &) {
    ayther::log::write(ayther::log::Severity::Error, "recording",
                       "invalid_length", "invalid length while loading %s",
                       path.c_str());
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Keyframes horneados (R7e): compresión/descompresión por separado.
// ---------------------------------------------------------------------------
void AytherRecording::add_keyframe(uint32_t frame,
                                   const std::vector<uint8_t> &raw_state) {
  if (raw_state.empty() || raw_state.size() > kMaxInitialStateBytes ||
      raw_state.size() > (std::numeric_limits<std::uint32_t>::max)())
    return;
  const size_t bound = ZSTD_compressBound(raw_state.size());
  Keyframe kf;
  kf.frame = frame;
  kf.raw_size = static_cast<uint32_t>(raw_state.size());
  kf.comp.resize(bound);
  const size_t n = ZSTD_compress(kf.comp.data(), bound, raw_state.data(),
                                 raw_state.size(), kZstdLevel);
  if (ZSTD_isError(n))
    return;
  kf.comp.resize(n);
  keyframes.push_back(std::move(kf));
}

bool AytherRecording::decompress_keyframe(size_t idx,
                                          std::vector<uint8_t> &out) const {
  if (idx >= keyframes.size())
    return false;
  const Keyframe &kf = keyframes[idx];
  return decode_keyframe(kf.raw_size, kf.comp, out);
}

// ---------------------------------------------------------------------------
// slice — sub-toma [begin, end) rebasada a 0 (Fase C: dividir tomas)
// ---------------------------------------------------------------------------
AytherRecording AytherRecording::slice(uint32_t begin, uint32_t end,
                                       std::vector<uint8_t> state) const {
  AytherRecording r;
  r.game_id = game_id;
  r.name = name;           // el caller renombra el tail
  r.hash_algo = hash_algo; // la sub-toma hereda el algo de su historia
  r.initial_state = std::move(state);
  r.inputs.assign(inputs.begin() + begin, inputs.begin() + end);
  if (stats.size() >= end) // stats parciales -> se omiten
    r.stats.assign(stats.begin() + begin, stats.begin() + end);

  // Historia CSR (.arp v3) — solo si esta completa para el rango.
  if (hash_offsets.size() == inputs.size() + 1) {
    const uint32_t base = hash_offsets[begin];
    r.sprite_hashes.assign(sprite_hashes.begin() + base,
                           sprite_hashes.begin() + hash_offsets[end]);
    r.hash_offsets.resize(end - begin + 1);
    for (uint32_t i = 0; i <= end - begin; ++i)
      r.hash_offsets[i] = hash_offsets[begin + i] - base;
  }

  // Historia CSR de audio (.arp v7) — idem.
  if (audio_offsets.size() == inputs.size() + 1) {
    const uint32_t base = audio_offsets[begin];
    r.audio_hashes.assign(audio_hashes.begin() + base,
                          audio_hashes.begin() + audio_offsets[end]);
    r.audio_offsets.resize(end - begin + 1);
    for (uint32_t i = 0; i <= end - begin; ++i)
      r.audio_offsets[i] = audio_offsets[begin + i] - base;
  }

  // Trim: interseccion de [trim_in, trim_out) con [begin, end), rebasada.
  // Interseccion vacia -> la sub-toma queda completa (sin recorte).
  const uint32_t tout = trim_out ? trim_out : frame_count();
  const uint32_t a = std::clamp(trim_in, begin, end);
  const uint32_t b = std::clamp(tout, begin, end);
  if (b > a) {
    r.trim_in = a - begin;
    r.trim_out = b - begin;
  } else {
    r.trim_in = 0;
    r.trim_out = end - begin;
  }
  return r;
}

} // namespace ayther
