#pragma once

#ifndef TERRARIA_RENDER_BATCH_HPP
#define TERRARIA_RENDER_BATCH_HPP

#include <glm/glm.hpp>
#include <vector>
#include <LLGL/LLGL.h>

#include "renderer/assets.hpp"
#include "math/rect.hpp"
#include "types/texture.hpp"

constexpr size_t MAX_QUADS = 5000;
constexpr size_t MAX_VERTICES = MAX_QUADS * 4;
constexpr size_t MAX_INDICES = MAX_QUADS * 6;

struct GlyphVertex {
    glm::vec4 transform_col_0;
    glm::vec4 transform_col_1;
    glm::vec4 transform_col_2;
    glm::vec4 transform_col_3;
    glm::vec3 color;
    // int texture_index;
    glm::vec2 size;
    glm::vec2 uv;
    float is_ui;
};

class RenderBatch {
public:
    virtual void init() = 0;
    virtual void render() = 0;
    virtual void update_buffers() = 0;
    virtual void set_buffers() = 0;

    virtual void begin() = 0;
    virtual void terminate() = 0;

    inline bool is_empty() { return m_index_count == 0; }
    inline bool is_full() { return m_index_count >= MAX_INDICES; }

    inline void set_projection_matrix(const glm::mat4& projection_matrix) {
        m_camera_projection = projection_matrix;
    }

    inline void set_screen_projection_matrix(const glm::mat4& projection_matrix) {
        m_screen_projection = projection_matrix;
    }

    inline void set_view_matrix(const glm::mat4& view_matrix) {
        m_camera_view = view_matrix;
    }

    inline void set_camera_frustum(const math::Rect& camera_frustum) {
        m_camera_frustum = camera_frustum;
    }

    inline void set_ui_frustum(const math::Rect& ui_frustum) {
        m_ui_frustum = ui_frustum;
    }

protected:
    std::vector<const Texture*> m_textures;
    glm::mat4 m_camera_projection = glm::mat4(1.0);
    glm::mat4 m_screen_projection = glm::mat4(1.0);
    glm::mat4 m_camera_view = glm::mat4(1.0);
    math::Rect m_camera_frustum;
    math::Rect m_ui_frustum;
    uint32_t m_texture_index = 0;
    size_t m_index_count = 0;
    LLGL::Buffer* m_vertex_buffer = nullptr;
    LLGL::Buffer* m_index_buffer = nullptr;
    LLGL::Buffer* m_constant_buffer = nullptr;
    LLGL::PipelineState* m_pipeline = nullptr;
    LLGL::ResourceHeap* m_resource_heap = nullptr;
};

class RenderBatchSprite : public RenderBatch {
public:
    void draw_sprite(const glm::mat4& transform, const glm::vec4& color, const glm::vec4& outline_color, float outline_thickness, const math::Rect& aabb, const glm::vec4& uv_offset_scale, const Texture* sprite_texture, bool ui);
    void init() override;
    void update_buffers() override;
    void render() override;
    void set_buffers() override;
    void begin() override;
    void terminate() override;
private:
    SpriteVertex* m_buffer = nullptr;
    SpriteVertex* m_buffer_ptr = nullptr;
};

class RenderBatchGlyph : public RenderBatch {
public:
    void draw_glyph(const glm::mat4& transform, const glm::vec3& color, /* const Texture& font_texture, */ const glm::vec2& uv, const glm::vec2& size, bool ui);

    void init() override;
    void render();
    void begin() override;
    void terminate() override;
private:
    GlyphVertex* m_buffer = nullptr;
    GlyphVertex* m_buffer_ptr = nullptr;
};

#endif
