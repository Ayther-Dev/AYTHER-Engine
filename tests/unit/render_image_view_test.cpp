#include <ayther/engine/vulkan_interop.hpp>

#include <cstdio>

int main() {
    const ayther::engine::RenderImageView empty{};
    const bool defaults_are_safe =
        !empty.is_valid() && empty.image == VK_NULL_HANDLE &&
        empty.image_view == VK_NULL_HANDLE && empty.sampler == VK_NULL_HANDLE &&
        empty.format == VK_FORMAT_UNDEFINED && empty.extent.width == 0U &&
        empty.extent.height == 0U &&
        empty.layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        empty.ready_stage_mask == 0U && empty.ready_access_mask == 0U &&
        empty.queue_family_index == VK_QUEUE_FAMILY_IGNORED;

    std::printf("  [%s] an empty borrowed view is invalid and inert\n",
                defaults_are_safe ? " OK " : "FAIL");
    return defaults_are_safe ? 0 : 1;
}
