#include <engine/vulkan/Shader.hpp>
#include <engine/VulkanAPI.hpp>
#include <engine/Log.hpp>
#include <engine/read_file.hpp>

const std::string compilerPath = "glslc";
const std::string shaderDirPath = "data/shader/";

namespace en::vk
{
    Shader::Shader() {}
    Shader::Shader(const std::vector<char>& code)
    {
        Create(code);
    }

    Shader::Shader(const std::string& fileName, bool compiled)
    {
        std::string fullFilePath = shaderDirPath + fileName;

        std::string outputFileName = fullFilePath + ".spv";

        if (!compiled)
        {
            std::string command =
                    compilerPath + " " +
                    fullFilePath +
                    " -o " + outputFileName;;
            Log::Info("Shader Compile Command: " + command);

            // Compile
            if (std::system(command.c_str()) != 0)
                Log::Error("Failed to compile shader", true);
        }

        Create(ReadFileBinary(outputFileName));
    }

    void Shader::Destroy()
    {
        vkDestroyShaderModule(VulkanAPI::GetDevice(), m_VulkanModule, nullptr);
    }

    VkShaderModule Shader::GetVulkanModule() const
    {
        return m_VulkanModule;
    }

    void Shader::Create(const std::vector<char>& code)
    {
        VkShaderModuleCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.flags = 0;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkResult result = vkCreateShaderModule(VulkanAPI::GetDevice(), &createInfo, nullptr, &m_VulkanModule);
        ASSERT_VULKAN(result);
    }
}