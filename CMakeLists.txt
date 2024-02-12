cmake_minimum_required(VERSION 3.8)

# Get VCPKG
if (DEFINED ENV{VCPKG_CMAKE_INCLUDE})
    include($ENV{VCPKG_CMAKE_INCLUDE})
endif()

# Project
project(NeuralRadianceCaching CXX CUDA)

#set(CMAKE_CXX_STANDARD 20)
if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
endif()

file(GLOB_RECURSE PROJECT_INCLUDE "include/*.hpp")
file(GLOB_RECURSE PROJECT_SOURCE "src/*.cpp")
file(GLOB_RECURSE PROJECT_CUDA_SOURCE "src/*.cu")

# Add executable and specify CUDA as language
add_executable(${PROJECT_NAME} ${PROJECT_INCLUDE} ${PROJECT_SOURCE} ${PROJECT_CUDA_SOURCE})

# Include CUDA headers
include_directories(${CUDA_INCLUDE_DIRS})

include_directories("C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v11.8/include")

# Set CUDA architectures
set(CMAKE_CUDA_ARCHITECTURES 86)
set_target_properties(${PROJECT_NAME} PROPERTIES CUDA_ARCHITECTURES "86")

# Link against CUDA libraries
target_link_libraries(${PROJECT_NAME} PRIVATE ${CUDA_LIBRARIES})

#target_include_directories(${PROJECT_NAME} PUBLIC "include" ${CUDA_INC_PATH})
# Add the path to CUDA include directory
target_include_directories(${PROJECT_NAME} PUBLIC "include" "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v11.8/include")

# Compile
list(APPEND CUDA_NVCC_FLAGS "--extended-lambda")
list(APPEND CUDA_NVCC_FLAGS "--expt-relaxed-constexpr")

#set(CMAKE_CUDA_ARCHITECTURES 86)
#set_target_properties(${PROJECT_NAME} PROPERTIES CUDA_ARCHITECTURES "86")
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
target_compile_features(${PROJECT_NAME} PUBLIC cuda_std_17)
target_compile_options(${PROJECT_NAME} PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:${CUDA_NVCC_FLAGS}>)

# Dependencies

# Vulkan
find_package(Vulkan REQUIRED)
target_include_directories(${PROJECT_NAME} PUBLIC ${Vulkan_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)

# GLFW
find_package(glfw3 CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE glfw)

# GLM
find_package(glm CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE glm::glm)

# STB
find_package(Stb REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE ${Stb_INCLUDE_DIR})

# IMGUI
find_package(imgui CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui)

# TCNN
add_compile_definitions(TCNN_MIN_GPU_ARCH=86)

add_subdirectory("dependencies/tiny-cuda-nn")
target_include_directories(${PROJECT_NAME} PRIVATE "dependencies/tiny-cuda-nn/include" "dependencies/tiny-cuda-nn/dependencies")
target_link_libraries(${PROJECT_NAME} PUBLIC ${CUDA_LIBRARIES} cuda cublas tiny-cuda-nn)
