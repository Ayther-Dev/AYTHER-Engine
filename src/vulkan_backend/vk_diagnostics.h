#pragma once

#include "runtime_options.h"

/// Per-object Vulkan diagnostics are an Engine rendering concern, independent
/// of the application-owned Vulkan context.
inline bool vk_verbose_logging() {
    static const bool enabled =
        ayther::RuntimeOptions::process().vulkan_verbose();
    return enabled;
}
