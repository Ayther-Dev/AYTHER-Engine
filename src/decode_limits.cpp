// ---------------------------------------------------------------------------
// decode_limits.cpp — header inspection for untrusted encoded images.
//
// stbi_info_from_memory parses the header only. That is the whole reason this
// exists: the declared size has to be known BEFORE stbi_load_from_memory is
// allowed to allocate it.
// ---------------------------------------------------------------------------
#include "decode_limits.h"

#include "log.h"

#include <stb_image.h>

namespace ayther::limits {

bool image_header_within_limits(const uint8_t* data, size_t size, int* width,
                                int* height) noexcept {
    if (width != nullptr) *width = 0;
    if (height != nullptr) *height = 0;
    if (data == nullptr || size == 0) return false;
    // stb takes an int length; a buffer larger than INT_MAX cannot be described
    // to it, and silently truncating the length would hand it a wrong size.
    if (size > static_cast<size_t>(INT32_MAX)) return false;

    int w = 0;
    int h = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &channels)) {
        const char* reason = stbi_failure_reason();
        log::write(log::Severity::Debug, "decode.limits", "image_header_unreadable",
                   "cannot read an image header from %zu bytes: %s", size,
                   reason != nullptr ? reason : "unknown");
        return false;
    }
    if (width != nullptr) *width = w;
    if (height != nullptr) *height = h;

    if (!image_dimensions_ok(w, h)) {
        log::write(log::Severity::Warning, "decode.limits", "image_too_large",
                   "refusing a %dx%d image: the ceiling is %lld pixels and %lld "
                   "per side",
                   w, h, static_cast<long long>(kMaxImagePixels),
                   static_cast<long long>(kMaxImageDimension));
        return false;
    }
    return true;
}

}  // namespace ayther::limits
