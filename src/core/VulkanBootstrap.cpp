#include "core/VulkanBootstrap.h"

#include <SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <sstream>
#include <vector>

namespace novaiso::core {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kSwapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

std::string VkResultText(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default: break;
    }
    return "VK_RESULT_" + std::to_string(static_cast<int>(result));
}

}  // namespace

bool VulkanBootstrap::QueueFamilyIndices::Complete() const {
    return graphics != UINT32_MAX && present != UINT32_MAX;
}

VulkanBootstrap::~VulkanBootstrap() {
    Shutdown();
}

bool VulkanBootstrap::Initialize(SDL_Window* window, std::string_view app_name, bool enable_validation) {
    Shutdown();
    window_ = window;
    validation_enabled_ = enable_validation && ValidationLayerAvailable(kValidationLayer);
    return CreateInstance(app_name, validation_enabled_) &&
           CreateSurface() &&
           PickPhysicalDevice() &&
           CreateLogicalDevice() &&
           CreateCommandPool();
}

void VulkanBootstrap::Shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    if (command_pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    window_ = nullptr;
    validation_enabled_ = false;
    families_ = {};
    physical_device_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
    present_queue_ = VK_NULL_HANDLE;
}

bool VulkanBootstrap::IsInitialized() const {
    return instance_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE;
}

VkInstance VulkanBootstrap::Instance() const {
    return instance_;
}

VkSurfaceKHR VulkanBootstrap::Surface() const {
    return surface_;
}

VkPhysicalDevice VulkanBootstrap::PhysicalDevice() const {
    return physical_device_;
}

VkDevice VulkanBootstrap::Device() const {
    return device_;
}

VkQueue VulkanBootstrap::GraphicsQueue() const {
    return graphics_queue_;
}

VkQueue VulkanBootstrap::PresentQueue() const {
    return present_queue_;
}

VkCommandPool VulkanBootstrap::CommandPool() const {
    return command_pool_;
}

VulkanBootstrap::QueueFamilyIndices VulkanBootstrap::Families() const {
    return families_;
}

bool VulkanBootstrap::RuntimeAvailable() {
    return RuntimeApiVersion() != 0;
}

std::uint32_t VulkanBootstrap::RuntimeApiVersion() {
    std::uint32_t version = VK_API_VERSION_1_0;
    const VkResult result = vkEnumerateInstanceVersion(&version);
    if (result != VK_SUCCESS) {
        return 0;
    }
    return version;
}

std::string VulkanBootstrap::RuntimeApiVersionString() {
    const std::uint32_t version = RuntimeApiVersion();
    if (version == 0) {
        return "unavailable";
    }
    std::ostringstream stream;
    stream << VK_VERSION_MAJOR(version) << '.' << VK_VERSION_MINOR(version) << '.' << VK_VERSION_PATCH(version);
    return stream.str();
}

bool VulkanBootstrap::CreateInstance(std::string_view app_name, bool enable_validation) {
    if (window_ == nullptr) {
        return false;
    }

    unsigned extension_count = 0;
    if (SDL_Vulkan_GetInstanceExtensions(window_, &extension_count, nullptr) != SDL_TRUE || extension_count == 0) {
        return false;
    }

    std::vector<const char*> extensions(extension_count);
    if (SDL_Vulkan_GetInstanceExtensions(window_, &extension_count, extensions.data()) != SDL_TRUE) {
        return false;
    }
    if (enable_validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = app_name.empty() ? "NovaIso Engine" : app_name.data();
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "NovaIso Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = RuntimeApiVersion();

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    std::array<const char*, 1> layers{kValidationLayer};
    if (enable_validation) {
        create_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        create_info.ppEnabledLayerNames = layers.data();
    }

    const VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
    return result == VK_SUCCESS;
}

bool VulkanBootstrap::CreateSurface() {
    return window_ != nullptr &&
           instance_ != VK_NULL_HANDLE &&
           SDL_Vulkan_CreateSurface(window_, instance_, &surface_) == SDL_TRUE;
}

bool VulkanBootstrap::PickPhysicalDevice() {
    std::uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(instance_, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count, VK_NULL_HANDLE);
    if (vkEnumeratePhysicalDevices(instance_, &device_count, devices.data()) != VK_SUCCESS) {
        return false;
    }

    for (VkPhysicalDevice candidate : devices) {
        QueueFamilyIndices candidate_families = FindQueueFamilies(candidate);
        if (!candidate_families.Complete()) {
            continue;
        }

        std::uint32_t extension_count = 0;
        if (vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr) != VK_SUCCESS) {
            continue;
        }
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, extensions.data()) != VK_SUCCESS) {
            continue;
        }

        bool has_swapchain = false;
        for (const auto& extension : extensions) {
            if (std::strcmp(extension.extensionName, kSwapchainExtension) == 0) {
                has_swapchain = true;
                break;
            }
        }
        if (!has_swapchain) {
            continue;
        }

        physical_device_ = candidate;
        families_ = candidate_families;
        return true;
    }

    return false;
}

bool VulkanBootstrap::CreateLogicalDevice() {
    if (physical_device_ == VK_NULL_HANDLE || !families_.Complete()) {
        return false;
    }

    const float queue_priority = 1.0f;
    std::set<std::uint32_t> unique_families{families_.graphics, families_.present};
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_families.size());
    for (std::uint32_t family : unique_families) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        queue_infos.push_back(queue_info);
    }

    VkPhysicalDeviceFeatures features{};

    std::array<const char*, 1> device_extensions{kSwapchainExtension};
    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    create_info.ppEnabledExtensionNames = device_extensions.data();
    create_info.pEnabledFeatures = &features;

    std::array<const char*, 1> layers{kValidationLayer};
    if (validation_enabled_) {
        create_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        create_info.ppEnabledLayerNames = layers.data();
    }

    const VkResult result = vkCreateDevice(physical_device_, &create_info, nullptr, &device_);
    if (result != VK_SUCCESS) {
        return false;
    }

    vkGetDeviceQueue(device_, families_.graphics, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, families_.present, 0, &present_queue_);
    return graphics_queue_ != VK_NULL_HANDLE && present_queue_ != VK_NULL_HANDLE;
}

bool VulkanBootstrap::CreateCommandPool() {
    if (device_ == VK_NULL_HANDLE || !families_.Complete()) {
        return false;
    }

    VkCommandPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    create_info.queueFamilyIndex = families_.graphics;
    return vkCreateCommandPool(device_, &create_info, nullptr, &command_pool_) == VK_SUCCESS;
}

VulkanBootstrap::QueueFamilyIndices VulkanBootstrap::FindQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices{};
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    if (family_count == 0) {
        return indices;
    }

    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
    for (std::uint32_t family_index = 0; family_index < family_count; ++family_index) {
        if ((families[family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            indices.graphics = family_index;
        }
        if (DeviceSupportsPresentation(device, family_index)) {
            indices.present = family_index;
        }
        if (indices.Complete()) {
            break;
        }
    }
    return indices;
}

bool VulkanBootstrap::DeviceSupportsPresentation(VkPhysicalDevice device, std::uint32_t queue_family) const {
    if (surface_ == VK_NULL_HANDLE) {
        return false;
    }
    VkBool32 supported = VK_FALSE;
    return vkGetPhysicalDeviceSurfaceSupportKHR(device, queue_family, surface_, &supported) == VK_SUCCESS &&
           supported == VK_TRUE;
}

bool VulkanBootstrap::ValidationLayerAvailable(const char* layer_name) const {
    std::uint32_t layer_count = 0;
    if (vkEnumerateInstanceLayerProperties(&layer_count, nullptr) != VK_SUCCESS || layer_count == 0) {
        return false;
    }
    std::vector<VkLayerProperties> layers(layer_count);
    if (vkEnumerateInstanceLayerProperties(&layer_count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, layer_name) == 0;
    });
}

}  // namespace novaiso::core
