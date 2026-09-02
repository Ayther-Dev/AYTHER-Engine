#include <ayther/engine/vulkan_interop.hpp>

#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

template <typename Handle>
Handle non_null_handle() noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(std::uintptr_t{1});
    } else {
        return static_cast<Handle>(1);
    }
}

}  // namespace

int main() {
    const ayther::engine::VulkanContextView empty_context{};
    const bool context_defaults_are_safe =
        !empty_context.is_valid() &&
        empty_context.instance_handle == VK_NULL_HANDLE &&
        empty_context.physical_device_handle == VK_NULL_HANDLE &&
        empty_context.device_handle == VK_NULL_HANDLE &&
        empty_context.graphics_queue_handle == VK_NULL_HANDLE &&
        empty_context.graphics_queue_family_index == VK_QUEUE_FAMILY_IGNORED &&
        empty_context.allocator_handle == nullptr;

    const ayther::engine::RenderImageView empty{};
    const bool defaults_are_safe =
        !empty.is_valid() && empty.image == VK_NULL_HANDLE &&
        empty.image_view == VK_NULL_HANDLE && empty.sampler == VK_NULL_HANDLE &&
        empty.format == VK_FORMAT_UNDEFINED && empty.extent.width == 0U &&
        empty.extent.height == 0U &&
        empty.layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        empty.ready_stage_mask == 0U && empty.ready_access_mask == 0U &&
        empty.queue_family_index == VK_QUEUE_FAMILY_IGNORED;

    const ayther::engine::RenderImageView missing_sampler{
        .image = non_null_handle<VkImage>(),
        .image_view = non_null_handle<VkImageView>(),
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {1U, 1U},
        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .ready_stage_mask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .ready_access_mask = VK_ACCESS_SHADER_READ_BIT,
        .queue_family_index = 0U,
    };
    const bool incomplete_view_is_rejected = !missing_sampler.is_valid();

    std::printf("  [%s] an empty borrowed Vulkan context is invalid and inert\n",
                context_defaults_are_safe ? " OK " : "FAIL");
    std::printf("  [%s] an empty borrowed image view is invalid and inert\n",
                defaults_are_safe ? " OK " : "FAIL");
    std::printf("  [%s] a handoff without the Engine sampler is invalid\n",
                incomplete_view_is_rejected ? " OK " : "FAIL");
    const bool all_checks_pass = context_defaults_are_safe &&
                                 defaults_are_safe &&
                                 incomplete_view_is_rejected;
    return all_checks_pass ? 0 : 1;
}
