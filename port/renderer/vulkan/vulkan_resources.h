#ifndef KF_RENDERER_VULKAN_RESOURCES_H
#define KF_RENDERER_VULKAN_RESOURCES_H
// Owning handles for the buffers and images a backend allocates through VMA.
// Move-only; destroyed with their owner.
#include "vulkan_device.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace kf {
enum class BufferAccess : std::uint8_t { DeviceOnly, HostWrites, HostReads };

class GpuBuffer {
public:
    GpuBuffer() = default;
    // Null on failure. Host-access buffers stay mapped (data()).
    [[nodiscard]] static GpuBuffer create(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                                          BufferAccess access);
    ~GpuBuffer() { release(); }
    GpuBuffer(GpuBuffer &&other) noexcept { *this = std::move(other); }
    GpuBuffer &operator=(GpuBuffer &&other) noexcept;
    GpuBuffer(const GpuBuffer &) = delete;
    GpuBuffer &operator=(const GpuBuffer &) = delete;

    explicit operator bool() const { return buffer_ != VK_NULL_HANDLE; }
    VkBuffer buffer() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    void *data() const { return mapped_; }
    // Makes host writes visible to the device (non-coherent memory).
    void flush() const;
    // Makes device writes visible to the host.
    void invalidate() const;

private:
    void release();
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void *mapped_ = nullptr;
};

struct ImageShape {
    uint32_t width = 1, height = 1, layers = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t mip_levels = 1;
};

class GpuImage {
public:
    GpuImage() = default;
    // Null on failure. The view covers every layer (a 2D array if layers > 1)
    // and every mip level.
    [[nodiscard]] static GpuImage create(VkDevice device, VmaAllocator allocator, const ImageShape &shape);
    ~GpuImage() { release(); }
    GpuImage(GpuImage &&other) noexcept { *this = std::move(other); }
    GpuImage &operator=(GpuImage &&other) noexcept;
    GpuImage(const GpuImage &) = delete;
    GpuImage &operator=(const GpuImage &) = delete;

    explicit operator bool() const { return image_ != VK_NULL_HANDLE; }
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    const ImageShape &shape() const { return shape_; }

private:
    void release();
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    ImageShape shape_;
};

// Moves an image between layouts, waiting for everything before and holding
// back everything after (the backend's barriers are few and not hot).
void transition(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                VkImageLayout to);
} // namespace kf

#endif // KF_RENDERER_VULKAN_RESOURCES_H
