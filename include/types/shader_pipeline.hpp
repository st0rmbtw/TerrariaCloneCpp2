#pragma once

#ifndef TERRARIA_TYPES_SHADER_PIPELINE
#define TERRARIA_TYPES_SHADER_PIPELINE

#include <LLGL/LLGL.h>

struct ShaderPipeline {
    LLGL::Shader* vs = nullptr; // Vertex shader
    LLGL::Shader* hs = nullptr; // Hull shader (aka. tessellation control shader)
    LLGL::Shader* ds = nullptr; // Domain shader (aka. tessellation evaluation shader)
    LLGL::Shader* gs = nullptr; // Geometry shader
    LLGL::Shader* ps = nullptr; // Pixel shader (aka. fragment shader)
    LLGL::Shader* cs = nullptr; // Compute shader
    
    void Unload(const LLGL::RenderSystemPtr& context) {
        if (vs != nullptr) context->Release(*vs);
        if (hs != nullptr) context->Release(*hs);
        if (ds != nullptr) context->Release(*ds);
        if (gs != nullptr) context->Release(*gs);
        if (ps != nullptr) context->Release(*ps);
        if (cs != nullptr) context->Release(*cs);
    }
};

#endif