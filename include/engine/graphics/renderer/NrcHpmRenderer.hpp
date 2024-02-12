#pragma once

#include <engine/graphics/Camera.hpp>
#include <engine/objects/VolumeData.hpp>
#include <engine/graphics/DirLight.hpp>
#include <engine/graphics/PointLight.hpp>
#include <engine/graphics/HdrEnvMap.hpp>
#include <engine/graphics/vulkan/Shader.hpp>
#include <engine/graphics/vulkan/CommandPool.hpp>
#include <cuda_runtime.h>

namespace en
{
    class NeuralRadianceCache;

    class NrcHpmRenderer
    {
    public:
        static void Init(VkDevice device);
        static void Shutdown(VkDevice device);

        NrcHpmRenderer(
                uint32_t width,
                uint32_t height,
                uint32_t trainWidth,
                uint32_t trainHeight,
                const Camera& camera,
                const VolumeData& volumeData,
                const DirLight& dirLight,
                const PointLight& pointLight,
                const HdrEnvMap& hdrEnvMap,
                NeuralRadianceCache& nrc);

        void Render(VkQueue queue) const;
        void Destroy();

        VkImage GetImage() const;
        VkImageView GetImageView() const;

    private:
        struct SpecializationData
        {
            uint32_t renderWidth;
            uint32_t renderHeight;

            uint32_t trainWidth;
            uint32_t trainHeight;
        };

        static VkDescriptorSetLayout m_DescSetLayout;
        static VkDescriptorPool m_DescPool;

        uint32_t m_RenderWidth;
        uint32_t m_RenderHeight;

        uint32_t m_TrainWidth;
        uint32_t m_TrainHeight;

        const Camera& m_Camera;
        const VolumeData& m_VolumeData;
        const DirLight& m_DirLight;
        const PointLight& m_PointLight;
        const HdrEnvMap& m_HdrEnvMap;
        NeuralRadianceCache& m_Nrc;

        VkSemaphore m_CudaStartSemaphore;
        cudaExternalSemaphore_t m_CuExtCudaStartSemaphore;

        VkSemaphore m_CudaFinishedSemaphore;
        cudaExternalSemaphore_t m_CuExtCudaFinishedSemaphore;

        VkDeviceSize m_NrcInferInputBufferSize;
        vk::Buffer* m_NrcInferInputBuffer = nullptr;
        cudaExternalMemory_t m_NrcInferInputCuExtMem;
        void* m_NrcInferInputDCuBuffer;

        VkDeviceSize m_NrcInferOutputBufferSize;
        vk::Buffer* m_NrcInferOutputBuffer = nullptr;
        cudaExternalMemory_t m_NrcInferOutputCuExtMem;
        void* m_NrcInferOutputDCuBuffer;

        VkDeviceSize m_NrcTrainInputBufferSize;
        vk::Buffer* m_NrcTrainInputBuffer = nullptr;
        cudaExternalMemory_t m_NrcTrainInputCuExtMem;
        void* m_NrcTrainInputDCuBuffer;

        VkDeviceSize m_NrcTrainTargetBufferSize;
        vk::Buffer* m_NrcTrainTargetBuffer = nullptr;
        cudaExternalMemory_t m_NrcTrainTargetCuExtMem;
        void* m_NrcTrainTargetDCuBuffer;

        VkPipelineLayout m_PipelineLayout;

        SpecializationData m_SpecData;
        std::vector<VkSpecializationMapEntry> m_SpecMapEntries;
        VkSpecializationInfo m_SpecInfo;

        vk::Shader m_GenRaysShader;
        VkPipeline m_GenRaysPipeline;

        vk::Shader m_PrepRayInfoShader;
        VkPipeline m_PrepRayInfoPipeline;

        vk::Shader m_PrepTrainRaysShader;
        VkPipeline m_PrepTrainRaysPipeline;

        vk::Shader m_RenderShader;
        VkPipeline m_RenderPipeline;

        VkImage m_OutputImage; // rgba32f output color
        VkDeviceMemory m_OutputImageMemory;
        VkImageView m_OutputImageView;

        VkImage m_PrimaryRayColorImage; // rgb32f primary ray output color + a32f transmittance
        VkDeviceMemory m_PrimaryRayColorImageMemory;
        VkImageView m_PrimaryRayColorImageView;

        VkImage m_PrimaryRayInfoImage;
        VkDeviceMemory m_PrimaryRayInfoImageMemory;
        VkImageView m_PrimaryRayInfoImageView;

        VkImage m_NrcRayOriginImage;
        VkDeviceMemory m_NrcRayOriginImageMemory;
        VkImageView m_NrcRayOriginImageView;

        VkImage m_NrcRayDirImage;
        VkDeviceMemory m_NrcRayDirImageMemory;
        VkImageView m_NrcRayDirImageView;

        VkDescriptorSet m_DescSet;

        vk::CommandPool m_CommandPool;
        VkCommandBuffer m_PreCudaCommandBuffer;
        VkCommandBuffer m_PostCudaCommandBuffer;

        void CreateSyncObjects(VkDevice device);

        void CreateNrcBuffers();

        void CreatePipelineLayout(VkDevice device);

        void InitSpecializationConstants();

        void CreateGenRaysPipeline(VkDevice device);
        void CreatePrepRayInfoPipeline(VkDevice device);
        void CreatePrepTrainRaysPipeline(VkDevice device);
        void CreateRenderPipeline(VkDevice device);

        void CreateOutputImage(VkDevice device);
        void CreatePrimaryRayColorImage(VkDevice device);
        void CreatePrimaryRayInfoImage(VkDevice device);
        void CreateNrcRayOriginImage(VkDevice device);
        void CreateNrcRayDirImage(VkDevice device);

        void AllocateAndUpdateDescriptorSet(VkDevice device);

        void RecordPreCudaCommandBuffer();
        void RecordPostCudaCommandBuffer();
    };
}