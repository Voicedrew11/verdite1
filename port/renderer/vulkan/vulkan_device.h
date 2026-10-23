#ifndef KF_RENDERER_VULKAN_DEVICE_H
#define KF_RENDERER_VULKAN_DEVICE_H
// The Vulkan instance, GPU, queue and memory allocator one backend draws with.
// Vulkan 1.1 with dynamic rendering; validation layers when asked for. Without
// a window it has no surface and draws offscreen only.
#include <volk.h>
#include <vk_mem_alloc.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

struct SDL_Window;

namespace kf {
struct VulkanDeviceStart;

class VulkanDevice {
    struct Key {
        explicit Key() = default;
    };

public:
    explicit VulkanDevice(Key) {}
    // KF_VK_VALIDATION=1 turns on the validation layers (on by default in debug
    // builds); KF_VK_DEVICE=<name part> picks a GPU by name (llvmpipe, say).
    static VulkanDeviceStart create(SDL_Window *window);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice &) = delete;
    VulkanDevice &operator=(const VulkanDevice &) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queue_family() const { return queue_family_; }
    VkSurfaceKHR surface() const { return surface_; }
    VmaAllocator allocator() const { return allocator_; }
    VkCommandPool command_pool() const { return command_pool_; }
    const std::string &name() const { return name_; }
    // Errors the validation layers reported so far (0 without layers).
    int validation_errors() const { return validation_errors_.load(); }

    // Records commands into a one-time command buffer, submits and waits.
    [[nodiscard]] bool run_now(const std::function<void(VkCommandBuffer)> &record) const;

private:
    // On the driver's thread, possibly: counts and prints a validation message.
    static VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data, void *self);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::string name_;
    std::atomic<int> validation_errors_{0};
};

struct VulkanDeviceStart {
    std::unique_ptr<VulkanDevice> device;   // null: error says why
    std::string error;
};
} // namespace kf

#endif // KF_RENDERER_VULKAN_DEVICE_H
