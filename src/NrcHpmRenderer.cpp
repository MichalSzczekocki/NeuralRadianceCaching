#include <engine/graphics/renderer/NrcHpmRenderer.hpp>
#include <engine/util/Log.hpp>
#include <engine/graphics/vulkan/CommandRecorder.hpp>

namespace en
{
    NrcHpmRenderer::NrcHpmRenderer(
            uint32_t width,
            uint32_t height,
            uint32_t trainWidth,
            uint32_t trainHeight,
            const Camera& camera,
            const VolumeData& volumeData,
            const DirLight& dirLight,
            const PointLight& pointLight,
            const HdrEnvMap& hdrEnvMap,
            const NeuralRadianceCache& nrc)
            :
            m_FrameWidth(width),
            m_FrameHeight(height),
            m_TrainWidth(trainWidth),
            m_TrainHeight(trainHeight),
            m_GenRaysShader("nrc/gen_rays.comp", false),
            m_RenderShader("nrc/render.comp", false),
            m_CommandPool(0, VulkanAPI::GetGraphicsQFI()),
            m_Camera(camera),
            m_VolumeData(volumeData),
            m_DirLight(dirLight),
            m_PointLight(pointLight),
            m_HdrEnvMap(hdrEnvMap),
            m_Nrc(nrc)
    {
        VkDevice device = VulkanAPI::GetDevice();

        CreatePipelineLayout(device);

        InitSpecializationConstants();

        CreateGenRaysPipeline(device);
        CreateRenderPipeline(device);

        CreateOutputImage(device);

        m_CommandPool.AllocateBuffers(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        m_CommandBuffer = m_CommandPool.GetBuffer(0);

        RecordCommandBuffer();
    }

    void NrcHpmRenderer::Render(VkQueue queue) const
    {
        VkSubmitInfo submitInfo;
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = nullptr;
        submitInfo.waitSemaphoreCount = 0;
        submitInfo.pWaitSemaphores = nullptr;
        submitInfo.pWaitDstStageMask = nullptr;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_CommandBuffer;
        submitInfo.signalSemaphoreCount = 0;
        submitInfo.pSignalSemaphores = nullptr;

        VkResult result = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        ASSERT_VULKAN(result);
    }

    void NrcHpmRenderer::Destroy()
    {
        VkDevice device = VulkanAPI::GetDevice();

        m_CommandPool.Destroy();
        vkDestroyFramebuffer(device, m_Framebuffer, nullptr);

        vkDestroyImageView(device, m_OutputImageView, nullptr);
        vkFreeMemory(device, m_OutputImageMemory, nullptr);
        vkDestroyImage(device, m_OutputImage, nullptr);

        vkDestroyPipeline(device, m_RenderPipeline, nullptr);
        m_RenderShader.Destroy();

        vkDestroyPipeline(device, m_GenRaysPipeline, nullptr);
        m_GenRaysShader.Destroy();

        vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
    }

//	void NrcHpmRenderer::ResizeFrame(uint32_t width, uint32_t height)
//	{
//		m_FrameWidth = width;
//		m_FrameHeight = height;
//
//		VkDevice device = VulkanAPI::GetDevice();
//
//		// Destroy
//		m_CommandPool.FreeBuffers();
//		vkDestroyFramebuffer(device, m_Framebuffer, nullptr);
//
//		vkDestroyImageView(device, m_ColorImageView, nullptr);
//		vkFreeMemory(device, m_ColorImageMemory, nullptr);
//		vkDestroyImage(device, m_ColorImage, nullptr);
//
//		// Create
//		CreateColorImage(device);
//		CreateFramebuffer(device);
//
//		m_CommandPool.AllocateBuffers(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
//		m_CommandBuffer = m_CommandPool.GetBuffer(0);
//
//		RecordCommandBuffer();
//	}

    VkImage NrcHpmRenderer::GetImage() const
    {
        return m_OutputImage;
    }

    VkImageView NrcHpmRenderer::GetImageView() const
    {
        return m_OutputImageView;
    }

    void NrcHpmRenderer::CreatePipelineLayout(VkDevice device)
    {
        std::vector<VkDescriptorSetLayout> layouts = {
                Camera::GetDescriptorSetLayout(),
                VolumeData::GetDescriptorSetLayout(),
                DirLight::GetDescriptorSetLayout(),
                NeuralRadianceCache::GetDescriptorSetLayout(),
                PointLight::GetDescriptorSetLayout(),
                HdrEnvMap::GetDescriptorSetLayout() };

        VkPipelineLayoutCreateInfo layoutCreateInfo;
        layoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutCreateInfo.pNext = nullptr;
        layoutCreateInfo.flags = 0;
        layoutCreateInfo.setLayoutCount = layouts.size();
        layoutCreateInfo.pSetLayouts = layouts.data();
        layoutCreateInfo.pushConstantRangeCount = 0;
        layoutCreateInfo.pPushConstantRanges = nullptr;

        VkResult result = vkCreatePipelineLayout(device, &layoutCreateInfo, nullptr, &m_PipelineLayout);
        ASSERT_VULKAN(result);
    }

    void NrcHpmRenderer::InitSpecializationConstants()
    {
        // Fill struct
        m_SpecData.renderWidth = m_FrameWidth;
        m_SpecData.renderHeight = m_FrameHeight;

        m_SpecData.trainWidth = m_TrainWidth;
        m_SpecData.trainHeight = m_TrainHeight;

        m_SpecData.posEncoding = m_Nrc.GetPosEncoding();
        m_SpecData.dirEncoding = m_Nrc.GetDirEncoding();

        m_SpecData.posFreqCount = m_Nrc.GetPosFreqCount();
        m_SpecData.posMinFreq = m_Nrc.GetPosMinFreq();
        m_SpecData.posMaxFreq = m_Nrc.GetPosMaxFreq();
        m_SpecData.posLevelCount = m_Nrc.GetPosLevelCount();
        m_SpecData.posHashTableSize = m_Nrc.GetPosHashTableSize();
        m_SpecData.posFeatureCount = m_Nrc.GetPosFeatureCount();

        m_SpecData.dirFreqCount = m_Nrc.GetDirFreqCount();
        m_SpecData.dirFeatureCount = m_Nrc.GetDirFeatureCount();

        m_SpecData.layerCount = m_Nrc.GetLayerCount();
        m_SpecData.layerWidth = m_Nrc.GetLayerWidth();
        m_SpecData.inputFeatureCount = m_Nrc.GetDirFeatureCount();
        m_SpecData.nrcLearningRate = m_Nrc.GetNrcLearningRate();
        m_SpecData.mrheLearningRate = m_Nrc.GetMrheLearningRate();

        // Init map entries
        // Render size
        VkSpecializationMapEntry renderWidthEntry;
        renderWidthEntry.constantID = 0;
        renderWidthEntry.offset = offsetof(SpecializationData, SpecializationData::renderWidth);
        renderWidthEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry renderHeightEntry;
        renderHeightEntry.constantID = 1;
        renderHeightEntry.offset = offsetof(SpecializationData, SpecializationData::renderHeight);
        renderHeightEntry.size = sizeof(uint32_t);

        // Train size
        VkSpecializationMapEntry trainWidthEntry;
        trainWidthEntry.constantID = 2;
        trainWidthEntry.offset = offsetof(SpecializationData, SpecializationData::trainWidth);
        trainWidthEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry trainHeightEntry;
        trainHeightEntry.constantID = 3;
        trainHeightEntry.offset = offsetof(SpecializationData, SpecializationData::trainHeight);
        trainHeightEntry.size = sizeof(uint32_t);

        // Encoding
        VkSpecializationMapEntry posEncodingEntry;
        posEncodingEntry.constantID = 4;
        posEncodingEntry.offset = offsetof(SpecializationData, SpecializationData::posEncoding);
        posEncodingEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry dirEncodingEntry;
        dirEncodingEntry.constantID = 5;
        dirEncodingEntry.offset = offsetof(SpecializationData, SpecializationData::dirEncoding);
        dirEncodingEntry.size = sizeof(uint32_t);

        // Pos parameters
        VkSpecializationMapEntry posFreqCountEntry;
        posFreqCountEntry.constantID = 6;
        posFreqCountEntry.offset = offsetof(SpecializationData, SpecializationData::posFreqCount);
        posFreqCountEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry posMinFreqEntry;
        posMinFreqEntry.constantID = 7;
        posMinFreqEntry.offset = offsetof(SpecializationData, SpecializationData::posMinFreq);
        posMinFreqEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry posMaxFreqEntry;
        posMaxFreqEntry.constantID = 8;
        posMaxFreqEntry.offset = offsetof(SpecializationData, SpecializationData::posMaxFreq);
        posMaxFreqEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry posLevelCountEntry;
        posLevelCountEntry.constantID = 9;
        posLevelCountEntry.offset = offsetof(SpecializationData, SpecializationData::posLevelCount);
        posLevelCountEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry posHashTableSizeEntry;
        posHashTableSizeEntry.constantID = 10;
        posHashTableSizeEntry.offset = offsetof(SpecializationData, SpecializationData::posHashTableSize);
        posHashTableSizeEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry posFeatureCountEntry;
        posFeatureCountEntry.constantID = 11;
        posFeatureCountEntry.offset = offsetof(SpecializationData, SpecializationData::posFeatureCount);
        posFeatureCountEntry.size = sizeof(uint32_t);

        // Dir parameters
        VkSpecializationMapEntry dirFreqCountEntry;
        dirFreqCountEntry.constantID = 12;
        dirFreqCountEntry.offset = offsetof(SpecializationData, SpecializationData::dirFreqCount);
        dirFreqCountEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry dirFeatureCountEntry;
        dirFeatureCountEntry.constantID = 13;
        dirFeatureCountEntry.offset = offsetof(SpecializationData, SpecializationData::dirFeatureCount);
        dirFeatureCountEntry.size = sizeof(uint32_t);

        // Nn
        VkSpecializationMapEntry layerCountEntry;
        layerCountEntry.constantID = 14;
        layerCountEntry.offset = offsetof(SpecializationData, SpecializationData::layerCount);
        layerCountEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry layerWidthEntry;
        layerWidthEntry.constantID = 15;
        layerWidthEntry.offset = offsetof(SpecializationData, SpecializationData::layerWidth);
        layerWidthEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry inputFeatureCountEntry;
        inputFeatureCountEntry.constantID = 16;
        inputFeatureCountEntry.offset = offsetof(SpecializationData, SpecializationData::inputFeatureCount);
        inputFeatureCountEntry.size = sizeof(uint32_t);

        VkSpecializationMapEntry nrcLearningRateEntry;
        nrcLearningRateEntry.constantID = 17;
        nrcLearningRateEntry.offset = offsetof(SpecializationData, SpecializationData::nrcLearningRate);
        nrcLearningRateEntry.size = sizeof(float);

        VkSpecializationMapEntry mrheLearningRateEntry;
        mrheLearningRateEntry.constantID = 18;
        mrheLearningRateEntry.offset = offsetof(SpecializationData, SpecializationData::mrheLearningRate);
        mrheLearningRateEntry.size = sizeof(float);

        m_SpecMapEntries = {
                renderWidthEntry,
                renderHeightEntry,

                trainWidthEntry,
                trainHeightEntry,

                posEncodingEntry,
                dirEncodingEntry,

                posFreqCountEntry,
                posMinFreqEntry,
                posMaxFreqEntry,
                posLevelCountEntry,
                posHashTableSizeEntry,
                posFeatureCountEntry,

                dirFreqCountEntry,
                dirFeatureCountEntry,

                layerCountEntry,
                layerWidthEntry,
                inputFeatureCountEntry,
                nrcLearningRateEntry,
                mrheLearningRateEntry };

        VkSpecializationInfo specInfo;
        specInfo.mapEntryCount = m_SpecMapEntries.size();
        specInfo.pMapEntries = m_SpecMapEntries.data();
        specInfo.dataSize = sizeof(SpecializationData);
        specInfo.pData = &m_SpecData;
    }

    void NrcHpmRenderer::CreateGenRaysPipeline(VkDevice device)
    {
        VkPipelineShaderStageCreateInfo shaderStage;
        shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStage.pNext = nullptr;
        shaderStage.flags = 0;
        shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shaderStage.module = m_GenRaysShader.GetVulkanModule();
        shaderStage.pName = "main";
        shaderStage.pSpecializationInfo = &m_SpecInfo;

        VkComputePipelineCreateInfo pipelineCI;
        pipelineCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineCI.pNext = nullptr;
        pipelineCI.flags = 0;
        pipelineCI.stage = shaderStage;
        pipelineCI.layout = m_PipelineLayout;
        pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
        pipelineCI.basePipelineIndex = 0;

        VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &m_GenRaysPipeline);
        ASSERT_VULKAN(result);
    }

    void NrcHpmRenderer::CreateRenderPipeline(VkDevice device)
    {
        VkPipelineShaderStageCreateInfo shaderStage;
        shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStage.pNext = nullptr;
        shaderStage.flags = 0;
        shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shaderStage.module = m_RenderShader.GetVulkanModule();
        shaderStage.pName = "main";
        shaderStage.pSpecializationInfo = &m_SpecInfo;

        VkComputePipelineCreateInfo pipelineCI;
        pipelineCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineCI.pNext = nullptr;
        pipelineCI.flags = 0;
        pipelineCI.stage = shaderStage;
        pipelineCI.layout = m_PipelineLayout;
        pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
        pipelineCI.basePipelineIndex = 0;

        VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &m_RenderPipeline);
        ASSERT_VULKAN(result);
    }

    void NrcHpmRenderer::CreateOutputImage(VkDevice device)
    {
        VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;

        // Create Image
        VkImageCreateInfo imageCI;
        imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCI.pNext = nullptr;
        imageCI.flags = 0;
        imageCI.imageType = VK_IMAGE_TYPE_2D;
        imageCI.format = format;
        imageCI.extent = { m_FrameWidth, m_FrameHeight, 1 };
        imageCI.mipLevels = 1;
        imageCI.arrayLayers = 1;
        imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCI.queueFamilyIndexCount = 0;
        imageCI.pQueueFamilyIndices = nullptr;
        imageCI.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

        VkResult result = vkCreateImage(device, &imageCI, nullptr, &m_OutputImage);
        ASSERT_VULKAN(result);

        // Change image layout
        VkCommandBufferBeginInfo beginInfo;
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext = nullptr;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;

        result = vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
        ASSERT_VULKAN(result);

        vk::CommandRecorder::ImageLayoutTransfer(
                m_CommandBuffer,
                m_OutputImage,
                VK_IMAGE_LAYOUT_PREINITIALIZED,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_NONE,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_NONE,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        result = vkEndCommandBuffer(m_CommandBuffer);
        ASSERT_VULKAN(result);

        VkSubmitInfo submitInfo;
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = nullptr;
        submitInfo.waitSemaphoreCount = 0;
        submitInfo.pWaitSemaphores = nullptr;
        submitInfo.pWaitDstStageMask = nullptr;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_CommandBuffer;
        submitInfo.signalSemaphoreCount = 0;
        submitInfo.pSignalSemaphores = nullptr;

        VkQueue queue = VulkanAPI::GetGraphicsQueue();
        result = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        ASSERT_VULKAN(result);
        result = vkQueueWaitIdle(queue);
        ASSERT_VULKAN(result);

        // Image Memory
        VkMemoryRequirements memoryRequirements;
        vkGetImageMemoryRequirements(device, m_OutputImage, &memoryRequirements);

        VkMemoryAllocateInfo allocateInfo;
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.pNext = nullptr;
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex = VulkanAPI::FindMemoryType(
                memoryRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        result = vkAllocateMemory(device, &allocateInfo, nullptr, &m_OutputImageMemory);
        ASSERT_VULKAN(result);

        result = vkBindImageMemory(device, m_OutputImage, m_OutputImageMemory, 0);
        ASSERT_VULKAN(result);

        // Create image view
        VkImageViewCreateInfo imageViewCI;
        imageViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewCI.pNext = nullptr;
        imageViewCI.flags = 0;
        imageViewCI.image = m_OutputImage;
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

        result = vkCreateImageView(device, &imageViewCI, nullptr, &m_OutputImageView);
        ASSERT_VULKAN(result);
    }

    void NrcHpmRenderer::RecordCommandBuffer()
    {
        // Begin
        VkCommandBufferBeginInfo beginInfo;
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext = nullptr;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;

        VkResult result = vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
        if (result != VK_SUCCESS)
            Log::Error("Failed to begin VkCommandBuffer", true);

        // Collect descriptor sets
        std::vector<VkDescriptorSet> descSets = {
                m_Camera.GetDescriptorSet(),
                m_VolumeData.GetDescriptorSet(),
                m_DirLight.GetDescriptorSet(),
                m_Nrc.GetDescriptorSet(),
                m_PointLight.GetDescriptorSet(),
                m_HdrEnvMap.GetDescriptorSet() };

        // Bind descriptor sets
        vkCmdBindDescriptorSets(
                m_CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout,
                0, descSets.size(), descSets.data(),
                0, nullptr);

        // Render pipeline
        vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_RenderPipeline);
        vkCmdDispatch(m_CommandBuffer, m_FrameWidth, m_FrameHeight, 1);

        // End
        result = vkEndCommandBuffer(m_CommandBuffer);
        if (result != VK_SUCCESS)
            Log::Error("Failed to end VkCommandBuffer", true);
    }
}
