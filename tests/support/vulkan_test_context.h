#pragma once

#include <ayther/engine/vulkan_interop.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <cstdio>
#include <cstdint>
#include <string>

/// Test-only Vulkan owner for Engine's GPU smoke executables.
///
/// Production applications own this state themselves (Runtime does so in its
/// local vk_context). The implicit conversion keeps the legacy smoke bodies
/// focused on renderer behavior while passing only the public borrowed view to
/// Engine APIs.
class VulkanTestContext final {
public:
    VulkanTestContext() = default;
    ~VulkanTestContext() { shutdown(); }

    VulkanTestContext(const VulkanTestContext&) = delete;
    VulkanTestContext& operator=(const VulkanTestContext&) = delete;

    bool init(SDL_Window* window) {
        if (window == nullptr) {
            return false;
        }

        vkb::InstanceBuilder instance_builder;
        instance_builder.set_app_name("Ayther Engine GPU smoke")
            .set_engine_name("Ayther")
            .require_api_version(1, 1, 0);
#ifndef NDEBUG
        instance_builder.request_validation_layers(true)
            .use_default_debug_messenger();
#endif

        auto instance_result = instance_builder.build();
        if (!instance_result) {
            std::fprintf(stderr, "[FAIL] Vulkan instance: %s\n",
                         instance_result.error().message().c_str());
            return false;
        }
        const vkb::Instance bootstrap_instance = instance_result.value();
        view_.instance_handle = bootstrap_instance.instance;
        debug_messenger_ = bootstrap_instance.debug_messenger;

        if (!SDL_Vulkan_CreateSurface(window, view_.instance(), nullptr,
                                      &surface_)) {
            std::fprintf(stderr, "[FAIL] Vulkan surface: %s\n", SDL_GetError());
            shutdown();
            return false;
        }

        vkb::PhysicalDeviceSelector selector(bootstrap_instance);
        auto physical_result = selector.set_surface(surface_)
            .set_minimum_version(1, 1)
            .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
            .select();
        if (!physical_result) {
            std::fprintf(stderr, "[FAIL] Vulkan physical device: %s\n",
                         physical_result.error().message().c_str());
            shutdown();
            return false;
        }
        const vkb::PhysicalDevice bootstrap_physical = physical_result.value();
        view_.physical_device_handle = bootstrap_physical.physical_device;
        gpu_name_ = bootstrap_physical.properties.deviceName;

        auto device_result = vkb::DeviceBuilder(bootstrap_physical).build();
        if (!device_result) {
            std::fprintf(stderr, "[FAIL] Vulkan device: %s\n",
                         device_result.error().message().c_str());
            shutdown();
            return false;
        }
        const vkb::Device bootstrap_device = device_result.value();
        view_.device_handle = bootstrap_device.device;

        const auto queue = bootstrap_device.get_queue(vkb::QueueType::graphics);
        const auto family =
            bootstrap_device.get_queue_index(vkb::QueueType::graphics);
        if (!queue || !family) {
            std::fprintf(stderr, "[FAIL] Vulkan graphics queue\n");
            shutdown();
            return false;
        }
        view_.graphics_queue_handle = queue.value();
        view_.graphics_queue_family_index = family.value();

        VmaAllocatorCreateInfo allocator_info{};
        allocator_info.instance = view_.instance();
        allocator_info.physicalDevice = view_.physical_device();
        allocator_info.device = view_.device();
        allocator_info.vulkanApiVersion = VK_API_VERSION_1_1;
        if (vmaCreateAllocator(&allocator_info, &view_.allocator_handle) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[FAIL] VMA allocator\n");
            shutdown();
            return false;
        }
        return true;
    }

    void shutdown() noexcept {
        if (view_.device() != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(view_.device());
        }
        if (view_.allocator_handle != nullptr) {
            vmaDestroyAllocator(view_.allocator_handle);
            view_.allocator_handle = nullptr;
        }
        if (view_.device() != VK_NULL_HANDLE) {
            vkDestroyDevice(view_.device(), nullptr);
            view_.device_handle = VK_NULL_HANDLE;
        }
        view_.graphics_queue_handle = VK_NULL_HANDLE;
        view_.graphics_queue_family_index = VK_QUEUE_FAMILY_IGNORED;

        if (surface_ != VK_NULL_HANDLE &&
            view_.instance() != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(view_.instance(), surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (debug_messenger_ != VK_NULL_HANDLE &&
            view_.instance() != VK_NULL_HANDLE) {
            const auto destroy_debug =
                reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(view_.instance(),
                                          "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_debug != nullptr) {
                destroy_debug(view_.instance(), debug_messenger_, nullptr);
            }
            debug_messenger_ = VK_NULL_HANDLE;
        }
        if (view_.instance() != VK_NULL_HANDLE) {
            vkDestroyInstance(view_.instance(), nullptr);
            view_.instance_handle = VK_NULL_HANDLE;
        }
        view_.physical_device_handle = VK_NULL_HANDLE;
        gpu_name_.clear();
    }

    [[nodiscard]] bool is_ready() const noexcept { return view_.is_valid(); }
    [[nodiscard]] VkInstance instance() const noexcept { return view_.instance(); }
    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept {
        return view_.physical_device();
    }
    [[nodiscard]] VkDevice device() const noexcept { return view_.device(); }
    [[nodiscard]] VkQueue graphics_queue() const noexcept {
        return view_.graphics_queue();
    }
    [[nodiscard]] std::uint32_t graphics_family() const noexcept {
        return view_.graphics_family();
    }
    [[nodiscard]] VmaAllocator_T* allocator() const noexcept {
        return view_.allocator();
    }
    [[nodiscard]] const std::string& gpu_name() const noexcept {
        return gpu_name_;
    }

    operator ayther::engine::VulkanContextView&() noexcept { return view_; }

private:
    ayther::engine::VulkanContextView view_{};
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    std::string gpu_name_;
};
