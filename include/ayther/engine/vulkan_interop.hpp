#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

struct VmaAllocator_T;

namespace ayther::engine {

/// Borrowed Vulkan state supplied by the application that hosts Engine.
///
/// The application owns every handle in this value, including the VMA
/// allocator. Engine may allocate renderer resources and submit work to the
/// graphics queue, but it must never destroy the instance, device, queue, or
/// allocator. Surface, presentation queue, and swapchain are deliberately not
/// part of this contract: presentation remains an application responsibility.
///
/// Copies do not extend any lifetime. All handles must remain valid until every
/// Engine renderer resource has been released and all submitted Engine work has
/// completed. Calls that use the allocator or graphics queue are externally
/// synchronized by the host according to the Vulkan and VMA specifications.
struct VulkanContextView {
    VkInstance instance_handle{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_handle{VK_NULL_HANDLE};
    VkDevice device_handle{VK_NULL_HANDLE};
    VkQueue graphics_queue_handle{VK_NULL_HANDLE};
    std::uint32_t graphics_queue_family_index{VK_QUEUE_FAMILY_IGNORED};
    VmaAllocator_T* allocator_handle{nullptr};

    [[nodiscard]] constexpr VkInstance instance() const noexcept {
        return instance_handle;
    }
    [[nodiscard]] constexpr VkPhysicalDevice physical_device() const noexcept {
        return physical_device_handle;
    }
    [[nodiscard]] constexpr VkDevice device() const noexcept {
        return device_handle;
    }
    [[nodiscard]] constexpr VkQueue graphics_queue() const noexcept {
        return graphics_queue_handle;
    }
    [[nodiscard]] constexpr std::uint32_t graphics_family() const noexcept {
        return graphics_queue_family_index;
    }
    [[nodiscard]] constexpr VmaAllocator_T* allocator() const noexcept {
        return allocator_handle;
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return instance_handle != VK_NULL_HANDLE &&
               physical_device_handle != VK_NULL_HANDLE &&
               device_handle != VK_NULL_HANDLE &&
               graphics_queue_handle != VK_NULL_HANDLE &&
               graphics_queue_family_index != VK_QUEUE_FAMILY_IGNORED &&
               allocator_handle != nullptr;
    }
};

/// Non-owning description of an Engine-rendered Vulkan image.
///
/// `image`, `image_view`, and `sampler` are borrowed handles. The Engine owns
/// the image, its memory, the view, and the sampler; copying this value does not
/// extend their lifetime and a consumer must never destroy or free them. A null
/// sampler means that only image operations are available.
///
/// The image uses exclusive queue-family ownership. `queue_family_index` names
/// the family that owns it, and consumer commands must execute on that family.
/// Queue-family transfer is not part of this contract.
///
/// `layout`, `ready_stage_mask`, and `ready_access_mask` describe the state in
/// which the latest Engine producer operation hands the image to the consumer.
/// They are the source scope for a barrier to another layout. Before the next
/// Engine access, the consumer must restore this layout and queue ownership and
/// order its work before that access.
///
/// When producer and consumer commands are recorded into the same command
/// buffer, command order plus the required barriers supplies synchronization;
/// no semaphore is needed. Across submissions, the consumer owns the fence or
/// semaphore chain and must wait for the producer before using the image, then
/// make completion visible before Engine reuses, resizes, or destroys it. This
/// view carries no synchronization primitive and signals nothing by itself.
///
/// Handles returned by AytherRenderer::render_image() remain valid until that
/// renderer is resized, shut down, or destroyed. Comparison-image handles also
/// end when the comparison image is released or recaptured at a new size.
/// Consumers must finish all GPU use and discard or rebind descriptors before
/// any such operation. Destroying a RenderImageView value has no Vulkan effect.
struct RenderImageView {
    VkImage image{VK_NULL_HANDLE};
    VkImageView image_view{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags ready_stage_mask{};
    VkAccessFlags ready_access_mask{};
    std::uint32_t queue_family_index{VK_QUEUE_FAMILY_IGNORED};

    /// Whether the required image metadata and borrow handles are present.
    /// A valid value still requires the producer/synchronization rules above.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return image != VK_NULL_HANDLE && image_view != VK_NULL_HANDLE &&
               format != VK_FORMAT_UNDEFINED && extent.width != 0U &&
               extent.height != 0U && layout != VK_IMAGE_LAYOUT_UNDEFINED &&
               ready_stage_mask != 0U &&
               queue_family_index != VK_QUEUE_FAMILY_IGNORED;
    }
};

}  // namespace ayther::engine
