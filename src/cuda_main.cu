//#include <cuda_main.hpp>

#define VK_USE_PLATFORM_WIN32_KHR

#include <aclapi.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <VersionHelpers.h>

#include <engine/util/Log.hpp>
#include <engine/graphics/Window.hpp>
#include <engine/graphics/VulkanAPI.hpp>
#include <engine/graphics/vulkan/CommandPool.hpp>
#include <engine/graphics/vulkan/CommandRecorder.hpp>
#include <engine/graphics/renderer/ImGuiRenderer.hpp>
#include <engine/graphics/vulkan/Swapchain.hpp>
#include <imgui.h>
#include <engine/graphics/renderer/NrcHpmRenderer.hpp>
#include <engine/graphics/NeuralRadianceCache.hpp>
#include <engine/util/read_file.hpp>
#include <engine/util/Input.hpp>
#include <engine/util/Time.hpp>
#include <engine/HpmScene.hpp>
#include <engine/AppConfig.hpp>
#include <filesystem>
#include <engine/graphics/renderer/McHpmRenderer.hpp>
#include <tinyexr.h>
#include <engine/graphics/vulkan/CommandPool.hpp>

#include <cuda_runtime.h>
#include <tiny-cuda-nn/config.h>
#include <vulkan/vulkan.h>
en::McHpmRenderer* gtRenderer = nullptr;
std::array<en::Camera*, 6> testCameras = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
std::array<float*, 6> gtImages = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

#define ASSERT_CUDA(error) if (error != cudaSuccess) { en::Log::Error("Cuda assert triggered: " + std::string(cudaGetErrorName(error)), true); }

en::NrcHpmRenderer* nrcHpmRenderer = nullptr;
en::McHpmRenderer* mcHpmRenderer = nullptr;

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

    if (nrcHpmRenderer != nullptr && mcHpmRenderer != nullptr && en::ImGuiRenderer::IsInitialized())
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
    vkDeviceWaitIdle(en::VulkanAPI::GetDevice()); // TODO: causes error with multithreaded rendering

    en::Log::Info("Skipping swapchain resize callback");
}

void Benchmark(
        uint32_t width,
        uint32_t height,
        uint32_t sceneID,
        const en::AppConfig& appConfig,
        const en::HpmScene& scene,
        const en::Camera* oldCamera,
        VkQueue queue)
{
    // Create output path if not exists
    std::string outputDirPath = "output/ " + appConfig.GetName() + "/";
    if (!std::filesystem::is_directory(outputDirPath) || !std::filesystem::exists(outputDirPath))
    {
        std::filesystem::create_directory(outputDirPath);
    }

    // Create reference folder if not exists
    std::string referenceDirPath = "output/" + std::to_string(sceneID) + "/";
    if (!std::filesystem::is_directory(referenceDirPath) || !std::filesystem::exists(referenceDirPath))
    {
        en::Log::Info("Reference folder for scene " + std::to_string(sceneID) + " was not found. Creating reference images");

        // Create folder
        std::filesystem::create_directory(referenceDirPath);

        // Generate reference data
        for (size_t i = 0; i < testCameras.size(); i++)
        {
            en::Log::Info("Generating reference image " + std::to_string(i));

            // Set new camera
            gtRenderer->SetCamera(queue, testCameras[i]);

            // Generate reference image
            for (size_t frame = 0; frame < 8192; frame++)
            {
                gtRenderer->Render(queue);
                ASSERT_VULKAN(vkQueueWaitIdle(queue));
            }

            // Export reference image
            gtRenderer->ExportOutputImageToFile(queue, referenceDirPath + std::to_string(i) + ".exr");
        }
    }

    // Load reference images from folder
    for (size_t i = 0; i < testCameras.size(); i++)
    {
        if (gtImages[i] != nullptr) { continue; }
        int exrWidth;
        int exrHeight;

        const std::string exrFilePath = referenceDirPath + std::to_string(i) + ".exr";
        if (TINYEXR_SUCCESS != LoadEXR(&gtImages[i], &exrWidth, &exrHeight, exrFilePath.c_str(), nullptr))
        {
            en::Log::Error("Tinyexr failed to load " + exrFilePath, true);
        }

        if (exrWidth != width || exrHeight != height)
        {
            en::Log::Error("Extent of loaded reference image does not match renderer extent", true);
        }
    }

    // Test frame
    const bool prevNrcBlend = nrcHpmRenderer->IsBlending();
    const bool prevMcBlend = mcHpmRenderer->IsBlending();
    nrcHpmRenderer->SetBlend(true);
    mcHpmRenderer->SetBlend(true);

    std::array<float, testCameras.size()> nrcMseLosses;
    std::array<float, testCameras.size()> mcMseLosses;
    for (size_t i = 0; i < testCameras.size(); i++)
    {
        nrcHpmRenderer->SetCamera(queue, testCameras[i]);
        mcHpmRenderer->SetCamera(queue, testCameras[i]);
        for (size_t frame = 0; frame < 1; frame++)
        {
            nrcHpmRenderer->Render(queue);
            ASSERT_VULKAN(vkQueueWaitIdle(queue));

            mcHpmRenderer->Render(queue);
            ASSERT_VULKAN(vkQueueWaitIdle(queue));
        }
        nrcMseLosses[i] = nrcHpmRenderer->CompareReferenceMSE(queue, gtImages[i]);
        mcMseLosses[i] = mcHpmRenderer->CompareReferenceMSE(queue, gtImages[i]);
    }

    nrcHpmRenderer->SetBlend(prevNrcBlend);
    mcHpmRenderer->SetBlend(prevMcBlend);

    // Calculate total loss
    float nrcMSE = 0.0f;
    float mcMSE = 0.0f;
    for (size_t i = 0; i < testCameras.size(); i++)
    {
        nrcMSE += nrcMseLosses[i];
        mcMSE += mcMseLosses[i];
    }
    const float frameCountF = static_cast<float>(testCameras.size());
    en::Log::Info("NRC MSE: " + std::to_string(nrcMSE / frameCountF));
    en::Log::Info("MC MSE: " + std::to_string(mcMSE / frameCountF));

    // Reset camera
    nrcHpmRenderer->SetCamera(queue, oldCamera);
    mcHpmRenderer->SetCamera(queue, oldCamera);
}

bool RunAppConfigInstance(const en::AppConfig& appConfig)
{
    // Start engine
    const std::string appName("NeuralRadianceCaching");
    uint32_t width = appConfig.renderWidth;
    uint32_t height = appConfig.renderHeight;
    en::Log::Info("Starting " + appName);

    en::Window::Init(width, height, false, appName);
    if (en::Window::IsSupported()) { en::Input::Init(en::Window::GetGLFWHandle()); }
    en::VulkanAPI::Init(appName);
    const VkDevice device = en::VulkanAPI::GetDevice();
    const uint32_t qfi = en::VulkanAPI::GetGraphicsQFI();
    const VkQueue queue = en::VulkanAPI::GetGraphicsQueue();

    // Renderer select
    const std::vector<char*> rendererMenuItems = { "MC", "NRC" }; // TODO: Restir
    const char* currentRendererMenuItem = rendererMenuItems[1];
    uint32_t rendererId = 1;

    // Init resources
    en::NeuralRadianceCache nrc(appConfig);

    en::HpmScene hpmScene(appConfig);

    // Setup rendering
    const float aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    en::Camera camera(
            glm::vec3(64.0f, 0.0f, 0.0f),
            glm::vec3(-1.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f),
            aspectRatio,
            glm::radians(60.0f),
            0.1f,
            100.0f);

    testCameras = {
            new en::Camera(
                    glm::vec3(64.0f, 0.0f, 0.0f),
                    glm::vec3(-1.0f, 0.0f, 0.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    aspectRatio,
                    glm::radians(60.0f),
                    0.1f,
                    100.0f),
            new	en::Camera(
                    glm::vec3(-64.0f, 0.0f, 0.0f),
                    glm::vec3(1.0f, 0.0f, 0.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    aspectRatio,
                    glm::radians(60.0f),
                    0.1f,
                    100.0f),
            new en::Camera(
                    glm::vec3(0.0f, 64.0f, 0.0f),
                    glm::vec3(0.0f, -1.0f, 0.0f),
                    glm::vec3(1.0f, 0.0f, 0.0f),
                    aspectRatio,
                    glm::radians(60.0f),
                    0.1f,
                    100.0f),
            new en::Camera(
                    glm::vec3(0.0f, -64.0f, 0.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    glm::vec3(1.0f, 0.0f, 0.0f),
                    aspectRatio,
                    glm::radians(60.0f),
                    0.1f,
                    100.0f),
            new en::Camera(
                    glm::vec3(0.0f, 0.0f, 64.0f),
                    glm::vec3(0.0f, 0.0f, -1.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    aspectRatio,
                    glm::radians(60.0f),
                    0.1f,
                    100.0f),
            new en::Camera(
                    glm::vec3(0.0f, 0.0f, -64.0f),
                    glm::vec3(0.0f, 0.0f, 1.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    aspectRatio,
                    glm::radians(60.0f),
                    0.1f,
                    100.0f)
    };

    // Init rendering pipeline
    en::vk::Swapchain* swapchain = nullptr;
    if (en::Window::IsSupported())
    {
        swapchain = new en::vk::Swapchain(width, height, RecordSwapchainCommandBuffer, SwapchainResizeCallback);
    }

    nrcHpmRenderer = new en::NrcHpmRenderer(
            width,
            height,
            appConfig.trainSampleRatio,
            appConfig.trainSpp,
            false,
            &camera,
            hpmScene,
            nrc);

    mcHpmRenderer = new en::McHpmRenderer(width, height, 32, false, &camera, hpmScene);
    gtRenderer = new en::McHpmRenderer(width, height, 64, true, &camera, hpmScene);

    if (en::Window::IsSupported())
    {
        en::ImGuiRenderer::Init(width, height);
        switch (rendererId)
        {
            case 0: // MC
                en::ImGuiRenderer::SetBackgroundImageView(mcHpmRenderer->GetImageView());
                break;
            case 1: // NRC
                en::ImGuiRenderer::SetBackgroundImageView(nrcHpmRenderer->GetImageView());
                break;
            default: // Error
                en::Log::Error("Renderer ID is invalid", true);
                break;
        }
    }

    // Swapchain rerecording because imgui renderer is now available
    if (en::Window::IsSupported()) { swapchain->Resize(width, height); }

    en::Log::Info(std::to_string(en::VulkanAPI::GetTimestampPeriod()));

    // Main loop
    VkResult result;
    size_t frameCount = 0;
    bool shutdown = false;
    bool restartAfterClose = false;
    bool benchmark = true;
    bool continueLoop = en::Window::IsSupported() ? !en::Window::IsClosed() : true;
    while (continueLoop && !shutdown)
    {
        // Exit
        //if (frameCount == 10) { break; }

        // Update
        if (en::Window::IsSupported())
        {
            en::Window::Update();
            en::Input::Update();
        }
        en::Time::Update();

        if (en::Window::IsSupported())
        {
            width = en::Window::GetWidth();
            height = en::Window::GetHeight();
        }

        float deltaTime = static_cast<float>(en::Time::GetDeltaTime());
        uint32_t fps = en::Time::GetFps();

        // Physics
        if (en::Window::IsSupported())
        {
            en::Input::HandleUserCamInput(&camera, deltaTime);
            camera.SetAspectRatio(width, height);
        }
        camera.UpdateUniformBuffer();

        // Render
        // Always render nrc for training
//        nrcHpmRenderer->Render(queue);
//        result = vkQueueWaitIdle(queue);
//        ASSERT_VULKAN(result);
//        nrcHpmRenderer->EvaluateTimestampQueries();

        switch (rendererId)
        {
            case 0: // MC
                mcHpmRenderer->Render(queue);
                result = vkQueueWaitIdle(queue);
                ASSERT_VULKAN(result);
                mcHpmRenderer->EvaluateTimestampQueries();
                break;
            case 1: // NRC
                nrcHpmRenderer->Render(queue);
                result = vkQueueWaitIdle(queue);
                ASSERT_VULKAN(result);
                nrcHpmRenderer->EvaluateTimestampQueries();
                break;
            default: // Error
                en::Log::Error("Renderer ID is invalid", true);
                break;
        }

        // Imgui
        if (en::Window::IsSupported())
        {
            en::ImGuiRenderer::StartFrame();

            ImGui::Begin("Statistics");
            ImGui::Text((std::string("Framecount ") + std::to_string(frameCount)).c_str());
            ImGui::Text("DeltaTime %f", deltaTime);
            ImGui::Text("FPS %d", fps);
            ImGui::Text("NRC Loss %f", nrc.GetLoss());
            ImGui::End();

            ImGui::Begin("Controls");
            shutdown = ImGui::Button("Shutdown");
            ImGui::Checkbox("Restart after shutdown", &restartAfterClose);
            ImGui::Checkbox("Benchmark", &benchmark);

            if (ImGui::BeginCombo("##combo", currentRendererMenuItem))
            {
                for (int i = 0; i < rendererMenuItems.size(); i++)
                {
                    bool selected = (currentRendererMenuItem == rendererMenuItems[i]);
                    if (ImGui::Selectable(rendererMenuItems[i], selected))
                    {
                        if (i != rendererId)
                        {
                            rendererId = i;
                            switch (rendererId)
                            {
                                case 0: // MC
                                    en::ImGuiRenderer::SetBackgroundImageView(mcHpmRenderer->GetImageView());
                                    break;
                                case 1: // NRC
                                    en::ImGuiRenderer::SetBackgroundImageView(nrcHpmRenderer->GetImageView());
                                    break;
                                default: // Error
                                    en::Log::Error("Renderer ID is invalid", true);
                                    break;
                            }
                        }
                        currentRendererMenuItem = rendererMenuItems[i];
                    };
                    if (selected) { ImGui::SetItemDefaultFocus(); }
                }
                ImGui::EndCombo();
            }

            ImGui::End();

            mcHpmRenderer->RenderImGui();
            nrcHpmRenderer->RenderImGui();

            hpmScene.Update(true);

            appConfig.RenderImGui();

            en::ImGuiRenderer::EndFrame(queue, VK_NULL_HANDLE);
            result = vkQueueWaitIdle(queue);
            ASSERT_VULKAN(result);
        }

        // Display
        if (en::Window::IsSupported()) { swapchain->DrawAndPresent(VK_NULL_HANDLE, VK_NULL_HANDLE); }

        // Check loss
        if (benchmark && frameCount % 100 == 0)
        {
            en::Log::Info("Frame: " + std::to_string(frameCount));
            Benchmark(appConfig.renderWidth, appConfig.renderHeight, appConfig.scene.id, appConfig, hpmScene, &camera, queue);
        }

        //
        frameCount++;
        continueLoop = en::Window::IsSupported() ? !en::Window::IsClosed() : true;
    }

    // Evaluate at end
//    Benchmark(appConfig.renderWidth, appConfig.renderHeight, appConfig.scene.id, appConfig, hpmScene, queue);
//    std::string outputDirPath = "output/ " + appConfig.GetName() + "/";
//    if (!std::filesystem::is_directory(outputDirPath) || std::filesystem::exists(outputDirPath))
//    {
//        std::filesystem::create_directory(outputDirPath);
//    }
//    std::string exrOutputFilePath =  outputDirPath + "1.exr";
//
//    // TODO: end evaluation
//    switch (rendererId)
//    {
//        case 0: // MC
//            mcHpmRenderer->ExportImageToFile(queue, exrOutputFilePath);
//            break;
//        case 1: // NRC
//            nrcHpmRenderer->ExportImageToFile(queue, exrOutputFilePath);
//            break;
//        default: // Error
//            en::Log::Error("Renderer ID is invalid", true);
//            break;
//    }

    // Stop gpu work
    result = vkDeviceWaitIdle(device);
    ASSERT_VULKAN(result);

    // End
    for (size_t i = 0; i < testCameras.size(); i++)
    {
        free(gtImages[i]);
    }

    gtRenderer->Destroy();
    delete gtRenderer;

    mcHpmRenderer->Destroy();
    delete mcHpmRenderer;

    nrcHpmRenderer->Destroy();
    delete nrcHpmRenderer;

    en::ImGuiRenderer::Shutdown();

    if (en::Window::IsSupported) { swapchain->Destroy(true); }

    for (size_t i = 0; i < testCameras.size(); i++)
    {
        testCameras[i]->Destroy();
        delete testCameras[i];
    }

    hpmScene.Destroy();
    camera.Destroy();
    nrc.Destroy();

    en::VulkanAPI::Shutdown();
    if (en::Window::IsSupported()) { en::Window::Shutdown(); }

    en::Log::Info("Ending " + appName);

    return restartAfterClose;
}

int main(int argc, char** argv)
{
    en::AppConfig appConfig(argc, argv);

    bool restartRunConfig;
    do {
        restartRunConfig = RunAppConfigInstance(appConfig);
    } while (restartRunConfig);

    return 0;
}
