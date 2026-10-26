#include "vulkan_swapchain.h"

#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace kf {
namespace {
// Unorm: the game's colours are not linear.
std::optional<VkSurfaceFormatKHR> choose_format(VkPhysicalDevice physical, VkSurfaceKHR surface) {
    uint32_t count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr) != VK_SUCCESS || count == 0)
        return std::nullopt;
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()) != VK_SUCCESS)
        return std::nullopt;
    for (VkFormat wanted : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
        for (const VkSurfaceFormatKHR &format : formats)
            if (format.format == wanted && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;
    std::fprintf(stderr, "[vulkan] the surface offers no 8-bit unorm format\n");
    return std::nullopt;
}

VkPresentModeKHR choose_present_mode(VkPhysicalDevice physical, VkSurfaceKHR surface, PresentPacing pacing) {
    if (pacing == PresentPacing::Vsync)
        return VK_PRESENT_MODE_FIFO_KHR;
    uint32_t count = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr) != VK_SUCCESS)
        return VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> modes(count);
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data()) != VK_SUCCESS)
        return VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
        for (VkPresentModeKHR mode : modes)
            if (mode == wanted)
                return mode;
    return VK_PRESENT_MODE_FIFO_KHR;   // always offered
}

VkCompositeAlphaFlagBitsKHR choose_alpha(VkCompositeAlphaFlagsKHR supported) {
    for (VkCompositeAlphaFlagBitsKHR alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if (supported & alpha)
            return alpha;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

// The surface's own extent where it has one, else the window's size.
VkExtent2D swapchain_extent(const VkSurfaceCapabilitiesKHR &capabilities, uint32_t width, uint32_t height) {
    if (capabilities.currentExtent.width != UINT32_MAX)
        return capabilities.currentExtent;
    return {std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
}
} // namespace

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

bool VulkanSwapchain::get_images() {
    uint32_t count = 0;
    if (vkGetSwapchainImagesKHR(device_.device(), swapchain_, &count, nullptr) != VK_SUCCESS)
        return false;
    images_.resize(count);
    if (vkGetSwapchainImagesKHR(device_.device(), swapchain_, &count, images_.data()) != VK_SUCCESS)
        return false;
    for (VkImage image : images_) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format_;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device_.device(), &info, nullptr, &view) != VK_SUCCESS)
            return false;
        views_.push_back(view);
    }
    return true;
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
    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physical(), surface_, &capabilities) != VK_SUCCESS)
        return false;
    const auto format = choose_format(device_.physical(), surface_);
    if (!format)
        return false;
    // No pre-rotation: Android reports a turned display as a transform, and the
    // compositor turns an IDENTITY image for us (ANDROID_PLAN.md, B3).
    const VkSurfaceTransformFlagBitsKHR transform =
        capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                 : capabilities.currentTransform;
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && info.minImageCount > capabilities.maxImageCount)
        info.minImageCount = capabilities.maxImageCount;
    info.imageFormat = format->format;
    info.imageColorSpace = format->colorSpace;
    info.imageExtent = swapchain_extent(capabilities, width, height);
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = transform;
    info.compositeAlpha = choose_alpha(capabilities.supportedCompositeAlpha);
    info.presentMode = choose_present_mode(device_.physical(), surface_, pacing);
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR built = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateSwapchainKHR(device_.device(), &info, nullptr, &built); result != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] swapchain: error %d\n", static_cast<int>(result));
        return false;
    }
    release_images();
    if (swapchain_)
        vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
    swapchain_ = built;
    format_ = info.imageFormat;
    extent_ = info.imageExtent;
    pacing_ = pacing;
    stale_ = false;
    turned_by_compositor_ = transform != capabilities.currentTransform;
    if (!get_images())
        return false;
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
    stale_ = result == VK_SUBOPTIMAL_KHR && !turned_by_compositor_;
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
    // Android calls every present suboptimal while its compositor turns the image.
    const bool presented = result == VK_SUCCESS || (result == VK_SUBOPTIMAL_KHR && turned_by_compositor_);
    if (!presented)
        stale_ = true;
    if (result == VK_ERROR_SURFACE_LOST_KHR)
        surface_lost_ = true;
    return presented;
}
} // namespace kf
