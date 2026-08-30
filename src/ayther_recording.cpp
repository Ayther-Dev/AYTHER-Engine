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
//   total_hashes × u64                                  flat sprite-hash history
//   frame_count × u32                                   per-frame hash counts (CSR)
//   --- v4 (R7e) ---
//   u32 kf_count
//   kf_count × (u32 frame, u32 raw_size, u32 comp_size, comp blob)   baked keyframes
//   --- v7 (audio por sonido) ---
//   u32 total_audio_hashes
//   total_audio_hashes × u64                            flat audio-hash history
//   frame_count × u32                                   per-frame audio counts (CSR)
//   --- v8 (algo de hash de sprites) ---
//   u32 hash_algo        (kSpriteHashAlgo con que se capturó la historia; <v8 = 0)
// ---------------------------------------------------------------------------
#include "ayther_recording.h"
#include "log.h"

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ayther {

namespace {

constexpr char     kMagic[4] = { 'A', 'R', 'P', '1' };
constexpr uint32_t kVersion  = 8;    // v8: + hash_algo (algoritmo de la historia de sprites)
constexpr int      kZstdLevel = 9;   // takes are saved rarely — favour ratio

void put_u32(std::ostream& o, uint32_t v) {
    uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
    o.write(reinterpret_cast<const char*>(b), 4);
}
void put_u16(std::ostream& o, uint16_t v) {
    uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
    o.write(reinterpret_cast<const char*>(b), 2);
}
void put_u64(std::ostream& o, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (8 * i));
    o.write(reinterpret_cast<const char*>(b), 8);
}
bool get_u32(std::istream& i, uint32_t& v) {
    uint8_t b[4];
    if (!i.read(reinterpret_cast<char*>(b), 4)) return false;
    v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}
bool get_u64(std::istream& i, uint64_t& v) {
    uint8_t b[8];
    if (!i.read(reinterpret_cast<char*>(b), 8)) return false;
    v = 0;
    for (int k = 0; k < 8; ++k) v |= uint64_t(b[k]) << (8 * k);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------
bool AytherRecording::save(const std::string& path) const {
    if (empty()) { ayther::log::write(ayther::log::Severity::Warning,
        "recording", "refuse_save_empty_take",
        "refuse to save empty take"); return false; }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { ayther::log::write(ayther::log::Severity::Error,
        "recording", "cannot_open",
        "cannot open %s",
        path.c_str()); return false; }

    // Compress the initial savestate.
    const size_t bound = ZSTD_compressBound(initial_state.size());
    std::vector<uint8_t> comp(bound);
    const size_t n = ZSTD_compress(comp.data(), bound,
                                   initial_state.data(), initial_state.size(), kZstdLevel);
    if (ZSTD_isError(n)) {
        ayther::log::write(ayther::log::Severity::Error,
            "recording", "compress_failed",
            "compress failed: %s",
            ZSTD_getErrorName(n));
        return false;
    }
    comp.resize(n);

    f.write(kMagic, 4);
    put_u32(f, kVersion);
    put_u32(f, static_cast<uint32_t>(game_id.size())); f.write(game_id.data(), game_id.size());
    put_u32(f, static_cast<uint32_t>(name.size()));    f.write(name.data(),    name.size());
    put_u32(f, frame_count());
    put_u32(f, static_cast<uint32_t>(initial_state.size()));
    put_u32(f, static_cast<uint32_t>(comp.size()));
    f.write(reinterpret_cast<const char*>(comp.data()), comp.size());
    for (uint16_t in : inputs) put_u16(f, in);

    // v2: trim marks + per-frame occurrence stats.
    put_u32(f, trim_in);
    put_u32(f, trim_out ? trim_out : frame_count());
    for (uint32_t i = 0; i < frame_count(); ++i) {
        const FrameStat s = (i < stats.size()) ? stats[i] : FrameStat{};
        put_u16(f, s.sprites); put_u16(f, s.tiles); put_u16(f, s.audio);
        put_u16(f, s.plane_a); put_u16(f, s.plane_b);   // v5
        put_u16(f, s.plane_w);                          // v6
    }

    // v3: per-frame sprite-hash history (CSR). Absent (all-zero counts) when not
    // captured — the flat array is empty and every per-frame count is 0.
    const bool have_hashes = hash_offsets.size() == frame_count() + 1;
    put_u32(f, have_hashes ? static_cast<uint32_t>(sprite_hashes.size()) : 0u);
    if (have_hashes) {
        for (uint64_t h : sprite_hashes) put_u64(f, h);
        for (uint32_t i = 0; i < frame_count(); ++i)
            put_u32(f, hash_offsets[i + 1] - hash_offsets[i]);
    } else {
        for (uint32_t i = 0; i < frame_count(); ++i) put_u32(f, 0u);
    }

    // v4: baked replay keyframes (ya comprimidos en memoria).
    put_u32(f, static_cast<uint32_t>(keyframes.size()));
    for (const Keyframe& kf : keyframes) {
        put_u32(f, kf.frame);
        put_u32(f, kf.raw_size);
        put_u32(f, static_cast<uint32_t>(kf.comp.size()));
        f.write(reinterpret_cast<const char*>(kf.comp.data()), kf.comp.size());
    }

    // v7: per-frame audio-hash history (CSR). Mismo patrón que v3 (sprites):
    // ausente (counts en cero) cuando no se capturó.
    const bool have_audio = audio_offsets.size() == frame_count() + 1;
    put_u32(f, have_audio ? static_cast<uint32_t>(audio_hashes.size()) : 0u);
    if (have_audio) {
        for (uint64_t h : audio_hashes) put_u64(f, h);
        for (uint32_t i = 0; i < frame_count(); ++i)
            put_u32(f, audio_offsets[i + 1] - audio_offsets[i]);
    } else {
        for (uint32_t i = 0; i < frame_count(); ++i) put_u32(f, 0u);
    }

    // v8: algoritmo de hash con que se capturó la historia de sprites.
    put_u32(f, hash_algo);

    ayther::log::write(ayther::log::Severity::Info,
        "recording", "saved_frames_state_b",
        "saved %s  (%u frames, state %zu→%zu B, %zu sprite-hashes, %zu keyframes)",
        path.c_str(),
        frame_count(),
        initial_state.size(),
        comp.size(),
        have_hashes ? sprite_hashes.size() : 0u,
        keyframes.size());
    return f.good();
}

// ---------------------------------------------------------------------------
// patch_name — reescritura del nombre visible en el header, resto intacto
// ---------------------------------------------------------------------------
bool AytherRecording::patch_name(const std::string& path,
                                 const std::string& new_name) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<char> buf((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    in.close();

    auto u32_at = [&buf](size_t off) {
        return uint32_t(uint8_t(buf[off]))              |
               (uint32_t(uint8_t(buf[off + 1])) << 8)   |
               (uint32_t(uint8_t(buf[off + 2])) << 16)  |
               (uint32_t(uint8_t(buf[off + 3])) << 24);
    };
    // Header: magic[4] + u32 version + (u32,game_id) + (u32,name) + …
    if (buf.size() < 16 || std::memcmp(buf.data(), kMagic, 4) != 0) return false;
    const size_t gid_len  = u32_at(8);
    const size_t name_pos = 12 + gid_len;
    if (name_pos + 4 > buf.size()) return false;
    const size_t name_len = u32_at(name_pos);
    const size_t rest     = name_pos + 4 + name_len;
    if (rest > buf.size()) return false;
    if (name_len == new_name.size() &&
        std::memcmp(buf.data() + name_pos + 4, new_name.data(), name_len) == 0)
        return true;

    // tmp + rename: un fallo a mitad de escritura no corrompe la toma.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(buf.data(), static_cast<std::streamsize>(name_pos));
        put_u32(out, static_cast<uint32_t>(new_name.size()));
        out.write(new_name.data(),
                  static_cast<std::streamsize>(new_name.size()));
        out.write(buf.data() + rest,
                  static_cast<std::streamsize>(buf.size() - rest));
        if (!out.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------
std::optional<AytherRecording> AytherRecording::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;

    char magic[4];
    if (!f.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return std::nullopt;

    uint32_t version = 0;
    if (!get_u32(f, version) || version < 2 || version > kVersion) return std::nullopt;

    AytherRecording rec;
    auto read_str = [&](std::string& s) -> bool {
        uint32_t len = 0;
        if (!get_u32(f, len)) return false;
        s.resize(len);
        return len == 0 || static_cast<bool>(f.read(s.data(), len));
    };
    if (!read_str(rec.game_id)) return std::nullopt;
    if (!read_str(rec.name))    return std::nullopt;

    uint32_t frames = 0, raw_size = 0, comp_size = 0;
    if (!get_u32(f, frames) || !get_u32(f, raw_size) || !get_u32(f, comp_size))
        return std::nullopt;

    std::vector<uint8_t> comp(comp_size);
    if (comp_size && !f.read(reinterpret_cast<char*>(comp.data()), comp_size))
        return std::nullopt;

    rec.initial_state.resize(raw_size);
    const size_t n = ZSTD_decompress(rec.initial_state.data(), raw_size,
                                     comp.data(), comp_size);
    if (ZSTD_isError(n) || n != raw_size) {
        ayther::log::write(ayther::log::Severity::Error,
            "recording", "decompress_failed",
            "decompress failed for %s",
            path.c_str());
        return std::nullopt;
    }

    rec.inputs.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        uint8_t b[2];
        if (!f.read(reinterpret_cast<char*>(b), 2)) return std::nullopt;
        rec.inputs[i] = uint16_t(b[0]) | (uint16_t(b[1]) << 8);
    }

    // v2: trim marks + occurrence stats.
    auto get_u16 = [&](uint16_t& v) -> bool {
        uint8_t b[2];
        if (!f.read(reinterpret_cast<char*>(b), 2)) return false;
        v = uint16_t(b[0]) | (uint16_t(b[1]) << 8);
        return true;
    };
    if (!get_u32(f, rec.trim_in) || !get_u32(f, rec.trim_out)) {
        rec.trim_in = 0; rec.trim_out = frames;   // tolerate a v1-style file
    } else {
        rec.stats.resize(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            FrameStat s{};
            if (!get_u16(s.sprites) || !get_u16(s.tiles) || !get_u16(s.audio)) {
                rec.stats.clear();   // truncated stats — drop them, keep the take usable
                break;
            }
            // v5: cobertura de planos A/B; v6: + Window (tomas viejas → 0).
            if (version >= 5 && (!get_u16(s.plane_a) || !get_u16(s.plane_b))) {
                rec.stats.clear();
                break;
            }
            if (version >= 6 && !get_u16(s.plane_w)) {
                rec.stats.clear();
                break;
            }
            rec.stats[i] = s;
        }
    }
    if (rec.trim_out == 0 || rec.trim_out > frames) rec.trim_out = frames;

    // v3: per-frame sprite-hash history (CSR).
    if (version >= 3) {
        uint32_t total = 0;
        if (get_u32(f, total)) {
            rec.sprite_hashes.resize(total);
            bool ok = true;
            for (uint32_t i = 0; i < total && ok; ++i) ok = get_u64(f, rec.sprite_hashes[i]);
            if (ok) {
                rec.hash_offsets.assign(frames + 1, 0);
                for (uint32_t i = 0; i < frames && ok; ++i) {
                    uint32_t cnt = 0;
                    ok = get_u32(f, cnt);
                    rec.hash_offsets[i + 1] = rec.hash_offsets[i] + cnt;
                }
                // Sanity: the CSR must cover exactly `total` hashes.
                if (!ok || rec.hash_offsets.back() != total) {
                    rec.sprite_hashes.clear();
                    rec.hash_offsets.clear();
                }
            } else {
                rec.sprite_hashes.clear();
            }
        }
    }

    // v4: baked replay keyframes (se guardan comprimidos — descompresión lazy).
    if (version >= 4) {
        uint32_t kf_count = 0;
        if (get_u32(f, kf_count) && kf_count > 0 && kf_count <= frames + 1) {
            rec.keyframes.reserve(kf_count);
            for (uint32_t i = 0; i < kf_count; ++i) {
                AytherRecording::Keyframe kf;
                uint32_t comp_size = 0;
                if (!get_u32(f, kf.frame) || !get_u32(f, kf.raw_size) ||
                    !get_u32(f, comp_size)) { rec.keyframes.clear(); break; }
                kf.comp.resize(comp_size);
                if (comp_size && !f.read(reinterpret_cast<char*>(kf.comp.data()), comp_size)) {
                    rec.keyframes.clear(); break;          // truncado → toma usable sin keyframes
                }
                rec.keyframes.push_back(std::move(kf));
            }
        }
    }

    // v7: per-frame audio-hash history (CSR). Mismo patrón que v3 (sprites).
    if (version >= 7) {
        uint32_t total = 0;
        if (get_u32(f, total)) {
            rec.audio_hashes.resize(total);
            bool ok = true;
            for (uint32_t i = 0; i < total && ok; ++i) ok = get_u64(f, rec.audio_hashes[i]);
            if (ok) {
                rec.audio_offsets.assign(frames + 1, 0);
                for (uint32_t i = 0; i < frames && ok; ++i) {
                    uint32_t cnt = 0;
                    ok = get_u32(f, cnt);
                    rec.audio_offsets[i + 1] = rec.audio_offsets[i] + cnt;
                }
                if (!ok || rec.audio_offsets.back() != total) {
                    rec.audio_hashes.clear();
                    rec.audio_offsets.clear();
                }
            } else {
                rec.audio_hashes.clear();
            }
        }
    }

    // v8: algoritmo de hash de la historia de sprites. Ausente (toma vieja) = 0
    // → el Lab la re-hornea con el hasher vigente al cargarla.
    rec.hash_algo = 0;
    if (version >= 8) {
        uint32_t algo = 0;
        if (get_u32(f, algo)) rec.hash_algo = algo;
    }

    ayther::log::write(ayther::log::Severity::Info,
        "recording", "loaded_frames_keyframes_hash",
        "loaded %s  (%u frames%s, %zu keyframes, hash_algo %u)",
        path.c_str(),
        frames,
        rec.hash_offsets.empty() ? "" : ", +hash history",
        rec.keyframes.size(),
        rec.hash_algo);
    return rec;
}

// ---------------------------------------------------------------------------
// Keyframes horneados (R7e): compresión/descompresión por separado.
// ---------------------------------------------------------------------------
void AytherRecording::add_keyframe(uint32_t frame, const std::vector<uint8_t>& raw_state) {
    if (raw_state.empty()) return;
    const size_t bound = ZSTD_compressBound(raw_state.size());
    Keyframe kf;
    kf.frame    = frame;
    kf.raw_size = static_cast<uint32_t>(raw_state.size());
    kf.comp.resize(bound);
    const size_t n = ZSTD_compress(kf.comp.data(), bound,
                                   raw_state.data(), raw_state.size(), kZstdLevel);
    if (ZSTD_isError(n)) return;
    kf.comp.resize(n);
    keyframes.push_back(std::move(kf));
}

bool AytherRecording::decompress_keyframe(size_t idx, std::vector<uint8_t>& out) const {
    if (idx >= keyframes.size()) return false;
    const Keyframe& kf = keyframes[idx];
    out.resize(kf.raw_size);
    const size_t n = ZSTD_decompress(out.data(), kf.raw_size, kf.comp.data(), kf.comp.size());
    return !ZSTD_isError(n) && n == kf.raw_size;
}

// ---------------------------------------------------------------------------
// slice — sub-toma [begin, end) rebasada a 0 (Fase C: dividir tomas)
// ---------------------------------------------------------------------------
AytherRecording AytherRecording::slice(uint32_t begin, uint32_t end,
                                       std::vector<uint8_t> state) const {
    AytherRecording r;
    r.game_id       = game_id;
    r.name          = name;            // el caller renombra el tail
    r.hash_algo     = hash_algo;       // la sub-toma hereda el algo de su historia
    r.initial_state = std::move(state);
    r.inputs.assign(inputs.begin() + begin, inputs.begin() + end);
    if (stats.size() >= end)           // stats parciales -> se omiten
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
    const uint32_t a    = std::clamp(trim_in, begin, end);
    const uint32_t b    = std::clamp(tout,    begin, end);
    if (b > a) { r.trim_in = a - begin; r.trim_out = b - begin; }
    else       { r.trim_in = 0;         r.trim_out = end - begin; }
    return r;
}

}  // namespace ayther
