#include <ayther/engine/vulkan_interop.hpp>

#include <cstdint>
#include <type_traits>

static_assert(std::is_standard_layout_v<ayther::engine::RenderImageView>);
static_assert(std::is_trivially_copyable_v<ayther::engine::RenderImageView>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::image), VkImage>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::image_view),
              VkImageView>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::sampler), VkSampler>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::format), VkFormat>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::extent), VkExtent2D>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::layout),
              VkImageLayout>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::ready_stage_mask),
              VkPipelineStageFlags>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::ready_access_mask),
              VkAccessFlags>);
static_assert(std::is_same_v<
              decltype(ayther::engine::RenderImageView::queue_family_index),
              std::uint32_t>);
static_assert(noexcept(ayther::engine::RenderImageView{}.is_valid()));
