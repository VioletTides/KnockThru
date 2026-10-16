#include "overlay_texture.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <dlfcn.h>

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace knockthru {
namespace {

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan ") + what + " failed: " + std::to_string(result));
}

// Splits SteamVR's space-separated extension list.
std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string word; stream >> word;) out.push_back(word);
    return out;
}

std::vector<const char*> c_strings(const std::vector<std::string>& strings) {
    std::vector<const char*> out;
    for (const auto& s : strings) out.push_back(s.c_str());
    return out;
}

} // namespace

struct OverlayTexture::Impl {
    void* library = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    std::uint32_t width, height;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* staging_map = nullptr;
    std::array<VkImage, 2> images{};
    std::array<VkDeviceMemory, 2> image_memory{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int next = 0;

#define KT_VK(name) PFN_vk##name name = nullptr
    KT_VK(CreateInstance); KT_VK(DestroyInstance); KT_VK(EnumeratePhysicalDevices);
    KT_VK(GetPhysicalDeviceQueueFamilyProperties); KT_VK(GetPhysicalDeviceMemoryProperties);
    KT_VK(CreateDevice); KT_VK(DestroyDevice); KT_VK(GetDeviceQueue); KT_VK(GetDeviceProcAddr);
    KT_VK(CreateBuffer); KT_VK(DestroyBuffer); KT_VK(GetBufferMemoryRequirements); KT_VK(BindBufferMemory);
    KT_VK(CreateImage); KT_VK(DestroyImage); KT_VK(GetImageMemoryRequirements); KT_VK(BindImageMemory);
    KT_VK(AllocateMemory); KT_VK(FreeMemory); KT_VK(MapMemory);
    KT_VK(CreateCommandPool); KT_VK(DestroyCommandPool); KT_VK(AllocateCommandBuffers);
    KT_VK(BeginCommandBuffer); KT_VK(EndCommandBuffer); KT_VK(ResetCommandBuffer);
    KT_VK(CmdPipelineBarrier); KT_VK(CmdCopyBufferToImage);
    KT_VK(CreateFence); KT_VK(DestroyFence); KT_VK(ResetFences); KT_VK(WaitForFences);
    KT_VK(QueueSubmit); KT_VK(DeviceWaitIdle);
#undef KT_VK

    template <typename T> void load(T& fn, const char* name, bool device_level) {
        fn = reinterpret_cast<T>(device_level ? GetDeviceProcAddr(device, name) : get_instance_proc(instance, name));
        if (!fn) throw std::runtime_error(std::string("Vulkan function missing: ") + name);
    }

    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties properties;
        GetPhysicalDeviceMemoryProperties(physical, &properties);
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("Vulkan has no suitable memory type");
    }

    void create_instance() {
        library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library) throw std::runtime_error("libvulkan.so.1 not found");
        get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
        if (!get_instance_proc) throw std::runtime_error("vkGetInstanceProcAddr not found");
        load(CreateInstance, "vkCreateInstance", false);

        std::array<char, 4096> buffer{};
        vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buffer.data(), static_cast<uint32_t>(buffer.size()));
        const auto extensions = split(buffer.data());
        const auto names = c_strings(extensions);
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "KnockThru";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
        info.ppEnabledExtensionNames = names.data();
        check(CreateInstance(&info, nullptr, &instance), "instance");

        load(DestroyInstance, "vkDestroyInstance", false);
        load(EnumeratePhysicalDevices, "vkEnumeratePhysicalDevices", false);
        load(GetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties", false);
        load(GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties", false);
        load(CreateDevice, "vkCreateDevice", false);
        load(GetDeviceProcAddr, "vkGetDeviceProcAddr", false);
    }

    void create_device() {
        // SteamVR names the GPU it composites on. Fall back to the first one.
        std::uint64_t output = 0;
        vr::VRSystem()->GetOutputDevice(&output, vr::TextureType_Vulkan, reinterpret_cast<VkInstance_T*>(instance));
        physical = reinterpret_cast<VkPhysicalDevice>(output);
        if (!physical) {
            std::uint32_t count = 1;
            EnumeratePhysicalDevices(instance, &count, &physical);
        }
        if (!physical) throw std::runtime_error("No Vulkan GPU");

        std::uint32_t count = 0;
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        family = count;
        for (std::uint32_t i = 0; i < count; ++i)
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
        if (family == count) throw std::runtime_error("No Vulkan graphics queue");

        std::array<char, 4096> buffer{};
        vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(reinterpret_cast<VkPhysicalDevice_T*>(physical),
            buffer.data(), static_cast<uint32_t>(buffer.size()));
        const auto extensions = split(buffer.data());
        const auto names = c_strings(extensions);
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue_info;
        info.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
        info.ppEnabledExtensionNames = names.data();
        check(CreateDevice(physical, &info, nullptr, &device), "device");

        load(DestroyDevice, "vkDestroyDevice", true); load(GetDeviceQueue, "vkGetDeviceQueue", true);
        load(CreateBuffer, "vkCreateBuffer", true); load(DestroyBuffer, "vkDestroyBuffer", true);
        load(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements", true);
        load(BindBufferMemory, "vkBindBufferMemory", true);
        load(CreateImage, "vkCreateImage", true); load(DestroyImage, "vkDestroyImage", true);
        load(GetImageMemoryRequirements, "vkGetImageMemoryRequirements", true);
        load(BindImageMemory, "vkBindImageMemory", true);
        load(AllocateMemory, "vkAllocateMemory", true); load(FreeMemory, "vkFreeMemory", true);
        load(MapMemory, "vkMapMemory", true);
        load(CreateCommandPool, "vkCreateCommandPool", true); load(DestroyCommandPool, "vkDestroyCommandPool", true);
        load(AllocateCommandBuffers, "vkAllocateCommandBuffers", true);
        load(BeginCommandBuffer, "vkBeginCommandBuffer", true); load(EndCommandBuffer, "vkEndCommandBuffer", true);
        load(ResetCommandBuffer, "vkResetCommandBuffer", true);
        load(CmdPipelineBarrier, "vkCmdPipelineBarrier", true);
        load(CmdCopyBufferToImage, "vkCmdCopyBufferToImage", true);
        load(CreateFence, "vkCreateFence", true); load(DestroyFence, "vkDestroyFence", true);
        load(ResetFences, "vkResetFences", true); load(WaitForFences, "vkWaitForFences", true);
        load(QueueSubmit, "vkQueueSubmit", true); load(DeviceWaitIdle, "vkDeviceWaitIdle", true);
        GetDeviceQueue(device, family, 0, &queue);
    }

    void create_resources() {
        const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(CreateBuffer(device, &buffer_info, nullptr, &staging), "staging buffer");
        VkMemoryRequirements needs;
        GetBufferMemoryRequirements(device, staging, &needs);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = memory_type(needs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(AllocateMemory(device, &alloc, nullptr, &staging_memory), "staging memory");
        check(BindBufferMemory(device, staging, staging_memory, 0), "staging bind");
        check(MapMemory(device, staging_memory, 0, size, 0, &staging_map), "staging map");

        for (int i = 0; i < 2; ++i) {
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
            image_info.extent = {width, height, 1};
            image_info.mipLevels = 1;
            image_info.arrayLayers = 1;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            check(CreateImage(device, &image_info, nullptr, &images[i]), "image");
            GetImageMemoryRequirements(device, images[i], &needs);
            alloc.allocationSize = needs.size;
            alloc.memoryTypeIndex = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(AllocateMemory(device, &alloc, nullptr, &image_memory[i]), "image memory");
            check(BindImageMemory(device, images[i], image_memory[i], 0), "image bind");
        }

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        check(CreateCommandPool(device, &pool_info, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        check(AllocateCommandBuffers(device, &command_info, &commands), "command buffer");
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(CreateFence(device, &fence_info, nullptr, &fence), "fence");
    }

    void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, nullptr, 0, nullptr, 1, &b);
    }

    ~Impl() {
        if (device) {
            DeviceWaitIdle(device);
            if (fence) DestroyFence(device, fence, nullptr);
            if (pool) DestroyCommandPool(device, pool, nullptr);
            for (int i = 0; i < 2; ++i) {
                if (images[i]) DestroyImage(device, images[i], nullptr);
                if (image_memory[i]) FreeMemory(device, image_memory[i], nullptr);
            }
            if (staging) DestroyBuffer(device, staging, nullptr);
            if (staging_memory) FreeMemory(device, staging_memory, nullptr);
            DestroyDevice(device, nullptr);
        }
        if (instance) DestroyInstance(instance, nullptr);
        if (library) dlclose(library);
    }
};

OverlayTexture::OverlayTexture(std::uint32_t width, std::uint32_t height) : impl_(std::make_unique<Impl>()) {
    impl_->width = width;
    impl_->height = height;
    impl_->create_instance();
    impl_->create_device();
    impl_->create_resources();
}

OverlayTexture::~OverlayTexture() = default;

void OverlayTexture::show(vr::VROverlayHandle_t overlay, const std::uint8_t* rgba) {
    auto& v = *impl_;
    const VkImage image = v.images[v.next];
    std::memcpy(v.staging_map, rgba, static_cast<std::size_t>(v.width) * v.height * 4);

    v.ResetCommandBuffer(v.commands, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(v.BeginCommandBuffer(v.commands, &begin), "begin");
    v.barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {v.width, v.height, 1};
    v.CmdCopyBufferToImage(v.commands, v.staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // SteamVR expects submitted Vulkan textures in TRANSFER_SRC_OPTIMAL.
    v.barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    check(v.EndCommandBuffer(v.commands), "end");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &v.commands;
    check(v.ResetFences(v.device, 1, &v.fence), "reset fence");
    check(v.QueueSubmit(v.queue, 1, &submit, v.fence), "submit");
    check(v.WaitForFences(v.device, 1, &v.fence, VK_TRUE, 1000000000ull), "wait");

    vr::VRVulkanTextureData_t data{};
    data.m_nImage = reinterpret_cast<std::uint64_t>(image);
    data.m_pDevice = reinterpret_cast<VkDevice_T*>(v.device);
    data.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(v.physical);
    data.m_pInstance = reinterpret_cast<VkInstance_T*>(v.instance);
    data.m_pQueue = reinterpret_cast<VkQueue_T*>(v.queue);
    data.m_nQueueFamilyIndex = v.family;
    data.m_nWidth = v.width;
    data.m_nHeight = v.height;
    data.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM;
    data.m_nSampleCount = 1;
    vr::Texture_t texture{&data, vr::TextureType_Vulkan, vr::ColorSpace_Gamma};
    const auto error = vr::VROverlay()->SetOverlayTexture(overlay, &texture);
    if (error != vr::VROverlayError_None)
        throw std::runtime_error(std::string("SetOverlayTexture failed: ") +
            vr::VROverlay()->GetOverlayErrorNameFromEnum(error));
    v.next ^= 1;
}

} // namespace knockthru
