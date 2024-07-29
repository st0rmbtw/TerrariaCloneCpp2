#ifndef TERRARIA_VULKAN_SHADER_COMPILER
#define TERRARIA_VULKAN_SHADER_COMPILER

#pragma once

#include <LLGL/ShaderFlags.h>

bool CompileVulkanShader(LLGL::ShaderType shaderType, const char* shaderSource, size_t shaderSourceSize, const char* outputPath);

#endif