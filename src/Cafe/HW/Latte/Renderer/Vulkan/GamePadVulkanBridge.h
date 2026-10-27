// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#pragma once
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "GamePadExtension.h"

class VulkanRenderer;
class LatteTextureView;
struct ImGuiContext;
struct ImGui_ImplVulkan_Offscreen;

class GamePadVulkanBridge
{
public:
    static bool ConfigureDevice(VkInstance instance, VkPhysicalDevice physical, VkDeviceCreateInfo& create,
                                std::vector<const char*>& extensions, VkPhysicalDeviceTimelineSemaphoreFeatures& timeline);
    explicit GamePadVulkanBridge(VulkanRenderer& renderer);
    ~GamePadVulkanBridge();
    void Capture(LatteTextureView* source);
private:
    struct Imported
    {
        uint64 generation{};
        VkImage image{};
        VkDeviceMemory memory{};
        VkSemaphore ready{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
        bool initialized{};
    };
    void Import(Imported& slot, const CgpFrame& frame);
    void Destroy(Imported& slot);
    void Draw(LatteTextureView* source, Imported& target, const CgpFrame& frame);
    void ReleaseToEncoder(Imported& target, const CgpFrame& frame);
    VulkanRenderer& m_renderer;
    std::array<Imported, CGP_MAX_SLOTS> m_slots;
    VkRenderPass m_renderPass{};
    ImGuiContext* m_context{};
    ImGui_ImplVulkan_Offscreen* m_overlay{};
    uint64 m_lastUiTick{};
    bool m_attached{};
    bool m_sharedRecorded{}, m_passOpen{};
};
