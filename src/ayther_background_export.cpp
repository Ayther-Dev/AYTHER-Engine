// ---------------------------------------------------------------------------
// ayther_background_export.cpp — BackgroundExporter (see ayther_background_export.h).
//
// STATUS: contract/skeleton, NOT yet wired into the root CMakeLists.txt. It depends
// only on the ayther_bg_stitcher_* FFI (done + validated in tools/background_spike),
// std, and stb_image_write (declarations; the STB_IMAGE_WRITE_IMPLEMENTATION TU
// lives in the Lab). It type-checks standalone:
//   clang-cl /std:c++20 /Zs -I include/ayther -I build/vcpkg_installed/x64-windows/include \
//            src/ayther_background_export.cpp
// Left out of the engine build until the Deliver flow calls it.
// ---------------------------------------------------------------------------

#include "ayther_background_export.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <stb_image_write.h>   // declarations only; impl TU defined in the Lab

namespace ayther {

static constexpr uint64_t kFnvSeed = 1469598103934665603ull;
static inline uint64_t fnv1a(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) { h ^= (v & 0xff); h *= 1099511628211ull; v >>= 8; }
    return h;
}

LayerImage BackgroundExporter::build(const AytherBgStitcher* st, uint8_t plane,
                                     const TileDecodeFn& decode) {
    LayerImage img;
    img.plane = plane;
    if (!st) return img;

    int32_t b[4];
    if (!ayther_bg_stitcher_bounds(st, plane, b)) return img;   // empty plane → 0×0
    const int32_t minx = b[0], miny = b[1], maxx = b[2], maxy = b[3];
    const uint32_t wt = uint32_t(maxx - minx + 1), ht = uint32_t(maxy - miny + 1);

    img.origin_x  = minx;
    img.origin_y  = miny;
    img.width_px  = wt * 8;
    img.height_px = ht * 8;
    img.animated  = uint32_t(ayther_bg_stitcher_animated_cells(st, plane));
    img.rgba.assign(size_t(img.width_px) * img.height_px * 4, 0);   // transparent holes

    uint64_t hash = kFnvSeed;
    uint8_t  tile[8 * 8 * 4];
    for (int32_t y = miny; y <= maxy; ++y) {
        for (int32_t x = minx; x <= maxx; ++x) {
            uint32_t code = 0;
            if (!ayther_bg_stitcher_get(st, plane, x, y, &code)) continue;   // hole
            ++img.cell_count;
            hash = fnv1a(hash, (uint64_t(uint32_t(x - minx)) << 40)
                             | (uint64_t(uint32_t(y - miny)) << 16)
                             | uint64_t(code & 0xffffu));

            const uint16_t pattern = uint16_t(code & 0x7ffu);
            const bool     hflip   = ((code >> 11) & 1u) != 0;
            const bool     vflip   = ((code >> 12) & 1u) != 0;
            const uint8_t  pal     = uint8_t((code >> 13) & 3u);
            decode(pattern, pal, hflip, vflip, tile);

            const uint32_t dx = uint32_t(x - minx) * 8u, dy = uint32_t(y - miny) * 8u;
            for (uint32_t r = 0; r < 8; ++r) {
                std::memcpy(&img.rgba[(size_t(dy + r) * img.width_px + dx) * 4],
                            &tile[r * 8 * 4], 8u * 4u);
            }
        }
    }
    img.layout_hash = hash;
    return img;
}

std::string BackgroundExporter::filename(const LayerImage& img) {
    const char plane = img.plane == 0 ? 'A' : (img.plane == 1 ? 'B' : 'W');
    char buf[96];
    std::snprintf(buf, sizeof(buf), "bg_%c_t%dx%d_%016llx.png",
                  plane, img.origin_x, img.origin_y,
                  static_cast<unsigned long long>(img.layout_hash));
    return std::string(buf);
}

Result<void> BackgroundExporter::write_png(const LayerImage& img, const std::string& path) {
    if (img.rgba.empty() || img.width_px == 0 || img.height_px == 0)
        return Result<void>::fail(ErrorCode::BadFormat, "background export: empty layer image");
    const int rc = stbi_write_png(path.c_str(), int(img.width_px), int(img.height_px),
                                  4, img.rgba.data(), int(img.width_px) * 4);
    if (rc == 0)
        return Result<void>::fail(ErrorCode::Io, "background export: PNG write failed: " + path);
    return Result<void>::ok();
}

}  // namespace ayther
