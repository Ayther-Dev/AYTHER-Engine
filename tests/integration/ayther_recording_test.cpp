// ---------------------------------------------------------------------------
// ayther_recording_test.cpp — round-trip de .arp (save/load) + slice().
//
// Cubre la CAPA DE DATOS de la que dependen los features recording-céntricos:
//   · v7: historia de hashes de AUDIO por frame (CSR) → audio-por-sonido +
//         la vista previa de audio (preview_audio escanea audio_hashes/offsets).
//   · v3: historia de hashes de SPRITE por frame (CSR).
//   · v5/v6: cobertura de planos A/B/Window en los FrameStat.
//   · slice(): que la sub-toma rebase ambos CSR correctamente.
//
// No depende de SDL/Vulkan/ROM: compila ayther_recording.cpp + zstd. Hand-rolled
// (mismo estilo que ayther_core_ffi_test.cpp). ctest -R ayther_recording.
// ---------------------------------------------------------------------------
#include "ayther_recording.h"
#include "recording_lock_identity.h"

#include <algorithm>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

using ayther::AytherRecording;
using ayther::FrameStat;

namespace ayther::recording_detail {
[[nodiscard]] std::filesystem::path
destination_identity_lock_path(const std::filesystem::path& destination);
}

static int g_fail = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", (msg)); ++g_fail; } } while (0)

static std::filesystem::path lock_path_for(
    const std::filesystem::path& destination) {
    std::filesystem::path lock = destination;
    lock += ".ayther.lock";
    return lock;
}

class HeldDestinationLock {
public:
    explicit HeldDestinationLock(const std::filesystem::path& destination) {
        const std::filesystem::path path = lock_path_for(destination);
#if defined(_WIN32)
        const std::string identity =
            ayther::recording_detail::destination_lock_identity(destination);
        std::wstring mutex_name = L"Local\\AytherRecording.";
        mutex_name.append(identity.begin(), identity.end());
        mutex_ = ::CreateMutexW(nullptr, FALSE, mutex_name.c_str());
        if (mutex_ == nullptr) return;
        const DWORD wait_result = ::WaitForSingleObject(mutex_, INFINITE);
        if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
            ::CloseHandle(mutex_);
            mutex_ = nullptr;
            return;
        }
        mutex_owned_ = true;
        handle_ = ::CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return;
        if (::LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD,
                         MAXDWORD, &overlapped_) == FALSE) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        const auto acquire = [](const std::filesystem::path& lock_path) {
            const int descriptor =
                ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (descriptor < 0) return -1;
            while (::flock(descriptor, LOCK_EX) != 0) {
                if (errno == EINTR) continue;
                (void)::close(descriptor);
                return -1;
            }
            return descriptor;
        };
        identity_fd_ = acquire(
            ayther::recording_detail::destination_identity_lock_path(
                destination));
        if (identity_fd_ < 0) return;
        fd_ = acquire(path);
        if (fd_ < 0) {
            (void)::flock(identity_fd_, LOCK_UN);
            (void)::close(identity_fd_);
            identity_fd_ = -1;
        }
#endif
    }

    ~HeldDestinationLock() { release(); }

    HeldDestinationLock(const HeldDestinationLock&) = delete;
    HeldDestinationLock& operator=(const HeldDestinationLock&) = delete;

    [[nodiscard]] bool locked() const {
#if defined(_WIN32)
        return mutex_owned_ && handle_ != INVALID_HANDLE_VALUE;
#else
        return identity_fd_ >= 0 && fd_ >= 0;
#endif
    }

    void release() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
            (void)::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (mutex_owned_) {
            (void)::ReleaseMutex(mutex_);
            mutex_owned_ = false;
        }
        if (mutex_ != nullptr) {
            (void)::CloseHandle(mutex_);
            mutex_ = nullptr;
        }
#else
        const auto release_descriptor = [](int& descriptor) {
            if (descriptor < 0) return;
            (void)::flock(descriptor, LOCK_UN);
            (void)::close(descriptor);
            descriptor = -1;
        };
        release_descriptor(fd_);
        release_descriptor(identity_fd_);
#endif
    }

private:
#if defined(_WIN32)
    HANDLE mutex_ = nullptr;
    bool mutex_owned_ = false;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_{};
#else
    int identity_fd_ = -1;
    int fd_ = -1;
#endif
};

template <typename Operation>
static void check_operation_waits_for_destination_lock(
    const std::filesystem::path& destination, Operation operation,
    const char* blocked_message, const char* result_message) {
    HeldDestinationLock held(destination);
    CHECK(held.locked(), "crear bloqueo externo de destino");
    if (!held.locked()) return;

    std::mutex state_mutex;
    std::condition_variable state_changed;
    bool entered = false;
    bool finished = false;
    bool result = false;
    std::thread worker([&] {
        {
            const std::lock_guard lock(state_mutex);
            entered = true;
        }
        state_changed.notify_all();
        result = operation();
        {
            const std::lock_guard lock(state_mutex);
            finished = true;
        }
        state_changed.notify_all();
    });

    {
        std::unique_lock lock(state_mutex);
        state_changed.wait(lock, [&] { return entered; });
        const bool completed_while_locked = state_changed.wait_for(
            lock, std::chrono::milliseconds(200), [&] { return finished; });
        CHECK(!completed_while_locked, blocked_message);
    }
    held.release();
    worker.join();
    CHECK(result, result_message);
}

static bool has_replacement_artifact(const std::filesystem::path& destination) {
    const std::string temporary_prefix =
        destination.filename().string() + ".tmp.";
    const std::string backup_prefix =
        destination.filename().string() + ".bak.";
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(destination.parent_path())) {
        const std::string filename = entry.path().filename().string();
        if (filename.starts_with(temporary_prefix) ||
            filename.starts_with(backup_prefix))
            return true;
    }
    return false;
}

static std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

static bool write_bytes(const std::filesystem::path& path,
                        const std::vector<char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

static bool read_u32_le(const std::vector<char>& bytes, std::size_t offset,
                        std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[offset + 1])) << 8U) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[offset + 2])) << 16U) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[offset + 3])) << 24U);
    return true;
}

static bool write_u32_le(std::vector<char>& bytes, std::size_t offset,
                         std::uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    bytes[offset] = static_cast<char>(value);
    bytes[offset + 1] = static_cast<char>(value >> 8U);
    bytes[offset + 2] = static_cast<char>(value >> 16U);
    bytes[offset + 3] = static_cast<char>(value >> 24U);
    return true;
}

static bool locate_sprite_counts(const std::vector<char>& bytes,
                                 std::size_t& counts_offset,
                                 std::uint32_t& frame_count,
                                 std::uint32_t& total_hashes) {
    std::size_t offset = 8;
    std::uint32_t length = 0;
    for (int field = 0; field < 2; ++field) {
        if (!read_u32_le(bytes, offset, length)) return false;
        offset += 4;
        if (length > bytes.size() - offset) return false;
        offset += length;
    }

    std::uint32_t compressed_size = 0;
    if (!read_u32_le(bytes, offset, frame_count) ||
        !read_u32_le(bytes, offset + 8, compressed_size))
        return false;
    offset += 12;
    const std::uint64_t body_size =
        static_cast<std::uint64_t>(compressed_size) +
        static_cast<std::uint64_t>(frame_count) * 2U + 8U +
        static_cast<std::uint64_t>(frame_count) * 12U;
    if (body_size > bytes.size() - offset) return false;
    offset += static_cast<std::size_t>(body_size);
    if (!read_u32_le(bytes, offset, total_hashes)) return false;
    offset += 4;
    const std::uint64_t hash_bytes =
        static_cast<std::uint64_t>(total_hashes) * 8U;
    if (hash_bytes > bytes.size() - offset) return false;
    counts_offset = offset + static_cast<std::size_t>(hash_bytes);
    return static_cast<std::uint64_t>(frame_count) * 4U <=
           bytes.size() - counts_offset;
}

static bool locate_keyframe_frames(const std::vector<char>& bytes,
                                   std::vector<std::size_t>& frame_offsets) {
    std::size_t counts_offset = 0;
    std::uint32_t frames = 0;
    std::uint32_t total_hashes = 0;
    if (!locate_sprite_counts(bytes, counts_offset, frames, total_hashes))
        return false;
    std::size_t offset = counts_offset + static_cast<std::size_t>(frames) * 4U;
    std::uint32_t keyframe_count = 0;
    if (!read_u32_le(bytes, offset, keyframe_count)) return false;
    offset += 4;
    frame_offsets.clear();
    frame_offsets.reserve(keyframe_count);
    for (std::uint32_t index = 0; index < keyframe_count; ++index) {
        std::uint32_t compressed_size = 0;
        if (offset > bytes.size() || bytes.size() - offset < 12U ||
            !read_u32_le(bytes, offset + 8U, compressed_size) ||
            compressed_size > bytes.size() - offset - 12U)
            return false;
        frame_offsets.push_back(offset);
        offset += 12U + compressed_size;
    }
    return true;
}

// Una toma de 3 frames con stats de planos + ambos CSR (sprite v3 + audio v7).
static AytherRecording make_rec() {
    AytherRecording r;
    r.game_id       = "crc32:test";
    r.name          = "take_test";
    r.initial_state = { 0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04 };
    r.inputs        = { 0x0001, 0x0002, 0x0003 };           // frame_count = 3
    r.stats = {
        { 2, 5, 1, 10, 20, 3 },   // frame 0: sprites,tiles,audio,plane_a,plane_b,plane_w
        { 1, 4, 0, 11, 21, 3 },   // frame 1
        { 0, 4, 2, 12, 22, 3 },   // frame 2
    };
    // Sprite CSR (v3): frame0=2 hashes, frame1=1, frame2=1 → offsets {0,2,3,4}.
    r.sprite_hashes = { 0x1111, 0x2222, 0x3333, 0x4444 };
    r.hash_offsets  = { 0, 2, 3, 4 };
    // Audio CSR (v7): frame0=1, frame1=0, frame2=2 → offsets {0,1,1,3}.
    r.audio_hashes  = { 0xA1A1, 0xA2A2, 0xA3A3 };
    r.audio_offsets = { 0, 1, 1, 3 };
    r.trim_in  = 0;
    r.trim_out = 3;
    return r;
}

int main() {
    namespace fs = std::filesystem;
    fs::path test_directory;
    const auto unique_seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 100 && test_directory.empty(); ++attempt) {
        const fs::path candidate = fs::temp_directory_path() /
            ("ayther_rec_test_" + std::to_string(unique_seed) + "_" +
             std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) test_directory = candidate;
    }
    CHECK(!test_directory.empty(), "crear directorio temporal exclusivo");
    if (test_directory.empty()) return 1;
    const fs::path path = test_directory / "roundtrip.arp";

    const AytherRecording a = make_rec();
    CHECK(a.save(path.string()), "save() debe tener éxito");

    const auto loaded = AytherRecording::load(path.string());
    CHECK(loaded.has_value(), "load() debe devolver una toma");

    if (loaded) {
        const AytherRecording& b = *loaded;
        CHECK(b.game_id == a.game_id, "game_id preservado");
        CHECK(b.inputs == a.inputs, "inputs preservados");
        CHECK(b.frame_count() == 3, "frame_count == 3");

        // Stats de planos (v5/v6).
        CHECK(b.stats.size() == 3, "stats: 3 frames");
        if (b.stats.size() == 3) {
            CHECK(b.stats[0].audio == 1 && b.stats[2].audio == 2, "stats.audio preservado");
            CHECK(b.stats[0].plane_a == 10 && b.stats[2].plane_a == 12, "stats.plane_a (v5)");
            CHECK(b.stats[0].plane_b == 20 && b.stats[1].plane_b == 21, "stats.plane_b (v5)");
            CHECK(b.stats[0].plane_w == 3 && b.stats[2].plane_w == 3,  "stats.plane_w (v6)");
        }

        // Sprite CSR (v3).
        CHECK(b.sprite_hashes == a.sprite_hashes, "sprite_hashes preservados");
        CHECK(b.hash_offsets == a.hash_offsets,   "hash_offsets (CSR sprite) preservados");
        CHECK(b.present(0, 0x1111) && b.present(0, 0x2222), "present(): sprites del frame 0");
        CHECK(!b.present(1, 0x1111), "present(): 0x1111 no está en el frame 1");
        CHECK(b.present(2, 0x4444),  "present(): 0x4444 en el frame 2");

        // Audio CSR (v7) — lo que escanea preview_audio para ubicar el sonido.
        CHECK(b.audio_hashes == a.audio_hashes,   "audio_hashes (v7) preservados");
        CHECK(b.audio_offsets == a.audio_offsets, "audio_offsets (CSR audio) preservados");
        CHECK(b.audio_offsets.size() == b.frame_count() + 1, "audio_offsets: frame_count+1");
        CHECK(b.audio_offsets.back() == b.audio_hashes.size(), "audio CSR cierra en total_hashes");
    }

    // The declared version is a strict schema promise. A truncated mandatory
    // section must fail closed instead of silently loading a degraded take.
    {
        const fs::path malformed = test_directory / "malformed.arp";
        CHECK(a.save(malformed.string()),
              "loader estricto: guardar fixture válido");
        const std::vector<char> valid_bytes = read_bytes(malformed);
        CHECK(!valid_bytes.empty(),
              "loader estricto: fixture contiene bytes");
        if (!valid_bytes.empty()) {
            std::vector<char> truncated = valid_bytes;
            truncated.pop_back();
            CHECK(write_bytes(malformed, truncated),
                  "loader estricto: escribir v8 truncado");
            CHECK(!AytherRecording::load(malformed.string()),
                  "loader estricto: rechazar sección v8 truncada");

            std::vector<char> overflow = valid_bytes;
            std::size_t counts_offset = 0;
            std::uint32_t frames = 0;
            std::uint32_t total = 0;
            const bool located = locate_sprite_counts(
                overflow, counts_offset, frames, total);
            CHECK(located && frames == 3 && total == 4,
                  "loader estricto: localizar CSR sprite del fixture");
            if (located && frames >= 3 && total == 4) {
                CHECK(write_u32_le(overflow, counts_offset, UINT32_MAX) &&
                          write_u32_le(overflow, counts_offset + 4, 5U) &&
                          write_u32_le(overflow, counts_offset + 8, 0U),
                      "loader estricto: construir overflow modular CSR");
                CHECK(write_bytes(malformed, overflow),
                      "loader estricto: escribir CSR malicioso");
                CHECK(!AytherRecording::load(malformed.string()),
                      "loader estricto: rechazar overflow modular CSR");
            }
        }
    }

    // Serialization must enforce the same public limits and reject malformed
    // in-memory CSR data instead of narrowing or wrapping it into a valid-looking
    // file.
    {
        AytherRecording too_many_frames = make_rec();
        too_many_frames.inputs.resize(54'001U, 0U);
        too_many_frames.stats.clear();
        too_many_frames.sprite_hashes.clear();
        too_many_frames.hash_offsets.clear();
        too_many_frames.audio_hashes.clear();
        too_many_frames.audio_offsets.clear();
        CHECK(!too_many_frames.save(
                  (test_directory / "too-many-frames.arp").string()),
              "RNF-3: save rechaza 54.001 frames sin truncar");

        AytherRecording invalid_csr = make_rec();
        invalid_csr.hash_offsets = {0U, UINT32_MAX, 2U, 4U};
        CHECK(!invalid_csr.save(
                  (test_directory / "invalid-csr.arp").string()),
              "RNF-3: save rechaza offsets CSR no monótonos");

        AytherRecording invalid_trim = make_rec();
        invalid_trim.trim_in = 3U;
        invalid_trim.trim_out = 2U;
        CHECK(!invalid_trim.save(
                  (test_directory / "invalid-trim.arp").string()),
              "RF-5.2: save rechaza un rango de trim invertido");

        AytherRecording unsorted_keyframes = make_rec();
        unsorted_keyframes.add_keyframe(2U, {0x20U});
        unsorted_keyframes.add_keyframe(1U, {0x10U});
        CHECK(!unsorted_keyframes.save(
                  (test_directory / "unsorted-keyframes-save.arp").string()),
              "RF-5.2: save rechaza keyframes fuera de orden");

        AytherRecording invalid_keyframe = make_rec();
        invalid_keyframe.keyframes.push_back(
            AytherRecording::Keyframe{1U, 1U, {0U, 0U, 0U, 0U}});
        CHECK(!invalid_keyframe.save(
                  (test_directory / "invalid-keyframe-zstd-save.arp").string()),
              "RF-5.2/RNF-3: save rechaza un keyframe que no es zstd");

        AytherRecording mismatched_keyframe = make_rec();
        mismatched_keyframe.add_keyframe(1U, {0x10U});
        CHECK(mismatched_keyframe.keyframes.size() == 1U,
              "RF-5.2: construir keyframe válido para alterar raw_size");
        if (mismatched_keyframe.keyframes.size() == 1U) {
            ++mismatched_keyframe.keyframes.front().raw_size;
            CHECK(!mismatched_keyframe.save(
                      (test_directory / "mismatched-keyframe-save.arp").string()),
                  "RF-5.2/RNF-3: save rechaza raw_size distinto del zstd");
        }

        AytherRecording sorted_keyframes = make_rec();
        sorted_keyframes.add_keyframe(1U, {0x10U});
        sorted_keyframes.add_keyframe(2U, {0x20U});
        const fs::path unsorted_disk =
            test_directory / "unsorted-keyframes-load.arp";
        CHECK(sorted_keyframes.save(unsorted_disk.string()),
              "RF-5.2: guardar fixture de keyframes ordenados");
        std::vector<char> unsorted_bytes = read_bytes(unsorted_disk);
        std::vector<std::size_t> keyframe_offsets;
        const bool found_keyframes =
            locate_keyframe_frames(unsorted_bytes, keyframe_offsets);
        CHECK(found_keyframes && keyframe_offsets.size() == 2U,
              "RF-5.2: localizar keyframes serializados");
        if (found_keyframes && keyframe_offsets.size() == 2U) {
            std::vector<char> invalid_zstd = unsorted_bytes;
            std::uint32_t compressed_size = 0;
            const std::size_t first_keyframe = keyframe_offsets[0];
            const bool found_compressed_size = read_u32_le(
                invalid_zstd, first_keyframe + 8U, compressed_size);
            CHECK(found_compressed_size && compressed_size != 0U &&
                      compressed_size <=
                          invalid_zstd.size() - first_keyframe - 12U,
                  "RF-5.2: localizar blob zstd del keyframe");
            if (found_compressed_size && compressed_size != 0U &&
                compressed_size <=
                    invalid_zstd.size() - first_keyframe - 12U) {
                std::fill_n(invalid_zstd.begin() +
                                static_cast<std::ptrdiff_t>(first_keyframe + 12U),
                            compressed_size, char{0});
                CHECK(write_bytes(unsorted_disk, invalid_zstd),
                      "RF-5.2: escribir keyframe con blob zstd corrupto");
                CHECK(!AytherRecording::load(unsorted_disk.string()),
                      "RF-5.2/RNF-3: load rechaza keyframe zstd corrupto");
            }

            std::vector<char> mismatched_size = unsorted_bytes;
            std::uint32_t raw_size = 0;
            CHECK(read_u32_le(mismatched_size, first_keyframe + 4U, raw_size) &&
                      write_u32_le(mismatched_size, first_keyframe + 4U,
                                   raw_size + 1U) &&
                      write_bytes(unsorted_disk, mismatched_size),
                  "RF-5.2: escribir keyframe con raw_size incoherente");
            CHECK(!AytherRecording::load(unsorted_disk.string()),
                  "RF-5.2/RNF-3: load rechaza raw_size distinto del zstd");

            CHECK(write_u32_le(unsorted_bytes, keyframe_offsets[0], 2U) &&
                      write_u32_le(unsorted_bytes, keyframe_offsets[1], 1U) &&
                      write_bytes(unsorted_disk, unsorted_bytes),
                  "RF-5.2: construir fixture de keyframes desordenados");
            CHECK(!AytherRecording::load(unsorted_disk.string()),
                  "RF-5.2: load rechaza keyframes fuera de orden");
        }

        const fs::path oversized_patch =
            test_directory / "oversized-patch.arp";
        CHECK(a.save(oversized_patch.string()),
              "RNF-3: guardar base para patch_name sobredimensionado");
        const std::vector<char> patch_before = read_bytes(oversized_patch);
        const std::string oversized_name(64U * 1024U * 1024U, 'n');
        CHECK(!AytherRecording::patch_name(oversized_patch.string(),
                                           oversized_name),
              "RNF-3: patch_name rechaza una toma mayor de 64 MiB");
        CHECK(read_bytes(oversized_patch) == patch_before,
              "RNF-3: patch_name sobredimensionado conserva el destino");
    }

    // slice([1,3)) — la sub-toma rebasa inputs + ambos CSR a 0.
    {
        const AytherRecording s = a.slice(1, 3, a.initial_state);
        CHECK(s.frame_count() == 2, "slice: 2 frames");
        CHECK((s.inputs == std::vector<uint16_t>{ 0x0002, 0x0003 }), "slice: inputs rebasados");
        // Audio del rango [1,3): frame1 vacío, frame2 = {0xA2A2,0xA3A3}.
        CHECK((s.audio_hashes == std::vector<uint64_t>{ 0xA2A2, 0xA3A3 }), "slice: audio_hashes del rango");
        CHECK((s.audio_offsets == std::vector<uint32_t>{ 0, 0, 2 }), "slice: audio CSR rebasado");
        CHECK((s.sprite_hashes == std::vector<uint64_t>{ 0x3333, 0x4444 }), "slice: sprite_hashes del rango");
        CHECK((s.hash_offsets == std::vector<uint32_t>{ 0, 1, 2 }), "slice: sprite CSR rebasado");
    }

    // Toma sin audio CSR (formato viejo): load no debe romperse, audio vacío.
    {
        AytherRecording noaud = make_rec();
        noaud.audio_hashes.clear();
        noaud.audio_offsets.clear();
        const fs::path p2 = test_directory / "noaudio.arp";
        CHECK(noaud.save(p2.string()), "save() sin audio CSR");
        const auto l2 = AytherRecording::load(p2.string());
        CHECK(l2.has_value(), "load() sin audio CSR");
        // Sin audio → no hay audio_hashes (los offsets pueden quedar en ceros,
        // misma convención que el CSR de sprites; lo que importa: sin datos).
        if (l2) CHECK(l2->audio_hashes.empty(), "sin audio CSR → audio_hashes vacío tras load");
        std::error_code ec; fs::remove(p2, ec);
    }

    // patch_name(): renombrar la toma en disco sin recomprimir — el nombre
    // visible (UTF-8, acentos incluidos) cambia y TODO el resto queda intacto.
    {
        CHECK(AytherRecording::patch_name(path.string(), "Men\xC3\xBA nuevo"),
              "patch_name: nombre más largo (con acento)");
        const auto p1 = AytherRecording::load(path.string());
        CHECK(p1.has_value(), "load() tras patch_name");
        if (p1) {
            CHECK(p1->name == "Men\xC3\xBA nuevo", "patch_name: name UTF-8 preservado");
            CHECK(p1->game_id == a.game_id, "patch_name: game_id intacto");
            CHECK(p1->inputs == a.inputs, "patch_name: inputs intactos");
            CHECK(p1->audio_hashes == a.audio_hashes, "patch_name: audio CSR intacto");
            CHECK(p1->sprite_hashes == a.sprite_hashes, "patch_name: sprite CSR intacto");
        }
        CHECK(AytherRecording::patch_name(path.string(), "x"),
              "patch_name: nombre más corto");
        const auto p2 = AytherRecording::load(path.string());
        CHECK(p2 && p2->name == "x" && p2->inputs == a.inputs,
              "patch_name: acortar no corrompe el resto");
        CHECK(AytherRecording::patch_name(path.string(), "x"),
              "patch_name: mismo nombre = no-op exitoso");
        CHECK(!AytherRecording::patch_name(
                  (test_directory / "no_existe.arp").string(), "y"),
              "patch_name: archivo inexistente → false");

#if defined(_WIN32)
        // A publication denied by another reader must report failure without
        // removing or partially rewriting the only confirmed take.
        const HANDLE locked = ::CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(locked != INVALID_HANDLE_VALUE,
              "patch_name: abrir destino sin compartir delete/write");
        if (locked != INVALID_HANDLE_VALUE) {
            CHECK(!AytherRecording::patch_name(path.string(), "blocked"),
                  "RNF-5: fallo de publicación se informa");
            const auto preserved = AytherRecording::load(path.string());
            CHECK(preserved && preserved->name == "x" &&
                      preserved->inputs == a.inputs,
                  "RNF-5: fallo de publicación conserva la toma confirmada");
            CHECK(!has_replacement_artifact(path),
                  "RNF-5: fallo de publicación limpia temporales y backups");
            ::CloseHandle(locked);
        }
#endif
    }

    // patch_name must validate the format version before rewriting anything.
    // A future or pre-v2 payload remains byte-for-byte untouched.
    {
        const fs::path incompatible = test_directory / "incompatible.arp";
        CHECK(a.save(incompatible.string()),
              "patch_name versión incompatible: guardar base");
        const std::vector<char> valid_bytes = read_bytes(incompatible);
        CHECK(valid_bytes.size() >= 8,
              "patch_name versión incompatible: header disponible");
        if (valid_bytes.size() >= 8) {
            for (const uint32_t version : {1U, 0xFFFFFFFFU}) {
                std::vector<char> invalid_bytes = valid_bytes;
                invalid_bytes[4] = static_cast<char>(version);
                invalid_bytes[5] = static_cast<char>(version >> 8U);
                invalid_bytes[6] = static_cast<char>(version >> 16U);
                invalid_bytes[7] = static_cast<char>(version >> 24U);
                CHECK(write_bytes(incompatible, invalid_bytes),
                      "patch_name versión incompatible: escribir fixture");
                CHECK(!AytherRecording::patch_name(incompatible.string(), "bad"),
                      "patch_name rechaza versión fuera de rango");
                CHECK(read_bytes(incompatible) == invalid_bytes,
                      "patch_name conserva bytes de versión incompatible");
                CHECK(!has_replacement_artifact(incompatible),
                      "versión incompatible no deja artefactos de reemplazo");
            }
        }
    }

    // save() must prepare a complete image before touching an existing take.
    // Force the final publication to fail after ordinary in-place writes would
    // already have been allowed, then verify rollback and artifact cleanup.
    {
        const fs::path save_failure_directory =
            test_directory / "save-publication-failure";
        std::error_code ec;
        CHECK(fs::create_directory(save_failure_directory, ec),
              "save atómico: crear directorio de prueba");
        const fs::path destination = save_failure_directory / "existing.arp";
        CHECK(a.save(destination.string()),
              "save atómico: guardar toma confirmada");
        const std::vector<char> confirmed_bytes = read_bytes(destination);

        AytherRecording replacement = make_rec();
        replacement.name = "must-not-publish";
        replacement.initial_state.push_back(0xFD);
        bool publication_failure_exercised = false;
#if defined(_WIN32)
        const HANDLE publishing_blocker = ::CreateFileW(
            destination.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(publishing_blocker != INVALID_HANDLE_VALUE,
              "save atómico: bloquear sólo delete/rename");
        if (publishing_blocker != INVALID_HANDLE_VALUE) {
            publication_failure_exercised = true;
            CHECK(!replacement.save(destination.string()),
                  "save atómico: informar fallo de publicación");
            ::CloseHandle(publishing_blocker);
        }
#else
        // Root bypasses directory mode bits; skip this fault injection in that
        // exceptional environment instead of turning a valid save into a
        // platform-dependent failure.
        if (::geteuid() != 0) {
            const bool made_read_only =
                ::chmod(save_failure_directory.c_str(), 0555) == 0;
            CHECK(made_read_only,
                  "save atómico: impedir creación/publicación en directorio");
            if (made_read_only) {
                publication_failure_exercised = true;
                CHECK(!replacement.save(destination.string()),
                      "save atómico: informar fallo de publicación");
                CHECK(::chmod(save_failure_directory.c_str(), 0755) == 0,
                      "save atómico: restaurar permisos del directorio");
            }
        }
#endif
        if (publication_failure_exercised) {
            CHECK(read_bytes(destination) == confirmed_bytes,
                  "save atómico: fallo conserva toma confirmada byte a byte");
            CHECK(!has_replacement_artifact(destination),
                  "save atómico: fallo no deja temporales ni backups");
        }
    }

    // The sidecar lock is acquired before reading and is shared with save(),
    // so cooperating writers cannot overwrite a newer destination snapshot.
    {
        const fs::path serialized = test_directory / "serialized.arp";
        CHECK(a.save(serialized.string()),
              "bloqueo por destino: guardar toma inicial");

        AytherRecording newer = make_rec();
        newer.name = "newer-snapshot";
        newer.initial_state.push_back(0xEE);
        const fs::path newer_fixture = test_directory / "newer-fixture.arp";
        CHECK(newer.save(newer_fixture.string()),
              "bloqueo por destino: preparar generación más nueva");
        const std::vector<char> newer_bytes = read_bytes(newer_fixture);

        HeldDestinationLock held(serialized);
        CHECK(held.locked(), "bloqueo por destino: adquirir lock externo");
        if (held.locked()) {
            std::mutex state_mutex;
            std::condition_variable state_changed;
            bool entered = false;
            bool finished = false;
            bool patch_result = false;
            std::thread patcher([&] {
                {
                    const std::lock_guard lock(state_mutex);
                    entered = true;
                }
                state_changed.notify_all();
                patch_result =
                    AytherRecording::patch_name(serialized.string(), "locked");
                {
                    const std::lock_guard lock(state_mutex);
                    finished = true;
                }
                state_changed.notify_all();
            });
            {
                std::unique_lock lock(state_mutex);
                state_changed.wait(lock, [&] { return entered; });
                CHECK(!state_changed.wait_for(
                          lock, std::chrono::milliseconds(200),
                          [&] { return finished; }),
                      "bloqueo por destino: patch_name espera antes de leer");
            }
            CHECK(write_bytes(serialized, newer_bytes),
                  "bloqueo por destino: publicar generación durante la espera");
            held.release();
            patcher.join();
            CHECK(patch_result,
                  "bloqueo por destino: patch_name continúa tras liberar");
            const auto patched = AytherRecording::load(serialized.string());
            CHECK(patched && patched->name == "locked" &&
                      patched->initial_state == newer.initial_state,
                  "bloqueo por destino: patch_name leyó la generación más nueva");
        }

        AytherRecording updated = make_rec();
        updated.name = "saved-under-lock";
        updated.initial_state.push_back(0xEF);
        check_operation_waits_for_destination_lock(
            serialized, [&] { return updated.save(serialized.string()); },
            "bloqueo por destino: save espera el mismo lock del SO",
            "bloqueo por destino: save continúa tras liberar");
        const auto saved = AytherRecording::load(serialized.string());
        CHECK(saved && saved->name == updated.name &&
                  saved->initial_state == updated.initial_state,
              "bloqueo por destino: save publicó la imagen nueva completa");

#if defined(_WIN32)
        const fs::path equivalent_spelling = serialized.string() + ".";
        check_operation_waits_for_destination_lock(
            serialized,
            [&] {
                return AytherRecording::patch_name(
                    equivalent_spelling.string(), "alias-serialized");
            },
            "bloqueo por identidad: alias Win32 espera el mismo lock",
            "bloqueo por identidad: alias continúa tras liberar");
        const auto aliased = AytherRecording::load(serialized.string());
        CHECK(aliased && aliased->name == "alias-serialized",
              "bloqueo por identidad: alias Win32 publica en el destino real");
#endif

        const fs::path hardlink = test_directory / "serialized-hardlink.arp";
        std::error_code hardlink_error;
        fs::create_hard_link(serialized, hardlink, hardlink_error);
        if (!hardlink_error) {
            CHECK(ayther::recording_detail::destination_lock_identity(serialized) ==
                      ayther::recording_detail::destination_lock_identity(hardlink),
                  "RNF-5: dos hardlinks comparten identidad de grabación");
            CHECK(ayther::recording_detail::destination_identity_lock_path(
                      serialized) ==
                      ayther::recording_detail::destination_identity_lock_path(
                          hardlink),
                  "RNF-5: dos hardlinks comparten el lock interproceso efectivo");
            check_operation_waits_for_destination_lock(
                serialized,
                [&] {
                    return AytherRecording::patch_name(
                        hardlink.string(), "hardlink-serialized");
                },
                "RNF-5: escritor por hardlink espera el lock del mismo inode",
                "RNF-5: escritor por hardlink continúa tras liberar");
            const auto hardlink_take = AytherRecording::load(hardlink.string());
            CHECK(hardlink_take && hardlink_take->name == "hardlink-serialized",
                  "RNF-5: escritor serializado publica por el hardlink");
        }

#if !defined(_WIN32)
        const fs::path identity_lock_before_env_change =
            ayther::recording_detail::destination_identity_lock_path(serialized);
        const char* original_tmpdir_value = std::getenv("TMPDIR");
        const bool had_tmpdir = original_tmpdir_value != nullptr;
        const std::string original_tmpdir =
            had_tmpdir ? original_tmpdir_value : std::string{};
        const fs::path alternate_tmpdir = test_directory / "alternate-tmp";
        std::error_code tmpdir_error;
        (void)fs::create_directory(alternate_tmpdir, tmpdir_error);
        CHECK(!tmpdir_error &&
                  ::setenv("TMPDIR", alternate_tmpdir.c_str(), 1) == 0,
              "RNF-5: preparar TMPDIR alternativo para identidad de lock");
        const fs::path identity_lock_after_env_change =
            ayther::recording_detail::destination_identity_lock_path(serialized);
        if (had_tmpdir)
            (void)::setenv("TMPDIR", original_tmpdir.c_str(), 1);
        else
            (void)::unsetenv("TMPDIR");
        CHECK(identity_lock_after_env_change == identity_lock_before_env_change,
              "RNF-5: el lock POSIX es independiente de TMPDIR entre procesos");
#endif
    }

    // A path referring to a symlink/reparse point is rejected. Replacing the
    // link itself would report success without changing the requested target.
    {
        const fs::path target = test_directory / "symlink-target.arp";
        const fs::path link = test_directory / "symlink.arp";
        CHECK(a.save(target.string()), "symlink: guardar destino real");
        const std::vector<char> target_before = read_bytes(target);
        std::error_code ec;
        fs::create_symlink(target.filename(), link, ec);
        if (!ec) {
            CHECK(!AytherRecording::patch_name(link.string(), "through-link"),
                  "patch_name rechaza symlink/reparse point");
            AytherRecording overwrite = make_rec();
            overwrite.name = "save-through-link";
            overwrite.initial_state.push_back(0xFC);
            CHECK(!overwrite.save(link.string()),
                  "save rechaza symlink/reparse point");
            CHECK(fs::is_symlink(fs::symlink_status(link, ec)),
                  "save/patch_name conservan la entrada symlink");
            CHECK(read_bytes(target) == target_before,
                  "save/patch_name conservan el destino del symlink");
        }
    }

    // Preserve filesystem metadata supported by the platform replacement
    // primitive in addition to keeping the serialized payload intact.
    {
        const fs::path metadata = test_directory / "metadata.arp";
        CHECK(a.save(metadata.string()), "metadata: guardar toma inicial");
#if defined(_WIN32)
        const DWORD attributes_before = ::GetFileAttributesW(metadata.c_str());
        CHECK(attributes_before != INVALID_FILE_ATTRIBUTES,
              "metadata: consultar atributos Windows");
        if (attributes_before != INVALID_FILE_ATTRIBUTES) {
            CHECK(::SetFileAttributesW(metadata.c_str(),
                                       attributes_before | FILE_ATTRIBUTE_HIDDEN) != FALSE,
                  "metadata: fijar atributo hidden");
        }
        std::filesystem::path alternate_stream = metadata;
        alternate_stream += L":ayther-recording-test";
        const std::string stream_payload = "preserve alternate stream";
        const bool stream_supported = write_bytes(
            alternate_stream,
            std::vector<char>(stream_payload.begin(), stream_payload.end()));

        CHECK(AytherRecording::patch_name(metadata.string(), "metadata-win"),
              "metadata: patch_name Windows");
        const DWORD attributes_after = ::GetFileAttributesW(metadata.c_str());
        if (attributes_before != INVALID_FILE_ATTRIBUTES) {
            CHECK((attributes_after & FILE_ATTRIBUTE_HIDDEN) != 0,
                  "metadata: preservar atributos Windows");
        }
        if (stream_supported) {
            const std::vector<char> stream_after = read_bytes(alternate_stream);
            CHECK(std::string(stream_after.begin(), stream_after.end()) == stream_payload,
                  "metadata: preservar alternate data stream");
        }
        if (attributes_before != INVALID_FILE_ATTRIBUTES)
            (void)::SetFileAttributesW(metadata.c_str(), attributes_before);
#else
        CHECK(::chmod(metadata.c_str(), 0640) == 0,
              "metadata: fijar modo POSIX");
        struct stat metadata_before {};
        CHECK(::stat(metadata.c_str(), &metadata_before) == 0,
              "metadata: consultar stat POSIX");
#if defined(__linux__)
        constexpr char xattr_name[] = "user.ayther_recording_test";
        constexpr char xattr_value[] = "preserve-xattr";
        const bool xattr_supported =
            ::setxattr(metadata.c_str(), xattr_name, xattr_value,
                       sizeof(xattr_value) - 1, 0) == 0;
#endif
        CHECK(AytherRecording::patch_name(metadata.string(), "metadata-posix"),
              "metadata: patch_name POSIX");
        struct stat metadata_after {};
        CHECK(::stat(metadata.c_str(), &metadata_after) == 0,
              "metadata: consultar stat tras patch");
        CHECK(metadata_after.st_uid == metadata_before.st_uid &&
                  metadata_after.st_gid == metadata_before.st_gid &&
                  (metadata_after.st_mode & 07777) ==
                      (metadata_before.st_mode & 07777),
              "metadata: preservar uid/gid/modo POSIX");
#if defined(__linux__)
        if (xattr_supported) {
            char value[64]{};
            const ssize_t value_size =
                ::getxattr(metadata.c_str(), xattr_name, value, sizeof(value));
            CHECK(value_size == static_cast<ssize_t>(sizeof(xattr_value) - 1) &&
                      std::string(value, static_cast<std::size_t>(value_size)) ==
                          xattr_value,
                  "metadata: preservar xattr POSIX");
        }
#endif
#endif
    }

    // RNF-5: two writers must never share a predictable temporary file. Both
    // completed replacements are valid (last writer wins), and neither may
    // publish the other writer's bytes or corrupt the take.
    {
        const fs::path concurrent = test_directory / "concurrent.arp";
        const fs::path legacy_temporary = concurrent.string() + ".tmp";
        std::error_code ec;
        fs::remove(concurrent, ec);
        CHECK(a.save(concurrent.string()),
              "patch_name concurrente: guardar toma inicial");
        {
            std::ofstream sentinel(legacy_temporary, std::ios::binary);
            sentinel << "sentinel: never truncate or publish";
        }
        const std::string name_a(1U << 20U, 'a');
        const std::string name_b(1U << 20U, 'b');
        std::barrier start(3);
        bool ok_a = false;
        bool ok_b = false;
        std::thread writer_a([&] {
            start.arrive_and_wait();
            ok_a = AytherRecording::patch_name(concurrent.string(), name_a);
        });
        std::thread writer_b([&] {
            start.arrive_and_wait();
            ok_b = AytherRecording::patch_name(concurrent.string(), name_b);
        });
        start.arrive_and_wait();
        writer_a.join();
        writer_b.join();
        CHECK(ok_a && ok_b,
              "RNF-5: dos escritores publican desde temporales independientes");
        const auto final = AytherRecording::load(concurrent.string());
        CHECK(final && (final->name == name_a || final->name == name_b),
              "RNF-5: la toma concurrente contiene exactamente un nombre completo");
        CHECK(final && final->inputs == a.inputs &&
                  final->sprite_hashes == a.sprite_hashes &&
                  final->audio_hashes == a.audio_hashes,
              "RNF-5: el reemplazo concurrente conserva el resto de la toma");
        std::ifstream sentinel(legacy_temporary, std::ios::binary);
        const std::string sentinel_bytes((std::istreambuf_iterator<char>(sentinel)),
                                         std::istreambuf_iterator<char>());
        CHECK(sentinel_bytes == "sentinel: never truncate or publish",
              "RNF-5: el temporal legado <destino>.tmp queda intacto");
        CHECK(!has_replacement_artifact(concurrent),
              "RNF-5: una publicación completa no deja artefactos exclusivos");
        fs::remove(legacy_temporary, ec);
        fs::remove(concurrent, ec);
    }

    std::error_code ec; fs::remove_all(test_directory, ec);

    if (g_fail) { std::fprintf(stderr, "%d checks fallaron\n", g_fail); return 1; }
    std::printf("ayther_recording_test: OK\n");
    return 0;
}
