#include <ayther/engine/vulkan_interop.hpp>
#include <ayther/engine/input.hpp>

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

    const ayther::engine::VulkanContextView complete_context{
        .instance_handle = non_null_handle<VkInstance>(),
        .physical_device_handle = non_null_handle<VkPhysicalDevice>(),
        .device_handle = non_null_handle<VkDevice>(),
        .graphics_queue_handle = non_null_handle<VkQueue>(),
        .graphics_queue_family_index = 7U,
        .allocator_handle = non_null_handle<VmaAllocator>(),
    };
    const bool context_accessors_match = complete_context.is_valid() &&
        complete_context.instance() == complete_context.instance_handle &&
        complete_context.physical_device() == complete_context.physical_device_handle &&
        complete_context.device() == complete_context.device_handle &&
        complete_context.graphics_queue() == complete_context.graphics_queue_handle &&
        complete_context.graphics_family() == 7U &&
        complete_context.allocator() == complete_context.allocator_handle;

    const ayther::engine::RenderImageView complete_image{
        .image = non_null_handle<VkImage>(),
        .image_view = non_null_handle<VkImageView>(),
        .sampler = non_null_handle<VkSampler>(),
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {320U, 240U},
        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .ready_stage_mask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .ready_access_mask = VK_ACCESS_SHADER_READ_BIT,
        .queue_family_index = 7U,
    };
    const bool complete_image_is_valid = complete_image.is_valid();

    using ayther::engine::InputState;
    using ayther::engine::RetroPadButton;
    const volatile std::uint8_t runtime_a = 8U;
    const auto a = static_cast<RetroPadButton>(runtime_a);
    const auto combined = InputState{a} | InputState{RetroPadButton::start};
    const auto restored = InputState::from_bits(combined.bits());
    const bool typed_input_is_exact =
        InputState{}.bits() == 0U &&
        restored.pressed(RetroPadButton::a) &&
        restored.pressed(RetroPadButton::start) &&
        !restored.pressed(RetroPadButton::b);

    std::printf("  [%s] an empty borrowed Vulkan context is invalid and inert\n",
                context_defaults_are_safe ? " OK " : "FAIL");
    std::printf("  [%s] an empty borrowed image view is invalid and inert\n",
                defaults_are_safe ? " OK " : "FAIL");
    std::printf("  [%s] a handoff without the Engine sampler is invalid\n",
                incomplete_view_is_rejected ? " OK " : "FAIL");
    std::printf("  [%s] a complete borrowed Vulkan context preserves every handle\n",
                context_accessors_match ? " OK " : "FAIL");
    std::printf("  [%s] a complete rendered image handoff is valid\n",
                complete_image_is_valid ? " OK " : "FAIL");
    std::printf("  [%s] typed input masks compose and round-trip exactly\n",
                typed_input_is_exact ? " OK " : "FAIL");
    const bool all_checks_pass = context_defaults_are_safe &&
                                 defaults_are_safe &&
                                 incomplete_view_is_rejected &&
                                 context_accessors_match &&
                                 complete_image_is_valid &&
                                 typed_input_is_exact;
    return all_checks_pass ? 0 : 1;
}
