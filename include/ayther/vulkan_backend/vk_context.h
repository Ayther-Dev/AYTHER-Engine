#pragma once
// ---------------------------------------------------------------------------
// VkContext — Vulkan instance, physical device, logical device, queues, VMA.
//
// One instance per application lifetime.  Destroyed in reverse creation order
// inside shutdown() (also called by the destructor).
//
// Uses vk-bootstrap for the boilerplate-heavy device selection / creation.
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <string>

// Forward declarations to keep this header lightweight.
struct SDL_Window;
struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;   // matches VK_DEFINE_HANDLE expansion

// Enables per-object backend diagnostics. Hot-path logging is disabled by
// default because synchronous console writes can exceed the frame budget and
// obscure the actual upload cost. Startup events and periodic aggregates remain
// available independently.
bool vk_verbose_logging();

/// @brief Owns the Vulkan instance, device, queues, surface, and VMA allocator.
///
/// Create one context for an application lifetime. The destructor performs
/// idempotent reverse-order cleanup. The context is thread-affine unless the
/// caller provides the synchronization required by Vulkan for each operation.
class VkContext {
public:
    VkContext()  = default;
    ~VkContext() { shutdown(); }

    VkContext(const VkContext&)            = delete;
    VkContext& operator=(const VkContext&) = delete;

    // Initialize from an existing SDL3 window that was created with
    // SDL_WINDOW_VULKAN.  Returns false (with stderr log) on any failure.
    // In Debug builds, enables validation layers + default debug messenger.
    bool init(SDL_Window* window);

    // Destroy all Vulkan resources in the correct order.
    // Safe to call multiple times or in a partially-initialized state.
    void shutdown();

    bool is_ready() const { return device_ != VK_NULL_HANDLE; }

    // ---- Accessors --------------------------------------------------------
    VkInstance         instance()        const { return instance_;        }
    VkSurfaceKHR       surface()         const { return surface_;         }
    VkPhysicalDevice   physical_device() const { return physical_device_; }
    VkDevice           device()          const { return device_;          }
    VkQueue            graphics_queue()  const { return graphics_queue_;  }
    VkQueue            present_queue()   const { return present_queue_;   }
    uint32_t           graphics_family() const { return graphics_family_; }
    uint32_t           present_family()  const { return present_family_;  }
    VmaAllocator       allocator()       const { return allocator_;       }

    // Human-readable GPU model string (populated after init).
    const std::string& gpu_name() const { return gpu_name_; }

private:
    VkInstance               instance_        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_         = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_device_ = VK_NULL_HANDLE;
    VkDevice                 device_          = VK_NULL_HANDLE;
    VkQueue                  graphics_queue_  = VK_NULL_HANDLE;
    VkQueue                  present_queue_   = VK_NULL_HANDLE;
    uint32_t                 graphics_family_ = 0;
    uint32_t                 present_family_  = 0;
    VmaAllocator             allocator_       = nullptr;
    std::string              gpu_name_;
};
