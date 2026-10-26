#ifndef KF_RENDERER_VULKAN_SWAPCHAIN_H
#define KF_RENDERER_VULKAN_SWAPCHAIN_H
// The window's surface and swapchain: built for the window's size and the
// VSync choice, rebuilt when either changes or the surface goes out of date.
// The surface is dropped when the window's is taken away (Android, in the
// background) or lost, and made again at the next fit. It also holds the pass
// that draws over its images (the overlay), which keeps what is there.
#include "vulkan_device.h"
#include "vulkan_render_pass.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace kf {
enum class PresentPacing : std::uint8_t { Vsync, Immediate };

struct SwapchainImage {
    uint32_t index;
    VkImage image;
    VkImageView view;
    VkSemaphore ready_to_present;   // signal it when the image is drawn
    const Framebuffer *framebuffer; // the image in render_pass()
};

class VulkanSwapchain {
public:
    // surface: VulkanDevice::take_surface, made for window; this owns it.
    VulkanSwapchain(const VulkanDevice &device, SDL_Window *window, VkSurfaceKHR surface)
        : device_(device), window_(window), surface_(surface) {}
    ~VulkanSwapchain();
    VulkanSwapchain(const VulkanSwapchain &) = delete;
    VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;

    // Builds or rebuilds it when the size or pacing differ from the current one,
    // making the surface again first if it was dropped.
    [[nodiscard]] bool fit(uint32_t width, uint32_t height, PresentPacing pacing);
    // Destroys the swapchain and the surface, waiting for the GPU first.
    void drop_surface();
    // The next image, once acquired signals; none when the swapchain must be rebuilt.
    std::optional<SwapchainImage> acquire(VkSemaphore acquired);
    // False when the swapchain must be rebuilt.
    bool present(const SwapchainImage &image);
    VkFormat format() const { return format_; }
    // The pass that draws over an image; made with the first swapchain, for its format.
    const RenderPass &render_pass() const { return pass_; }
    VkExtent2D extent() const { return extent_; }
    uint32_t image_count() const { return static_cast<uint32_t>(images_.size()); }

private:
    void release_images();
    bool get_images();
    bool make_surface();
    const VulkanDevice &device_;
    SDL_Window *window_;
    VkSurfaceKHR surface_;
    bool surface_lost_ = false;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{0, 0};
    PresentPacing pacing_ = PresentPacing::Vsync;
    bool stale_ = false;
    bool turned_by_compositor_ = false;   // its transform is not the display's (Android, turned)
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> ready_;
    RenderPass pass_;
    std::vector<Framebuffer> framebuffers_;
};
} // namespace kf

#endif // KF_RENDERER_VULKAN_SWAPCHAIN_H
