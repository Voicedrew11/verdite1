#include "vulkan_resources.h"

namespace kf {
GpuBuffer GpuBuffer::create(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, BufferAccess access) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    if (access == BufferAccess::HostWrites)
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (access == BufferAccess::HostReads)
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    GpuBuffer out;
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(allocator, &info, &allocation_info, &out.buffer_, &out.allocation_, &result) != VK_SUCCESS)
        return {};
    out.allocator_ = allocator;
    out.size_ = size;
    out.mapped_ = result.pMappedData;
    return out;
}

GpuBuffer &GpuBuffer::operator=(GpuBuffer &&other) noexcept {
    if (this != &other) {
        release();
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

void GpuBuffer::flush() const {
    if (allocation_)
        vmaFlushAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE);
}

void GpuBuffer::invalidate() const {
    if (allocation_)
        vmaInvalidateAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE);
}

void GpuBuffer::release() {
    if (buffer_)
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    mapped_ = nullptr;
}

GpuImage GpuImage::create(VkDevice device, VmaAllocator allocator, const ImageShape &shape) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = shape.format;
    info.extent = {shape.width, shape.height, 1};
    info.mipLevels = shape.mip_levels;
    info.arrayLayers = shape.layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = shape.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    GpuImage out;
    if (vmaCreateImage(allocator, &info, &allocation_info, &out.image_, &out.allocation_, nullptr) != VK_SUCCESS)
        return {};
    out.device_ = device;
    out.allocator_ = allocator;
    out.shape_ = shape;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = out.image_;
    view.viewType = shape.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format = shape.format;
    view.subresourceRange = {shape.aspect, 0, shape.mip_levels, 0, shape.layers};
    if (vkCreateImageView(device, &view, nullptr, &out.view_) != VK_SUCCESS)
        return {};
    return out;
}

GpuImage &GpuImage::operator=(GpuImage &&other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        shape_ = other.shape_;
    }
    return *this;
}

void GpuImage::release() {
    if (view_)
        vkDestroyImageView(device_, view_, nullptr);
    if (image_)
        vmaDestroyImage(allocator_, image_, allocation_);
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
}

void transition(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                VkImageLayout to) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
}
} // namespace kf
