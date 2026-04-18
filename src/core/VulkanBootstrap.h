#pragma once

#include <SDL.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace novaiso::core {

class VulkanBootstrap {
public:
    struct QueueFamilyIndices {
        std::uint32_t graphics = UINT32_MAX;
        std::uint32_t present = UINT32_MAX;

        [[nodiscard]] bool Complete() const;
    };

    VulkanBootstrap() = default;
    ~VulkanBootstrap();

    VulkanBootstrap(const VulkanBootstrap&) = delete;
    VulkanBootstrap& operator=(const VulkanBootstrap&) = delete;

    bool Initialize(SDL_Window* window, std::string_view app_name, bool enable_validation = false);
    void Shutdown();

    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] VkInstance Instance() const;
    [[nodiscard]] VkSurfaceKHR Surface() const;
    [[nodiscard]] VkPhysicalDevice PhysicalDevice() const;
    [[nodiscard]] VkDevice Device() const;
    [[nodiscard]] VkQueue GraphicsQueue() const;
    [[nodiscard]] VkQueue PresentQueue() const;
    [[nodiscard]] VkCommandPool CommandPool() const;
    [[nodiscard]] QueueFamilyIndices Families() const;

    static bool RuntimeAvailable();
    static std::uint32_t RuntimeApiVersion();
    static std::string RuntimeApiVersionString();

private:
    bool CreateInstance(std::string_view app_name, bool enable_validation);
    bool CreateSurface();
    bool PickPhysicalDevice();
    bool CreateLogicalDevice();
    bool CreateCommandPool();
    [[nodiscard]] QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
    [[nodiscard]] bool DeviceSupportsPresentation(VkPhysicalDevice device, std::uint32_t queue_family) const;
    [[nodiscard]] bool ValidationLayerAvailable(const char* layer_name) const;

    SDL_Window* window_ = nullptr;
    bool validation_enabled_ = false;
    QueueFamilyIndices families_{};
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
};

}  // namespace novaiso::core
