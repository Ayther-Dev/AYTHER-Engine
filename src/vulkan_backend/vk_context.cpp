#include "vulkan_backend/vk_context.h"
#include "log.h"
#include "runtime_options.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Enable validation layers in every non-Release build.
#ifdef NDEBUG
static constexpr bool kValidation = false;
#else
static constexpr bool kValidation = true;
#endif

// Ver la nota de vk_context.h: los logs por-objeto del camino caliente cuestan
// ~5 ms cada uno contra la consola. Se lee una sola vez; el env var no cambia
// en vivo y consultarlo por textura sería otro costo por objeto.
bool vk_verbose_logging() {
    static const bool on = [] {
        return ayther::RuntimeOptions::process().vulkan_verbose();
    }();
    return on;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool VkContext::init(SDL_Window* window) {

    // -----------------------------------------------------------------------
    // 1. Vulkan Instance
    // -----------------------------------------------------------------------
    vkb::InstanceBuilder inst_builder;
    inst_builder
        .set_app_name("Ayther Engine")
        .set_engine_name("Ayther")
        .require_api_version(1, 1, 0);

    if (kValidation) {
        // request_ (not require_) so the build doesn't hard-fail on machines
        // without the Vulkan SDK validation layer package installed.
        inst_builder
            .request_validation_layers(true)
            .use_default_debug_messenger();
    }

    auto inst_ret = inst_builder.build();
    if (!inst_ret) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.context", "instance_creation_failed",
            "Instance creation failed: %s",
            inst_ret.error().message().c_str());
        return false;
    }
    vkb::Instance vkb_inst = inst_ret.value();
    instance_        = vkb_inst.instance;
    debug_messenger_ = vkb_inst.debug_messenger;   // VK_NULL_HANDLE in Release

    // -----------------------------------------------------------------------
    // 2. Window surface (SDL3)
    // -----------------------------------------------------------------------
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.context", "surface_creation_failed",
            "Surface creation failed: %s",
            SDL_GetError());
        return false;
    }

    // -----------------------------------------------------------------------
    // 3. Physical device — prefer discrete GPU, require presentation support
    // -----------------------------------------------------------------------
    vkb::PhysicalDeviceSelector phys_sel(vkb_inst);
    auto phys_ret = phys_sel
        .set_surface(surface_)
        .set_minimum_version(1, 1)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .select();

    if (!phys_ret) {
        ayther::log::write(ayther::log::Severity::Warning,
            "vulkan.context", "suitable_gpu",
            "No suitable GPU: %s",
            phys_ret.error().message().c_str());
        return false;
    }
    vkb::PhysicalDevice vkb_phys = phys_ret.value();
    physical_device_ = vkb_phys.physical_device;
    gpu_name_        = std::string(vkb_phys.properties.deviceName);

    // Which device and which driver answered. A GPU result is only meaningful
    // alongside what produced it: "the renderer tests passed" says nothing
    // useful without knowing whether that was a discrete card or a software
    // rasteriser. This line is what the GPU matrix records.
    {
        const uint32_t driver = vkb_phys.properties.driverVersion;
        const uint32_t api = vkb_phys.properties.apiVersion;
        ayther::log::write(ayther::log::Severity::Info,
            "vulkan.context", "device_selected",
            "GPU: %s  type=%u  vendor=0x%04X  driver=%u.%u.%u  api=%u.%u.%u",
            gpu_name_.c_str(),
            static_cast<unsigned>(vkb_phys.properties.deviceType),
            static_cast<unsigned>(vkb_phys.properties.vendorID),
            VK_VERSION_MAJOR(driver), VK_VERSION_MINOR(driver),
            VK_VERSION_PATCH(driver),
            VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api), VK_VERSION_PATCH(api));
    }

    // -----------------------------------------------------------------------
    // 4. Logical device + queues
    // -----------------------------------------------------------------------
    vkb::DeviceBuilder dev_builder(vkb_phys);
    auto dev_ret = dev_builder.build();
    if (!dev_ret) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.context", "device_creation_failed",
            "Device creation failed: %s",
            dev_ret.error().message().c_str());
        return false;
    }
    vkb::Device vkb_dev = dev_ret.value();
    device_ = vkb_dev.device;

    auto gq  = vkb_dev.get_queue(vkb::QueueType::graphics);
    auto pq  = vkb_dev.get_queue(vkb::QueueType::present);
    auto gqi = vkb_dev.get_queue_index(vkb::QueueType::graphics);
    auto pqi = vkb_dev.get_queue_index(vkb::QueueType::present);

    if (!gq || !pq || !gqi || !pqi) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.context", "queue_retrieval_failed",
            "Queue retrieval failed");
        return false;
    }
    graphics_queue_  = gq.value();
    present_queue_   = pq.value();
    graphics_family_ = gqi.value();
    present_family_  = pqi.value();

    // -----------------------------------------------------------------------
    // 5. VMA allocator
    //    VMA_STATIC_VULKAN_FUNCTIONS=1 (set via CMake) so no pVulkanFunctions
    // -----------------------------------------------------------------------
    VmaAllocatorCreateInfo vma_info{};
    vma_info.physicalDevice   = physical_device_;
    vma_info.device           = device_;
    vma_info.instance         = instance_;
    vma_info.vulkanApiVersion = VK_API_VERSION_1_1;

    if (vmaCreateAllocator(&vma_info, &allocator_) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.context", "vma_allocator_creation_failed",
            "VMA allocator creation failed");
        return false;
    }

    ayther::log::write(ayther::log::Severity::Info,
        "vulkan.context", "ready_gpu_queues_g",
        "Ready — GPU: %s  queues G=%u P=%u  validation=%s",
        gpu_name_.c_str(),
        graphics_family_,
        present_family_,
        kValidation ? "on" : "off");
    return true;
}

// ---------------------------------------------------------------------------
// shutdown — destroy in reverse creation order
// ---------------------------------------------------------------------------
void VkContext::shutdown() {
    if (!instance_) return;  // never initialized (or already shut down)

    if (allocator_) {
        vmaDestroyAllocator(allocator_);
        allocator_ = nullptr;
    }

    if (device_) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_          = VK_NULL_HANDLE;
        graphics_queue_  = VK_NULL_HANDLE;
        present_queue_   = VK_NULL_HANDLE;
    }

    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    // Validation debug messenger (Debug builds only)
    if (debug_messenger_) {
        auto pfn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (pfn) pfn(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }

    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;

    ayther::log::write(ayther::log::Severity::Info,
        "vulkan.context", "shutdown_complete",
        "Shutdown complete.");
}
