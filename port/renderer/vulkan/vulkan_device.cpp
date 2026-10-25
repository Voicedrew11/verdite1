#include "vulkan_device.h"

#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace kf {
namespace {
bool validation_wanted() {
    if (const char *setting = std::getenv("KF_VK_VALIDATION"))
        return std::strcmp(setting, "0") != 0;
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

// A saved cache bigger than this is not read: the backend's pipelines take far less.
constexpr std::uintmax_t pipeline_cache_limit_bytes = 64u << 20;
// VkPipelineCacheHeaderVersionOne: size, version, vendor, device, UUID.
constexpr std::size_t pipeline_cache_header_bytes = 16 + VK_UUID_SIZE;

std::vector<std::uint8_t> read_pipeline_cache(const std::filesystem::path &file, const VkPhysicalDeviceProperties &gpu) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > pipeline_cache_limit_bytes)
        return {};
    std::ifstream in(file, std::ios::binary);
    std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!in.eof() || !pipeline_cache_fits(data, gpu))
        return {};
    return data;
}

void write_pipeline_cache(VkDevice device, VkPipelineCache cache, const std::filesystem::path &file) {
    std::size_t size = 0;
    if (vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS || size == 0)
        return;
    std::vector<std::uint8_t> data(size);
    if (vkGetPipelineCacheData(device, cache, &size, data.data()) != VK_SUCCESS)
        return;
    auto temporary = file;
    temporary += ".partial";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(size));
        if (!out)
            return;
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error)
        std::fprintf(stderr, "[vulkan] cannot keep the pipeline cache: %s\n", error.message().c_str());
}

std::vector<const char *> window_extensions(SDL_Window *window) {
    if (!window)
        return {};
    Uint32 count = 0;
    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names)
        return {};
    return {names, names + count};
}
} // namespace

bool pipeline_cache_fits(std::span<const std::uint8_t> data, const VkPhysicalDeviceProperties &gpu) {
    if (data.size() < pipeline_cache_header_bytes)
        return false;
    std::array<std::uint32_t, 4> words{};
    std::memcpy(words.data(), data.data(), sizeof words);
    const auto [header_bytes, version, vendor, device] = words;
    return header_bytes >= pipeline_cache_header_bytes && header_bytes <= data.size() &&
           version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && vendor == gpu.vendorID && device == gpu.deviceID &&
           std::memcmp(data.data() + 16, gpu.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *self) {
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        return VK_FALSE;
    const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    if (error && self)
        ++static_cast<VulkanDevice *>(self)->validation_errors_;
    std::fprintf(stderr, "[vulkan] %s: %s\n", error ? "error" : "warning", data && data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

VulkanDeviceStart VulkanDevice::create(SDL_Window *window, std::filesystem::path pipeline_cache_file) {
    if (volkInitialize() != VK_SUCCESS)
        return {nullptr, "no Vulkan loader (libvulkan.so.1)"};
    auto self = std::make_unique<VulkanDevice>(Key{});

    vkb::InstanceBuilder instance_builder(vkGetInstanceProcAddr);
    instance_builder.set_app_name("King's Field").require_api_version(1, 1, 0);
    instance_builder.enable_extensions(window_extensions(window));
    if (!window)
        instance_builder.set_headless();
    if (validation_wanted())
        instance_builder.request_validation_layers()
            .set_debug_callback(&VulkanDevice::on_validation)
            .set_debug_callback_user_data_pointer(self.get());
    auto instance = instance_builder.build();
    if (!instance)
        return {nullptr, "instance: " + instance.error().message()};
    self->instance_ = instance->instance;
    self->messenger_ = instance->debug_messenger;
    volkLoadInstanceOnly(self->instance_);

    if (window && !SDL_Vulkan_CreateSurface(window, self->instance_, nullptr, &self->surface_))
        return {nullptr, std::string("surface: ") + SDL_GetError()};

    vkb::PhysicalDeviceSelector selector(instance.value());
    selector.set_minimum_version(1, 1)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .allow_any_gpu_device_type();
    if (self->surface_)
        selector.set_surface(self->surface_);
    else
        selector.require_present(false);
    auto candidates = selector.select_devices();
    if (!candidates || candidates->empty())
        return {nullptr, "GPU: " + (candidates ? std::string("none suitable") : candidates.error().message())};
    const vkb::PhysicalDevice *physical = &candidates->front();
    if (const char *wanted = std::getenv("KF_VK_DEVICE")) {
        const auto named = std::find_if(candidates->begin(), candidates->end(),
            [&](const vkb::PhysicalDevice &candidate) { return candidate.name.find(wanted) != std::string::npos; });
        if (named == candidates->end())
            return {nullptr, std::string("GPU: none named like ") + wanted};
        physical = &*named;
    }
    self->physical_ = physical->physical_device;
    self->name_ = physical->name;
    // Anisotropic filtering where the device has it; 1 (none) otherwise.
    vkb::PhysicalDevice chosen = *physical;
    VkPhysicalDeviceFeatures anisotropic{};
    anisotropic.samplerAnisotropy = VK_TRUE;
    if (chosen.enable_features_if_present(anisotropic))
        self->max_anisotropy_ = std::max(1.0f, chosen.properties.limits.maxSamplerAnisotropy);

    auto device = vkb::DeviceBuilder(chosen).build();
    if (!device)
        return {nullptr, "device: " + device.error().message()};
    self->device_ = device->device;
    volkLoadDevice(self->device_);
    auto queue = device->get_queue(vkb::QueueType::graphics);
    auto family = device->get_queue_index(vkb::QueueType::graphics);
    if (!queue || !family)
        return {nullptr, "no graphics queue"};
    self->queue_ = queue.value();
    self->queue_family_ = family.value();
    if (self->surface_) {
        VkBool32 presents = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(self->physical_, self->queue_family_, self->surface_, &presents) !=
                VK_SUCCESS ||
            !presents)
            return {nullptr, "the graphics queue cannot present to the window"};
    }

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = self->physical_;
    allocator_info.device = self->device_;
    allocator_info.instance = self->instance_;
    allocator_info.pVulkanFunctions = &functions;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_1;
    if (vmaCreateAllocator(&allocator_info, &self->allocator_) != VK_SUCCESS)
        return {nullptr, "memory allocator"};

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = self->queue_family_;
    if (vkCreateCommandPool(self->device_, &pool_info, nullptr, &self->command_pool_) != VK_SUCCESS)
        return {nullptr, "command pool"};

    const std::vector<std::uint8_t> saved = pipeline_cache_file.empty()
                                                ? std::vector<std::uint8_t>{}
                                                : read_pipeline_cache(pipeline_cache_file, chosen.properties);
    VkPipelineCacheCreateInfo cache_info{};
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cache_info.initialDataSize = saved.size();
    cache_info.pInitialData = saved.empty() ? nullptr : saved.data();
    if (vkCreatePipelineCache(self->device_, &cache_info, nullptr, &self->pipeline_cache_) != VK_SUCCESS)
        return {nullptr, "pipeline cache"};
    self->pipeline_cache_file_ = std::move(pipeline_cache_file);
    return {std::move(self), {}};
}

VulkanDevice::~VulkanDevice() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        if (pipeline_cache_) {
            if (!pipeline_cache_file_.empty())
                write_pipeline_cache(device_, pipeline_cache_, pipeline_cache_file_);
            vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        }
        if (command_pool_)
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        if (allocator_)
            vmaDestroyAllocator(allocator_);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_)
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    if (messenger_)
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_)
        vkDestroyInstance(instance_, nullptr);
}

bool VulkanDevice::run_now(const std::function<void(VkCommandBuffer)> &record) const {
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = command_pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &allocate, &commands) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool good = vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS;
    if (good) {
        record(commands);
        good = vkEndCommandBuffer(commands) == VK_SUCCESS;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    good = good && vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
    good = good && vkQueueWaitIdle(queue_) == VK_SUCCESS;
    vkFreeCommandBuffers(device_, command_pool_, 1, &commands);
    return good;
}
} // namespace kf
