#include <engine/graphics/Reference.hpp>
#include <filesystem>
#include <engine/graphics/renderer/McHpmRenderer.hpp>
#include <engine/graphics/VulkanAPI.hpp>
#include <engine/graphics/vulkan/CommandRecorder.hpp>

namespace en
{
    void Reference::Result::Norm(uint32_t width, uint32_t height)
    {
        const float normFactor = 1.0f / static_cast<float>(width * height);
        mse *= normFactor;
        biasX *= normFactor;
        biasY *= normFactor;
        biasZ *= normFactor;
    }

    Reference::Reference(
            uint32_t width,
            uint32_t height,
            const AppConfig& appConfig,
            const HpmScene& scene,
            VkQueue queue)
            :
            m_Width(width),
            m_Height(height),
            m_CmdPool(VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, VulkanAPI::GetGraphicsQFI()),
            m_CmpShader("ref/cmp.comp", false),
            m_ResultStagingBuffer(
                    sizeof(Reference::Result),
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    {}),
            m_ResultBuffer(
                    sizeof(Reference::Result),
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    {})
    {
        m_CmdPool.AllocateBuffers(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        m_CmdBuf = m_CmdPool.GetBuffer(0);

        CreateDescriptor();

        InitSpecInfo();
        CreatePipelineLayout();
        CreateCmpPipeline();

        CreateRefCameras();
        CreateRefImages(queue);
        GenRefImages(appConfig, scene, queue);
    }

    std::array<Reference::Result, 6> Reference::CompareNrc(NrcHpmRenderer& renderer, const Camera* oldCamera, VkQueue queue)
    {
        std::array<Result, 6> results = {};

        for (size_t i = 0; i < m_RefCameras.size(); i++)
        {
            // Render on noisy renderer
            renderer.SetCamera(queue, m_RefCameras[i]);
            renderer.Render(queue, false);
            ASSERT_VULKAN(vkQueueWaitIdle(queue));

            // Update
            UpdateDescriptor(m_RefImageViews[i], renderer.GetImageView());
            RecordCmpCmdBuf();

            // Submit comparision
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.pNext = nullptr;
            submitInfo.waitSemaphoreCount = 0;
            submitInfo.pWaitSemaphores = nullptr;
            submitInfo.pWaitDstStageMask = nullptr;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_CmdBuf;
            submitInfo.signalSemaphoreCount = 0;
            submitInfo.pSignalSemaphores = nullptr;
            ASSERT_VULKAN(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
            ASSERT_VULKAN(vkQueueWaitIdle(queue));

            // Sync result buffer to host
            vk::Buffer::Copy(&m_ResultBuffer, &m_ResultStagingBuffer, sizeof(Result));
            m_ResultStagingBuffer.GetData(sizeof(Result), &results[i], 0, 0);
            results[i].Norm(m_Width, m_Height);

            // Eval results
            Log::Info(
                    "MSE: " + std::to_string(results[i].mse) +
                    " | Bias: (" + std::to_string(results[i].biasX) +
                            ", " + std::to_string(results[i].biasY) +
                            ", " + std::to_string(results[i].biasZ) +
                            ")");
        }

        renderer.SetCamera(queue, oldCamera);

        return results;
    }

    std::array<Reference::Result, 6> Reference::CompareMc(McHpmRenderer& renderer, const Camera* oldCamera, VkQueue queue)
    {
        std::array<Result, 6> results = {};

        for (size_t i = 0; i < m_RefCameras.size(); i++)
        {
            // Render on noisy renderer
            renderer.SetCamera(queue, m_RefCameras[i]);
            renderer.Render(queue);
            ASSERT_VULKAN(vkQueueWaitIdle(queue));

            // Update
            UpdateDescriptor(m_RefImageViews[i], renderer.GetImageView());
            RecordCmpCmdBuf();

            // Submit comparision
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.pNext = nullptr;
            submitInfo.waitSemaphoreCount = 0;
            submitInfo.pWaitSemaphores = nullptr;
            submitInfo.pWaitDstStageMask = nullptr;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_CmdBuf;
            submitInfo.signalSemaphoreCount = 0;
            submitInfo.pSignalSemaphores = nullptr;
            ASSERT_VULKAN(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
            ASSERT_VULKAN(vkQueueWaitIdle(queue));

            // Sync result buffer to host
            vk::Buffer::Copy(&m_ResultBuffer, &m_ResultStagingBuffer, sizeof(Result));
            m_ResultStagingBuffer.GetData(sizeof(Result), &results[i], 0, 0);
            results[i].Norm(m_Width, m_Height);

            // Eval results
            Log::Info(
                    "MSE: " + std::to_string(results[i].mse) +
                    " | Bias: (" + std::to_string(results[i].biasX) +
                    ", " + std::to_string(results[i].biasY) +
                    ", " + std::to_string(results[i].biasZ) +
                    ")");
        }

        renderer.SetCamera(queue, oldCamera);

        return results;
    }

    void Reference::Destroy()
    {
        VkDevice device = VulkanAPI::GetDevice();

        for (size_t i = 0; i < m_RefCameras.size(); i++)
        {
            m_RefCameras[i]->Destroy();
            delete m_RefCameras[i];

            vkDestroyImageView(device, m_RefImageViews[i], nullptr);
            vkFreeMemory(device, m_RefImageMemories[i], nullptr);
            vkDestroyImage(device, m_RefImages[i], nullptr);
        }

        vkDestroyPipeline(device, m_CmpPipeline, nullptr);
        m_CmpShader.Destroy();
        vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);

        vkDestroyDescriptorPool(device, m_DescPool, nullptr);
        vkDestroyDescriptorSetLayout(device, m_DescSetLayout, nullptr);

        m_CmdPool.Destroy();

        m_ResultBuffer.Destroy();
        m_ResultStagingBuffer.Destroy();
    }

    void Reference::CreateDescriptor()
    {
        VkDevice device = VulkanAPI::GetDevice();

        // Create desc set layout
        uint32_t binding = 0;

        VkDescriptorSetLayoutBinding refImageBinding = {};
        refImageBinding.binding = binding++;
        refImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        refImageBinding.descriptorCount = 1;
        refImageBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        refImageBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutBinding cmdImageBinding = {};
        cmdImageBinding.binding = binding++;
        cmdImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        cmdImageBinding.descriptorCount = 1;
        cmdImageBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        cmdImageBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutBinding resultBufferBinding = {};
        resultBufferBinding.binding = binding++;
        resultBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        resultBufferBinding.descriptorCount = 1;
        resultBufferBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        resultBufferBinding.pImmutableSamplers = nullptr;

        std::vector<VkDescriptorSetLayoutBinding> bindings = { refImageBinding, cmdImageBinding, resultBufferBinding };

        VkDescriptorSetLayoutCreateInfo layoutCI = {};
        layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCI.pNext = nullptr;
        layoutCI.flags = 0;
        layoutCI.bindingCount = bindings.size();
        layoutCI.pBindings = bindings.data();
        ASSERT_VULKAN(vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_DescSetLayout));

        // Create desc pool
        VkDescriptorPoolSize storageImagePS = {};
        storageImagePS.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        storageImagePS.descriptorCount = 2;

        VkDescriptorPoolSize storageBufferPS = {};
        storageBufferPS.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        storageBufferPS.descriptorCount = 1;

        std::vector<VkDescriptorPoolSize> poolSizes = { storageImagePS, storageBufferPS };

        VkDescriptorPoolCreateInfo poolCI = {};
        poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.pNext = nullptr;
        poolCI.flags = 0;
        poolCI.maxSets = 1;
        poolCI.poolSizeCount = poolSizes.size();
        poolCI.pPoolSizes = poolSizes.data();
        ASSERT_VULKAN(vkCreateDescriptorPool(device, &poolCI, nullptr, &m_DescPool));

        // Allocate desc set
        VkDescriptorSetAllocateInfo descSetAI = {};
        descSetAI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descSetAI.pNext = nullptr;
        descSetAI.descriptorPool = m_DescPool;
        descSetAI.descriptorSetCount = 1;
        descSetAI.pSetLayouts = &m_DescSetLayout;
        ASSERT_VULKAN(vkAllocateDescriptorSets(device, &descSetAI, &m_DescSet));
    }

    void Reference::UpdateDescriptor(VkImageView refImageView, VkImageView cmpImageView)
    {
        uint32_t binding = 0;

        VkDescriptorImageInfo refImageInfo = {};
        refImageInfo.sampler = VK_NULL_HANDLE;
        refImageInfo.imageView = refImageView;
        refImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet refImageWrite = {};
        refImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        refImageWrite.pNext = nullptr;
        refImageWrite.dstSet = m_DescSet;
        refImageWrite.dstBinding = binding++;
        refImageWrite.dstArrayElement = 0;
        refImageWrite.descriptorCount = 1;
        refImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        refImageWrite.pImageInfo = &refImageInfo;
        refImageWrite.pBufferInfo = nullptr;
        refImageWrite.pTexelBufferView = nullptr;

        VkDescriptorImageInfo cmpImageInfo = {};
        cmpImageInfo.sampler = VK_NULL_HANDLE;
        cmpImageInfo.imageView = cmpImageView;
        cmpImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet cmpImageWrite = {};
        cmpImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cmpImageWrite.pNext = nullptr;
        cmpImageWrite.dstSet = m_DescSet;
        cmpImageWrite.dstBinding = binding++;
        cmpImageWrite.dstArrayElement = 0;
        cmpImageWrite.descriptorCount = 1;
        cmpImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        cmpImageWrite.pImageInfo = &cmpImageInfo;
        cmpImageWrite.pBufferInfo = nullptr;
        cmpImageWrite.pTexelBufferView = nullptr;

        VkDescriptorBufferInfo resultBufferInfo = {};
        resultBufferInfo.buffer = m_ResultBuffer.GetVulkanHandle();
        resultBufferInfo.offset = 0;
        resultBufferInfo.range = sizeof(Result);

        VkWriteDescriptorSet resultBufferWrite = {};
        resultBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        resultBufferWrite.pNext = nullptr;
        resultBufferWrite.dstSet = m_DescSet;
        resultBufferWrite.dstBinding = binding++;
        resultBufferWrite.dstArrayElement = 0;
        resultBufferWrite.descriptorCount = 1;
        resultBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        resultBufferWrite.pImageInfo = nullptr;
        resultBufferWrite.pBufferInfo = &resultBufferInfo;
        resultBufferWrite.pTexelBufferView = nullptr;

        std::vector<VkWriteDescriptorSet> writes = { refImageWrite, cmpImageWrite, resultBufferWrite };
        vkUpdateDescriptorSets(VulkanAPI::GetDevice(), writes.size(), writes.data(), 0, nullptr);
    }

    void Reference::InitSpecInfo()
    {
        m_SpecData.width = m_Width;
        m_SpecData.height = m_Height;

        uint32_t constantID = 0;

        VkSpecializationMapEntry widthEntry = {};
        widthEntry.constantID = constantID++;
        widthEntry.offset = offsetof(SpecializationData, SpecializationData::width);
        widthEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry heightEntry = {};
        heightEntry.constantID = constantID++;
        heightEntry.offset = offsetof(SpecializationData, SpecializationData::height);
        heightEntry.size = sizeof(uint32_t);

        m_SpecEntries = { widthEntry, heightEntry };

        m_SpecInfo.mapEntryCount = m_SpecEntries.size();
        m_SpecInfo.pMapEntries = m_SpecEntries.data();
        m_SpecInfo.dataSize = sizeof(SpecializationData);
        m_SpecInfo.pData = &m_SpecData;
    }

    void Reference::CreatePipelineLayout()
    {
        VkPipelineLayoutCreateInfo layoutCI = {};
        layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutCI.pNext = nullptr;
        layoutCI.flags = 0;
        layoutCI.setLayoutCount = 1;
        layoutCI.pSetLayouts = &m_DescSetLayout;
        layoutCI.pushConstantRangeCount = 0;
        layoutCI.pPushConstantRanges = nullptr;
        ASSERT_VULKAN(vkCreatePipelineLayout(VulkanAPI::GetDevice(), &layoutCI, nullptr, &m_PipelineLayout));
    }

    void Reference::CreateCmpPipeline()
    {
        VkPipelineShaderStageCreateInfo stageCI = {};
        stageCI.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageCI.pNext = nullptr;
        stageCI.flags = 0;
        stageCI.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageCI.module = m_CmpShader.GetVulkanModule();
        stageCI.pName = "main";
        stageCI.pSpecializationInfo = &m_SpecInfo;

        VkComputePipelineCreateInfo pipelineCI = {};
        pipelineCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineCI.pNext = nullptr;
        pipelineCI.flags = 0;
        pipelineCI.stage = stageCI;
        pipelineCI.layout = m_PipelineLayout;
        pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
        pipelineCI.basePipelineIndex = 0;
        ASSERT_VULKAN(vkCreateComputePipelines(VulkanAPI::GetDevice(), VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &m_CmpPipeline));
    }

    void Reference::RecordCmpCmdBuf()
    {
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext = nullptr;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;
        ASSERT_VULKAN(vkBeginCommandBuffer(m_CmdBuf, &beginInfo));

        vkCmdFillBuffer(m_CmdBuf, m_ResultBuffer.GetVulkanHandle(), 0, sizeof(Result), 0);

        vkCmdBindDescriptorSets(m_CmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescSet, 0, nullptr);
        vkCmdBindPipeline(m_CmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_CmpPipeline);
        vkCmdDispatch(m_CmdBuf, m_Width / 32, m_Height, 1);

        ASSERT_VULKAN(vkEndCommandBuffer(m_CmdBuf));
    }

    void Reference::CreateRefCameras()
    {
        const float aspectRatio = static_cast<float>(m_Width) / static_cast<float>(m_Height);

        m_RefCameras = {
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
    }

    void Reference::CreateRefImages(VkQueue queue)
    {
        VkDevice device = VulkanAPI::GetDevice();

        for (size_t i = 0; i < m_RefImages.size(); i++)
        {
            VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;

            // Create Image
            VkImageCreateInfo imageCI;
            imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageCI.pNext = nullptr;
            imageCI.flags = 0;
            imageCI.imageType = VK_IMAGE_TYPE_2D;
            imageCI.format = format;
            imageCI.extent = { m_Width, m_Height, 1 };
            imageCI.mipLevels = 1;
            imageCI.arrayLayers = 1;
            imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
            imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageCI.queueFamilyIndexCount = 0;
            imageCI.pQueueFamilyIndices = nullptr;
            imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkResult result = vkCreateImage(device, &imageCI, nullptr, &m_RefImages[i]);
            ASSERT_VULKAN(result);

            // Image Memory
            VkMemoryRequirements memoryRequirements;
            vkGetImageMemoryRequirements(device, m_RefImages[i], &memoryRequirements);

            VkMemoryAllocateInfo allocateInfo;
            allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocateInfo.pNext = nullptr;
            allocateInfo.allocationSize = memoryRequirements.size;
            allocateInfo.memoryTypeIndex = VulkanAPI::FindMemoryType(
                    memoryRequirements.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            result = vkAllocateMemory(device, &allocateInfo, nullptr, &m_RefImageMemories[i]);
            ASSERT_VULKAN(result);

            result = vkBindImageMemory(device, m_RefImages[i], m_RefImageMemories[i], 0);
            ASSERT_VULKAN(result);

            // Create image view
            VkImageViewCreateInfo imageViewCI;
            imageViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            imageViewCI.pNext = nullptr;
            imageViewCI.flags = 0;
            imageViewCI.image = m_RefImages[i];
            imageViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
            imageViewCI.format = format;
            imageViewCI.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            imageViewCI.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            imageViewCI.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            imageViewCI.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            imageViewCI.subresourceRange.baseMipLevel = 0;
            imageViewCI.subresourceRange.levelCount = 1;
            imageViewCI.subresourceRange.baseArrayLayer = 0;
            imageViewCI.subresourceRange.layerCount = 1;

            result = vkCreateImageView(device, &imageViewCI, nullptr, &m_RefImageViews[i]);
            ASSERT_VULKAN(result);

            // Change image layout
            VkCommandBufferBeginInfo beginInfo;
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.pNext = nullptr;
            beginInfo.flags = 0;
            beginInfo.pInheritanceInfo = nullptr;

            result = vkBeginCommandBuffer(m_CmdBuf, &beginInfo);
            ASSERT_VULKAN(result);

            vk::CommandRecorder::ImageLayoutTransfer(
                    m_CmdBuf,
                    m_RefImages[i],
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_NONE,
                    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

            result = vkEndCommandBuffer(m_CmdBuf);
            ASSERT_VULKAN(result);

            VkSubmitInfo submitInfo;
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.pNext = nullptr;
            submitInfo.waitSemaphoreCount = 0;
            submitInfo.pWaitSemaphores = nullptr;
            submitInfo.pWaitDstStageMask = nullptr;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_CmdBuf;
            submitInfo.signalSemaphoreCount = 0;
            submitInfo.pSignalSemaphores = nullptr;

            VkQueue queue = VulkanAPI::GetGraphicsQueue();
            result = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
            ASSERT_VULKAN(result);
            result = vkQueueWaitIdle(queue);
            ASSERT_VULKAN(result);
        }
    }

    void Reference::GenRefImages(const AppConfig& appConfig, const HpmScene& scene,	VkQueue queue)
    {
        const uint32_t sceneID = appConfig.scene.id;

        // Create output path if not exists
        std::string outputDirPath = "output/ " + appConfig.GetName() + "/";
        if (!std::filesystem::is_directory(outputDirPath) || !std::filesystem::exists(outputDirPath))
        {
            std::filesystem::create_directory(outputDirPath);
        }

        // Create reference folder if not exists
        std::string referenceDirPath = "reference/" + std::to_string(sceneID) + "/";
#if __cplusplus >= 201703L
        en::Log::Warn("C++ version lower then 17. Cant create reference data");
#else
        if (!std::filesystem::is_directory(referenceDirPath) || !std::filesystem::exists(referenceDirPath))
        {
            en::Log::Info("Reference folder for scene " + std::to_string(sceneID) + " was not found. Creating reference images");

            // Create reference renderer
            McHpmRenderer refRenderer(m_Width, m_Height, 32, true, m_RefCameras[0], scene);

            // Create folder
            std::filesystem::create_directory(referenceDirPath);

            // Generate reference data
            for (size_t i = 0; i < m_RefCameras.size(); i++)
            {
                en::Log::Info("Generating reference image " + std::to_string(i));

                // Set new camera
                refRenderer.SetCamera(queue, m_RefCameras[i]);

                // Generate reference image
                for (size_t frame = 0; frame < 8192; frame++)
                {
                    refRenderer.Render(queue);
                    ASSERT_VULKAN(vkQueueWaitIdle(queue));
                }

                // Export reference image
                refRenderer.ExportOutputImageToFile(queue, referenceDirPath + std::to_string(i) + ".exr");

                // Copy to ref image for faster comparison
                CopyToRefImage(i, refRenderer.GetImage(), queue);
            }

            refRenderer.Destroy();
        }
#endif
    }

    void Reference::CopyToRefImage(uint32_t imageIdx, VkImage srcImage, VkQueue queue)
    {
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext = nullptr;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;
        ASSERT_VULKAN(vkBeginCommandBuffer(m_CmdBuf, &beginInfo));

        VkImageCopy imageCopy = {};
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
        imageCopy.extent = { m_Width, m_Height, 1 };
        vkCmdCopyImage(
                m_CmdBuf,
                srcImage,
                VK_IMAGE_LAYOUT_GENERAL,
                m_RefImages[imageIdx],
                VK_IMAGE_LAYOUT_GENERAL,
                1,
                &imageCopy);

        ASSERT_VULKAN(vkEndCommandBuffer(m_CmdBuf));

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = nullptr;
        submitInfo.waitSemaphoreCount = 0;
        submitInfo.pWaitSemaphores = nullptr;
        submitInfo.pWaitDstStageMask = nullptr;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_CmdBuf;
        submitInfo.signalSemaphoreCount = 0;
        submitInfo.pSignalSemaphores = nullptr;

        ASSERT_VULKAN(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
        ASSERT_VULKAN(vkQueueWaitIdle(queue));
    }
}
