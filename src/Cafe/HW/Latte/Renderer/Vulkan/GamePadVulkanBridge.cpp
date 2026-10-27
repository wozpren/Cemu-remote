// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#include "GamePadVulkanBridge.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "Cafe/OS/libs/erreula/erreula.h"
#include "imgui/imgui_impl_vulkan.h"
#include "imgui/imgui_extension.h"
#include <imgui.h>
#include <cstring>

namespace
{
void CheckVk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) throw std::runtime_error(fmt::format("{} failed: {}", operation, static_cast<sint32>(result)));
}
template<class T> T InstanceFunction(VkInstance instance, const char* name)
{
    auto result = reinterpret_cast<T>(vkGetInstanceProcAddr(instance, name));
    if (!result) throw std::runtime_error(fmt::format("Missing Vulkan function {}", name));
    return result;
}
template<class T> T DeviceFunction(VkDevice device, const char* name)
{
    auto result = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
    if (!result) throw std::runtime_error(fmt::format("Missing Vulkan device function {}", name));
    return result;
}
}

bool GamePadVulkanBridge::ConfigureDevice(VkInstance instance, VkPhysicalDevice physical, VkDeviceCreateInfo& create,
                                        std::vector<const char*>& extensions, VkPhysicalDeviceTimelineSemaphoreFeatures& timeline)
{
    if (!RemoteGamePad::Enabled()) return false;
    try
    {
        uint32 count{};
        CheckVk(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "Enumerate GamePad GPU extensions");
        std::vector<VkExtensionProperties> available(count);
        CheckVk(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()), "Read GamePad GPU extensions");
        const char* required[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
        for (auto name : required)
            if (std::none_of(available.begin(), available.end(), [name](const auto& ext) { return std::strcmp(ext.extensionName, name) == 0; }))
                throw std::runtime_error(fmt::format("GPU does not support {}", name));
        timeline = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &timeline;
        InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2")(physical, &features);
        if (!timeline.timelineSemaphore) throw std::runtime_error("GPU timeline semaphore support is unavailable");
        VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &identity;
        InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2")(physical, &properties);
        if (properties.properties.vendorID != 0x10de || !identity.deviceLUIDValid)
            throw std::runtime_error("The first GamePad backend requires an NVIDIA Vulkan device with a valid LUID");
        VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkPhysicalDeviceImageFormatInfo2 image{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        image.pNext = &external;
        image.format = VK_FORMAT_B8G8R8A8_UNORM;
        image.type = VK_IMAGE_TYPE_2D;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkExternalImageFormatProperties exported{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 format{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        format.pNext = &exported;
        CheckVk(InstanceFunction<PFN_vkGetPhysicalDeviceImageFormatProperties2>(instance, "vkGetPhysicalDeviceImageFormatProperties2")(physical, &image, &format), "Query D3D11 texture import");
        if (!(exported.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            throw std::runtime_error("This Vulkan driver cannot import D3D11 encoder textures");
        VkPhysicalDeviceExternalSemaphoreInfo semaphore{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        semaphore.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        VkExternalSemaphoreProperties supported{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        InstanceFunction<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties")(physical, &semaphore, &supported);
        if (!(supported.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
            throw std::runtime_error("This Vulkan driver cannot import the D3D11 shared ready fence");
        for (auto name : required)
            if (std::none_of(extensions.begin(), extensions.end(), [name](const char* enabled) { return std::strcmp(enabled, name) == 0; }))
                extensions.push_back(name);
        timeline.pNext = const_cast<void*>(create.pNext);
        create.pNext = &timeline;
        create.enabledExtensionCount = static_cast<uint32>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        return true;
    }
    catch (const std::exception& ex)
    {
        cemuLog_log(LogType::Force, "[Remote GamePad] GPU interop unavailable: {}", ex.what());
        return false;
    }
}

GamePadVulkanBridge::GamePadVulkanBridge(VulkanRenderer& renderer) : m_renderer(renderer)
{
    auto physical = renderer.GetPhysicalDevice();
    VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &identity;
    InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>(renderer.GetVkInstance(), "vkGetPhysicalDeviceProperties2")(physical, &properties);
    CgpDevice context{sizeof(CgpDevice), properties.properties.vendorID, {}, properties.properties.driverVersion};
    std::memcpy(context.luid, identity.deviceLUID, 8);
    if (!RemoteGamePad::Attach(context)) throw std::runtime_error("Could not attach the GamePad hardware encoder");
    m_attached = true;
    RemoteGamePad::SetGame(true);
}

GamePadVulkanBridge::~GamePadVulkanBridge()
{
    // Called after Cemu's final GPU submission has completed, before either
    // the Vulkan device or the shared ImGui font atlas is destroyed.
    if (m_attached) RemoteGamePad::Detach();
    for (auto& slot : m_slots) Destroy(slot);
    if (m_overlay) ImGui_ImplVulkan_DestroyOffscreen(m_overlay);
    if (m_context) ImGui::DestroyContext(m_context);
    if (m_renderPass) vkDestroyRenderPass(m_renderer.GetLogicalDevice(), m_renderPass, nullptr);
}
void GamePadVulkanBridge::Destroy(Imported& slot)
{
    auto device = m_renderer.GetLogicalDevice();
    if (slot.framebuffer) vkDestroyFramebuffer(device, slot.framebuffer, nullptr);
    if (slot.view) vkDestroyImageView(device, slot.view, nullptr);
    if (slot.ready) vkDestroySemaphore(device, slot.ready, nullptr);
    if (slot.image) vkDestroyImage(device, slot.image, nullptr);
    if (slot.memory) vkFreeMemory(device, slot.memory, nullptr);
    slot = {};
}

void GamePadVulkanBridge::Import(Imported& slot, const CgpFrame& frame)
{
    if (frame.vk_format != VK_FORMAT_B8G8R8A8_UNORM || !frame.width || !frame.height || frame.width > 1920 || frame.height > 1080)
        throw std::runtime_error("Unsupported GamePad GPU export descriptor");
    if (slot.generation == frame.generation) return;
    Destroy(slot);
    auto device = m_renderer.GetLogicalDevice();
    try
    {
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.pNext = &external;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_B8G8R8A8_UNORM;
        image.extent = {frame.width, frame.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVk(vkCreateImage(device, &image, nullptr, &slot.image), "Create imported GamePad image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, slot.image, &requirements);
        VkMemoryWin32HandlePropertiesKHR handle{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        CheckVk(DeviceFunction<PFN_vkGetMemoryWin32HandlePropertiesKHR>(device, "vkGetMemoryWin32HandlePropertiesKHR")(
            device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, reinterpret_cast<HANDLE>(frame.texture_handle), &handle), "Query shared encoder texture memory");
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(m_renderer.GetPhysicalDevice(), &memory);
        uint32 type = memory.memoryTypeCount;
        for (uint32 i = 0; i < memory.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & handle.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        if (type == memory.memoryTypeCount) throw std::runtime_error("No device-local memory type can import this encoder texture");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = slot.image;
        VkImportMemoryWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imported.pNext = &dedicated;
        imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imported.handle = reinterpret_cast<HANDLE>(frame.texture_handle);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &imported;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        CheckVk(vkAllocateMemory(device, &allocation, nullptr, &slot.memory), "Import encoder texture allocation");
        CheckVk(vkBindImageMemory(device, slot.image, slot.memory, 0), "Bind imported encoder texture");
        VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphore.pNext = &typeInfo;
        CheckVk(vkCreateSemaphore(device, &semaphore, nullptr, &slot.ready), "Create GamePad timeline semaphore");
        VkImportSemaphoreWin32HandleInfoKHR fence{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        fence.semaphore = slot.ready;
        fence.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        fence.handle = reinterpret_cast<HANDLE>(frame.ready_fence_handle);
        CheckVk(DeviceFunction<PFN_vkImportSemaphoreWin32HandleKHR>(device, "vkImportSemaphoreWin32HandleKHR")(device, &fence), "Import encoder ready fence");
        if (!m_renderPass)
        {
            VkAttachmentDescription attachment{};
            attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &reference;
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            pass.attachmentCount = 1;
            pass.pAttachments = &attachment;
            pass.subpassCount = 1;
            pass.pSubpasses = &subpass;
            CheckVk(vkCreateRenderPass(device, &pass, nullptr, &m_renderPass), "Create offscreen GamePad render pass");
        }
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = slot.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_B8G8R8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVk(vkCreateImageView(device, &view, nullptr, &slot.view), "Create offscreen GamePad view");
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = m_renderPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &slot.view;
        framebuffer.width = frame.width;
        framebuffer.height = frame.height;
        framebuffer.layers = 1;
        CheckVk(vkCreateFramebuffer(device, &framebuffer, nullptr, &slot.framebuffer), "Create offscreen GamePad framebuffer");
        slot.generation = frame.generation;
    }
    catch (...) { Destroy(slot); throw; }
}

void GamePadVulkanBridge::Draw(LatteTextureView* source, Imported& target, const CgpFrame& frame)
{
    bool uiFrame{};
    struct RestoreContext
    {
        ImGuiContext* previous;
        bool& frame;
        ~RestoreContext() { if (frame) ImGui::EndFrame(); ImGui::SetCurrentContext(previous); }
    } restore{ImGui::GetCurrentContext(), uiFrame};
    auto& renderer = m_renderer;
    if (!m_context)
    {
        m_context = ImGui::CreateContext(renderer.imguiFontAtlas);
        ImGui::SetCurrentContext(m_context);
        ImGui::GetIO().IniFilename = nullptr;
    }
    ImGui::SetCurrentContext(m_context);
    ImGui_PrecacheFonts();
    // Loading a requested font calls Renderer::Flush(true), which submits the
    // old command buffer and begins another. Acquire the command buffer and
    // record resource lifetimes only after that flush; the previous handle is
    // no longer recording and cannot receive the font-upload commands below.
    auto command = renderer.getCurrentCommandBuffer();
    ImGui_ImplVulkan_CreateFontsTexture(command);
    if (!m_overlay)
    {
        std::shared_lock lock(renderer.m_pipeline_cache_save_mutex);
        m_overlay = ImGui_ImplVulkan_CreateOffscreen(m_renderPass, CGP_MAX_SLOTS);
    }
    auto state = RemoteGamePad::ReadInput(2);
    auto& io = ImGui::GetIO();
    auto now = RemoteGamePad::Now();
    io.DeltaTime = m_lastUiTick ? std::clamp(float(now - m_lastUiTick) * 1e-9f, 0.001f, 0.25f) : 1.f / 60;
    m_lastUiTick = now;
    io.DisplaySize = {float(frame.width), float(frame.height)};
    io.DisplayFramebufferScale = {1, 1};
    io.AddMousePosEvent(state.pointer_x * frame.width, state.pointer_y * frame.height);
    io.AddMouseButtonEvent(0, state.connected && state.pointer_down);
    ImGui::NewFrame();
    uiFrame = true;
    swkbd_render(false);
    nn::erreula::render(false);
    ImGui::Render();
    uiFrame = false;
    RemoteGamePad::SetOverlayFocus(io.WantCaptureMouse);

    auto* sourceView = static_cast<LatteTextureViewVk*>(source);
    sourceView->GetBaseImage()->GetImageObj()->flagForCurrentCommandBuffer();
    sourceView->GetViewRGBA()->flagForCurrentCommandBuffer();
    auto* shader = RendererOutputShader::s_copy_shader;
    auto pipeline = renderer.backbufferBlit_createGraphicsPipeline(renderer.m_swapchainDescriptorSetLayout, true, shader, m_renderPass);
    auto descriptor = renderer.backbufferBlit_createDescriptorSet(renderer.m_swapchainDescriptorSetLayout, sourceView, true);
    auto uniforms = shader->FillUniformBlockBuffer(*source, {static_cast<sint32>(frame.content_width), static_cast<sint32>(frame.content_height)}, true);
    auto offset = renderer.uniformData_uploadUniformDataBufferGetOffset({reinterpret_cast<uint8*>(&uniforms), sizeof(uniforms)});

    VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    acquire.oldLayout = target.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    acquire.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    acquire.dstQueueFamilyIndex = renderer.m_indices.graphicsFamily;
    acquire.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    acquire.image = target.image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &acquire);
    m_sharedRecorded = true;
    target.initialized = true;
    VkMemoryBarrier sourceReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    sourceReady.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    sourceReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &sourceReady, 0, nullptr, 0, nullptr);
    VkClearValue background{};
    background.color.float32[3] = 1;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = m_renderPass;
    begin.framebuffer = target.framebuffer;
    begin.renderArea.extent = {frame.width, frame.height};
    begin.clearValueCount = 1;
    begin.pClearValues = &background;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    m_passOpen = true;
    VkViewport viewport{frame.content_x, frame.content_y, frame.content_width, frame.content_height, 0, 1};
    VkRect2D scissor{{0, 0}, {frame.width, frame.height}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.m_pipelineLayout, 0, 1, &descriptor, 1, &offset);
    vkCmdDraw(command, 6, 1, 0, 0);
    ImGui_ImplVulkan_RenderOffscreen(m_overlay, frame.slot, ImGui::GetDrawData(), command);
    vkCmdEndRenderPass(command);
    m_passOpen = false;
    vkCmdSetViewport(command, 0, 1, &renderer.m_state.currentViewport);
    vkCmdSetScissor(command, 0, 1, &renderer.m_state.currentScissorRect);
    renderer.m_state.currentPipeline = VK_NULL_HANDLE;
    target.initialized = true;
}

void GamePadVulkanBridge::ReleaseToEncoder(Imported& target, const CgpFrame& frame)
{
    VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    release.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    release.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release.srcQueueFamilyIndex = m_renderer.m_indices.graphicsFamily;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release.image = target.image;
    release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(m_renderer.getCurrentCommandBuffer(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &release);
    // Queue ownership and signal submission remain on Cemu's render thread.
    // The plugin sees the lease only after this submission has succeeded.
    m_renderer.SubmitCommandBuffer(VK_NULL_HANDLE, VK_NULL_HANDLE, target.ready, frame.ready_value);
    RemoteGamePad::Submit(frame);
}

void GamePadVulkanBridge::Capture(LatteTextureView* source)
{
    RemoteGamePad::UpdateBinding();
    CgpFrame frame{};
    if (!RemoteGamePad::Acquire(frame)) return;
    bool committed{};
    m_sharedRecorded = m_passOpen = false;
    try
    {
        auto& slot = m_slots.at(frame.slot);
        Import(slot, frame);
        LatteTexture_UpdateDataToLatest(source->baseTexture);
        LatteTC_MarkTextureStillInUse(source->baseTexture);
        sint32 width{}, height{};
        source->baseTexture->GetEffectiveSize(width, height, source->firstMip);
        if (width <= 0 || height <= 0) throw std::runtime_error("Invalid DRC source texture extent");
        float scale = std::min(float(frame.width) / width, float(frame.height) / height);
        frame.content_width = std::max(1.f, std::floor(width * scale));
        frame.content_height = std::max(1.f, std::floor(height * scale));
        frame.content_x = std::floor((frame.width - frame.content_width) * 0.5f);
        frame.content_y = std::floor((frame.height - frame.content_height) * 0.5f);
        m_renderer.draw_endRenderPass();
        Draw(source, slot, frame);
        ReleaseToEncoder(slot, frame);
        committed = true;
    }
    catch (const std::exception& ex)
    {
        if (!committed && m_sharedRecorded)
        {
            // Recorded GPU work cannot be cancelled by merely freeing its
            // lease. Submit its release signal before stopping the session.
            if (m_passOpen) vkCmdEndRenderPass(m_renderer.getCurrentCommandBuffer());
            m_renderer.m_state.currentPipeline = VK_NULL_HANDLE;
            ReleaseToEncoder(m_slots.at(frame.slot), frame);
            committed = true;
        }
        if (!committed) RemoteGamePad::Cancel(frame);
        RemoteGamePad::ReportError(ex.what());
    }
}

void RemoteGamePad::Capture(LatteTextureView* source)
{
    if (auto* renderer = dynamic_cast<VulkanRenderer*>(g_renderer.get())) renderer->StreamRemoteGamePad(source);
    else
    {
        static bool reported{};
        if (!reported)
        {
            cemuLog_log(LogType::Force, "[Remote GamePad] Streaming requires the Vulkan renderer; no CPU capture fallback is enabled");
            reported = true;
        }
    }
}
