//
// Created by micha on 11/22/2023.
//

#include <engine/util/Log.hpp>
#include <engine/graphics/Window.hpp>
#include <engine/graphics/VulkanAPI.hpp>
#include <engine/graphics/vulkan/Swapchain.hpp>
#include <engine/graphics/vulkan/CommandRecorder.hpp>
#include <engine/graphics/Camera.hpp>
#include <engine/graphics/renderer/ImGuiRenderer.hpp>
#include <imgui.h>
#include <engine/graphics/renderer/DensityPathTracer.hpp>
#include <engine/util/read_file.hpp>
#include <engine/graphics/vulkan/Texture3D.hpp>
#include <engine/util/Input.hpp>
#include <engine/util/Time.hpp>
#include <engine/graphics/Sun.hpp>
#include <engine/graphics/NeuralRadianceCache.hpp>

en::DensityPathTracer* pathTracer = nullptr;

void RecordSwapchainCommandBuffer(VkCommandBuffer commandBuffer, VkImage image)
{
    uint32_t width = en::Window::GetWidth();
    uint32_t height = en::Window::GetHeight();

    VkCommandBufferBeginInfo beginInfo;
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.pNext = nullptr;
    beginInfo.flags = 0;
    beginInfo.pInheritanceInfo = nullptr;

    VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS)
        en::Log::Error("Failed to begin VkCommandBuffer", true);

    en::vk::CommandRecorder::ImageLayoutTransfer(
            commandBuffer,
            image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_NONE_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (pathTracer != nullptr && en::ImGuiRenderer::IsInitialized())
    {
        VkImageCopy imageCopy;
        imageCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageCopy.srcSubresource.mipLevel = 0;
        imageCopy.srcSubresource.baseArrayLayer = 0;
        imageCopy.srcSubresource.layerCount = 1;
        imageCopy.srcOffset = { 0, 0, 0 };
        imageCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageCopy.dstSubresource.mipLevel = 0;
        imageCopy.dstSubresource.baseArrayLayer = 0;
        imageCopy.dstSubresource.layerCount = 1;
        imageCopy.dstOffset = { 0, 0, 0 };
        imageCopy.extent = { width, height, 1 };

        vkCmdCopyImage(
                commandBuffer,
                en::ImGuiRenderer::GetImage(),
                //pathTracer->GetImage(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &imageCopy);
    }

    en::vk::CommandRecorder::ImageLayoutTransfer(
            commandBuffer,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS)
        en::Log::Error("Failed to end VkCommandBuffer", true);
}

void SwapchainResizeCallback()
{
    en::Window::WaitForUsableSize();
    vkDeviceWaitIdle(en::VulkanAPI::GetDevice());

    uint32_t width = en::Window::GetWidth();
    uint32_t height = en::Window::GetHeight();
    pathTracer->ResizeFrame(width, height);
    en::ImGuiRenderer::Resize(width, height);
    en::ImGuiRenderer::SetBackgroundImageView(pathTracer->GetImageView());
}

int main()
{
    std::string appName("NRC-HPM-Renderer");
    uint32_t width = 600;
    uint32_t height = width;

    // Start engine
    en::Log::Info("Starting " + appName);

    en::Window::Init(width, height, true, appName);
    en::Input::Init(en::Window::GetGLFWHandle());
    en::VulkanAPI::Init(appName);

    // Load data
    auto density3D = en::ReadFileDensity3D("data/cloud_sixteenth", 125, 85, 153);
    en::vk::Texture3D density3DTex(density3D, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    en::VolumeData volumeData(&density3DTex);

    // Setup rendering
    en::Camera camera(
            glm::vec3(0.0f, 0.0f, -5.0f),
            glm::vec3(0.0f, 0.0f, 1.0f),
            glm::vec3(0.0f, 1.0f, 0.0f),
            static_cast<float>(width) / static_cast<float>(height),
            glm::radians(60.0f),
            0.1f,
            100.0f);

    en::Sun sun(-1.57f, 0.0f, glm::vec3(1.0f));

    en::vk::Swapchain swapchain(width, height, RecordSwapchainCommandBuffer, SwapchainResizeCallback);

    pathTracer = new en::DensityPathTracer(width, height, &camera, &volumeData, &sun);

    en::ImGuiRenderer::Init(width, height);
    en::ImGuiRenderer::SetBackgroundImageView(pathTracer->GetImageView());

    swapchain.Resize(width, height); // Rerecords commandbuffers (needs to be called if renderer are created)

    en::NeuralRadianceCache nrc;

    // Main loop
    VkDevice device = en::VulkanAPI::GetDevice();
    VkQueue graphicsQueue = en::VulkanAPI::GetGraphicsQueue();
    VkResult result;
    while (!en::Window::IsClosed())
    {
        // Update
        en::Window::Update();
        en::Input::Update();
        en::Time::Update();
        width = en::Window::GetWidth();
        height = en::Window::GetHeight();

        float deltaTime = static_cast<float>(en::Time::GetDeltaTime());
        uint32_t fps = en::Time::GetFps();
        en::Input::HandleUserCamInput(&camera, deltaTime);
        en::Window::SetTitle(appName + " | Delta time: " + std::to_string(deltaTime) + "s | Fps: " + std::to_string(fps));

        // Physics
        camera.SetAspectRatio(width, height);
        camera.UpdateUniformBuffer();

        // Render
//        en::ImGuiRenderer::StartFrame();

        pathTracer->Render(graphicsQueue);
        result = vkQueueWaitIdle(graphicsQueue);
        ASSERT_VULKAN(result);

        en::ImGuiRenderer::StartFrame();

        volumeData.RenderImGui();
        volumeData.Update();
        sun.RenderImgui();

        en::ImGuiRenderer::EndFrame(graphicsQueue);
        result = vkQueueWaitIdle(graphicsQueue);
        ASSERT_VULKAN(result);

        swapchain.DrawAndPresent();
    }
    result = vkDeviceWaitIdle(device);
    ASSERT_VULKAN(result);

    // End
    density3DTex.Destroy();

    en::ImGuiRenderer::Shutdown();

    pathTracer->Destroy();
    delete pathTracer;

    swapchain.Destroy(true);

    camera.Destroy();

    sun.Destroy();

    en::VulkanAPI::Shutdown();
    en::Window::Shutdown();

    en::Log::Info("Ending " + appName);

    return 0;
}