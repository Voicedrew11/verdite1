#include "vulkan_swapchain.h"

#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>

#include <cstdio>

namespace kf {
VulkanSwapchain::~VulkanSwapchain() { drop_surface(); }

void VulkanSwapchain::drop_surface() {
    vkDeviceWaitIdle(device_.device());
    release_images();
    if (swapchain_)
        vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (surface_)
        SDL_Vulkan_DestroySurface(device_.instance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    surface_lost_ = false;
}

// A new surface for the window, which the device's queue must present to.
bool VulkanSwapchain::make_surface() {
    if (!SDL_Vulkan_CreateSurface(window_, device_.instance(), nullptr, &surface_)) {
        std::fprintf(stderr, "[vulkan] surface: %s\n", SDL_GetError());
        surface_ = VK_NULL_HANDLE;
        return false;
    }
    VkBool32 presents = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(device_.physical(), device_.queue_family(), surface_, &presents) !=
            VK_SUCCESS ||
        !presents) {
        std::fprintf(stderr, "[vulkan] the graphics queue cannot present to the new surface\n");
        drop_surface();
        return false;
    }
    return true;
}

void VulkanSwapchain::release_images() {
    framebuffers_.clear();
    for (VkImageView view : views_)
        vkDestroyImageView(device_.device(), view, nullptr);
    for (VkSemaphore semaphore : ready_)
        vkDestroySemaphore(device_.device(), semaphore, nullptr);
    views_.clear();
    ready_.clear();
    images_.clear();
}

bool VulkanSwapchain::fit(uint32_t width, uint32_t height, PresentPacing pacing) {
    if (surface_lost_)
        drop_surface();
    if (swapchain_ && !stale_ && width == extent_.width && height == extent_.height && pacing == pacing_)
        return true;
    if (!surface_) {
        if (!make_surface())
            return false;
        std::printf("[vulkan] window surface made again\n");
    }
    vkDeviceWaitIdle(device_.device());
    vkb::SwapchainBuilder builder(device_.physical(), device_.device(), surface_, device_.queue_family(),
                                  device_.queue_family());
    // Unorm: the game's colours are not linear.
    builder.set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .add_fallback_format({VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        .set_old_swapchain(swapchain_);
    if (pacing == PresentPacing::Vsync) {
        builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    } else {
        builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
            .add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
            .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    }
    auto built = builder.build();
    if (!built) {
        std::fprintf(stderr, "[vulkan] swapchain: %s\n", built.error().message().c_str());
        return false;
    }
    release_images();
    if (swapchain_)
        vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
    swapchain_ = built->swapchain;
    format_ = built->image_format;
    extent_ = built->extent;
    pacing_ = pacing;
    stale_ = false;
    auto images = built->get_images();
    auto views = built->get_image_views();
    if (!images || !views)
        return false;
    images_ = images.value();
    views_ = views.value();
    if (!pass_ || pass_.shape().color != format_)
        pass_ = RenderPass::create(device_.device(), {format_, VK_FORMAT_UNDEFINED, ColorLoad::Keep});
    if (!pass_)
        return false;
    for (VkImageView view : views_) {
        framebuffers_.push_back(Framebuffer::create(device_.device(), pass_, std::span(&view, 1), extent_));
        if (!framebuffers_.back())
            return false;
    }
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (std::size_t i = 0; i < images_.size(); ++i) {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (vkCreateSemaphore(device_.device(), &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
            return false;
        ready_.push_back(semaphore);
    }
    return true;
}

std::optional<SwapchainImage> VulkanSwapchain::acquire(VkSemaphore acquired) {
    if (!swapchain_ || stale_)
        return std::nullopt;
    uint32_t index = 0;
    const VkResult result = vkAcquireNextImageKHR(device_.device(), swapchain_, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) ||
        index >= images_.size()) {
        stale_ = true;
        surface_lost_ = result == VK_ERROR_SURFACE_LOST_KHR;
        return std::nullopt;
    }
    stale_ = result == VK_SUBOPTIMAL_KHR;
    return SwapchainImage{index, images_[index], views_[index], ready_[index], &framebuffers_[index]};
}

bool VulkanSwapchain::present(const SwapchainImage &image) {
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &image.ready_to_present;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &image.index;
    const VkResult result = vkQueuePresentKHR(device_.queue(), &info);
    if (result != VK_SUCCESS)
        stale_ = true;
    if (result == VK_ERROR_SURFACE_LOST_KHR)
        surface_lost_ = true;
    return result == VK_SUCCESS;
}
} // namespace kf
