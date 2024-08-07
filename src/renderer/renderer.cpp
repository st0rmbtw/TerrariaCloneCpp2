#include "renderer.hpp"

#include <memory>
#include <LLGL/Utils/TypeNames.h>
#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include "../assets.hpp"
#include "../log.hpp"

#include "LLGL/CommandBufferFlags.h"
#include "LLGL/PipelineStateFlags.h"
#include "utils.hpp"
#include "batch.hpp"
#include "world_renderer.hpp"
#include "background_renderer.hpp"

static struct RendererState {
    LLGL::RenderSystemPtr context = nullptr;
    LLGL::SwapChain* swap_chain = nullptr;
    LLGL::CommandBuffer* command_buffer = nullptr;
    LLGL::CommandQueue* command_queue = nullptr;
    std::shared_ptr<CustomSurface> surface = nullptr;
    LLGL::RenderingLimits limits;
#if DEBUG
    LLGL::RenderingDebugger* debugger = nullptr;
#endif

    LLGL::Buffer* constant_buffer = nullptr;
    
    RenderBatchSprite sprite_batch;
    RenderBatchGlyph glyph_batch;
    WorldRenderer world_renderer;
    BackgroundRenderer background_renderer;
    RenderBackend backend;
    
    uint32_t global_depth_index = 0;

    math::Rect ui_frustum;
    math::Rect nonscale_camera_frustum;
    math::Rect camera_frustum;
} state;

static constexpr uint32_t MAX_TEXTURES_COUNT = 32;

const LLGL::RenderSystemPtr& Renderer::Context() { return state.context; }
LLGL::SwapChain* Renderer::SwapChain() { return state.swap_chain; }
LLGL::CommandBuffer* Renderer::CommandBuffer() { return state.command_buffer; }
LLGL::CommandQueue* Renderer::CommandQueue() { return state.command_queue; }
const std::shared_ptr<CustomSurface>& Renderer::Surface() { return state.surface; }
LLGL::Buffer* Renderer::GlobalUniformBuffer() { return state.constant_buffer; }
RenderBackend Renderer::Backend() { return state.backend; }
uint32_t Renderer::GetGlobalDepthIndex() { return state.global_depth_index; };

bool Renderer::InitEngine(RenderBackend backend) {
    LLGL::Report report;

    void* configPtr = nullptr;
    size_t configSize = 0;

#ifdef BACKEND_OPENGL
    LLGL::RendererConfigurationOpenGL config;
    config.majorVersion = 4;
    config.minorVersion = 3;
    configPtr = &config;
    configSize = sizeof(config);
#endif

    LLGL::RenderSystemDescriptor rendererDesc;
    rendererDesc.moduleName = backend.ToString();
    rendererDesc.rendererConfig = configPtr;
    rendererDesc.rendererConfigSize = configSize;

#if DEBUG
    state.debugger = new LLGL::RenderingDebugger();
    rendererDesc.flags    = LLGL::RenderSystemFlags::DebugDevice;
    rendererDesc.debugger = state.debugger;
#endif

    state.context = LLGL::RenderSystem::Load(rendererDesc, &report);
    state.backend = backend;

    if (report.HasErrors()) {
        LOG_ERROR("%s", report.GetText());
        return false;
    }

    return true;
}

bool Renderer::Init(GLFWwindow* window, const LLGL::Extent2D& resolution, bool vsync, bool fullscreen) {
    const LLGL::RenderSystemPtr& context = state.context;

    state.surface = std::make_shared<CustomSurface>(window, resolution);

    LLGL::SwapChainDescriptor swapChainDesc;
    swapChainDesc.resolution = resolution;
    swapChainDesc.fullscreen = fullscreen;

    state.swap_chain = context->CreateSwapChain(swapChainDesc, state.surface);
    state.swap_chain->SetVsyncInterval(vsync);

    const auto& info = context->GetRendererInfo();

    LOG_INFO("Renderer:             %s", info.rendererName.c_str());
    LOG_INFO("Device:               %s", info.deviceName.c_str());
    LOG_INFO("Vendor:               %s", info.vendorName.c_str());
    LOG_INFO("Shading Language:     %s", info.shadingLanguageName.c_str());
    LOG_INFO("Swap Chain Format:    %s", LLGL::ToString(state.swap_chain->GetColorFormat()));
    LOG_INFO("Depth/Stencil Format: %s", LLGL::ToString(state.swap_chain->GetDepthStencilFormat()));

    LOG_INFO("Extensions:");
    for (const std::string& extension : info.extensionNames) {
        LOG_INFO("  %s", extension.c_str());
    }

    state.limits = context->GetRenderingCaps().limits;

    state.command_buffer = context->CreateCommandBuffer();
    state.command_queue = context->GetCommandQueue();

    state.constant_buffer = CreateConstantBuffer(sizeof(ProjectionsUniform));

    state.sprite_batch.init();
    state.glyph_batch.init();
    state.world_renderer.init();
    state.background_renderer.init();

    return true;
}

void Renderer::Begin(const Camera& camera) {
    auto* const commands = Renderer::CommandBuffer();
    auto* const swap_chain = Renderer::SwapChain();

    const math::Rect camera_frustum = math::Rect::from_corners(
        camera.position() + camera.get_projection_area().min,
        camera.position() + camera.get_projection_area().max
    );
    const math::Rect nonscale_camera_frustum = math::Rect::from_corners(
        camera.position() + camera.get_nonscale_projection_area().min,
        camera.position() + camera.get_nonscale_projection_area().max
    );
    const math::Rect ui_frustum = math::Rect::from_corners(glm::vec2(0.0), camera.viewport());

    state.camera_frustum = camera_frustum;
    state.nonscale_camera_frustum = nonscale_camera_frustum;
    state.ui_frustum = ui_frustum;

    auto projections_uniform = ProjectionsUniform {
        .screen_projection_matrix = camera.get_screen_projection_matrix(),
        .view_projection_matrix = camera.get_projection_matrix() * camera.get_view_matrix(),
        .nonscale_view_projection_matrix = camera.get_nonscale_projection_matrix() * camera.get_view_matrix(),
        .nonscale_projection_matrix = camera.get_nonscale_projection_matrix(),
        .transform_matrix = camera.get_transform_matrix(),
        .camera_position = camera.position(),
        .window_size = camera.viewport()
    };

    commands->Begin();
    commands->SetViewport(LLGL::Extent2D(camera.viewport().x, camera.viewport().y));

    commands->UpdateBuffer(*state.constant_buffer, 0, &projections_uniform, sizeof(projections_uniform));

    LLGL::ClearValue clear_value = LLGL::ClearValue(1.0f, 0.0f, 0.0f, 1.0f, 0.0f);

    commands->BeginRenderPass(*swap_chain);
    commands->Clear(LLGL::ClearFlags::ColorDepth, clear_value);

    state.sprite_batch.begin();
    state.glyph_batch.begin();

    state.global_depth_index = 0;
}

void Renderer::RenderWorld() {
    const uint32_t wall_depth = state.global_depth_index++;
    const uint32_t tile_depth = state.global_depth_index++;

    state.world_renderer.set_depth(wall_depth, tile_depth);
}

void Renderer::Render(const World& world) {
    auto* const commands = Renderer::CommandBuffer();
    auto* const queue = state.command_queue;
    auto* const swap_chain = Renderer::SwapChain();

    state.background_renderer.render();
    state.world_renderer.render(world);
    state.sprite_batch.render();
    state.glyph_batch.render();

    commands->EndRenderPass();
    commands->End();

    queue->Submit(*commands);

    swap_chain->Present();
}

#if DEBUG
void Renderer::PrintDebugInfo() {
    LLGL::FrameProfile profile;
    state.debugger->FlushProfile(&profile);

    LOG_DEBUG("Draw commands count: %u", profile.commandBufferRecord.drawCommands);
}
#endif

inline void add_sprite_to_batch(const Sprite& sprite, bool is_ui, int depth) {
    glm::vec4 uv_offset_scale = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);

    if (sprite.flip_x()) {
        uv_offset_scale.x += uv_offset_scale.z;
        uv_offset_scale.z *= -1.0f;
    }

    if (sprite.flip_y()) {
        uv_offset_scale.y += uv_offset_scale.w;
        uv_offset_scale.w *= -1.0f;
    }

    state.sprite_batch.draw_sprite(sprite, uv_offset_scale, sprite.texture(), is_ui, depth);
}

inline void add_atlas_sprite_to_batch(const TextureAtlasSprite& sprite, bool is_ui, int depth) {
    const math::Rect& rect = sprite.atlas().get_rect(sprite.index());

    glm::vec4 uv_offset_scale = glm::vec4(
        rect.min.x / sprite.atlas().texture().size.x,
        rect.min.y / sprite.atlas().texture().size.y,
        rect.size().x / sprite.atlas().texture().size.x,
        rect.size().y / sprite.atlas().texture().size.y
    );

    if (sprite.flip_x()) {
        uv_offset_scale.x += uv_offset_scale.z;
        uv_offset_scale.z *= -1.0f;
    }

    if (sprite.flip_y()) {
        uv_offset_scale.y += uv_offset_scale.w;
        uv_offset_scale.w *= -1.0f;
    }

    state.sprite_batch.draw_sprite(sprite, uv_offset_scale, sprite.atlas().texture(), is_ui, depth);
}

void Renderer::DrawSprite(const Sprite& sprite, RenderLayer render_layer, int depth) {
    const math::Rect aabb = sprite.calculate_aabb();

    if (sprite.nonscalable()) {
        if (!state.nonscale_camera_frustum.intersects(aabb)) return;
    } else {
        if (!state.camera_frustum.intersects(aabb)) return;
    }

    add_sprite_to_batch(sprite, false, depth);
}

void Renderer::DrawSpriteUI(const Sprite& sprite, int depth) {
    const math::Rect aabb = sprite.calculate_aabb();
    if (!state.ui_frustum.intersects(aabb)) return;

    add_sprite_to_batch(sprite, true, depth);
}

void Renderer::DrawAtlasSprite(const TextureAtlasSprite& sprite, RenderLayer render_layer, int depth) {
    const math::Rect aabb = sprite.calculate_aabb();

    if (sprite.nonscalable()) {
        if (!state.nonscale_camera_frustum.intersects(aabb)) return;
    } else {
        if (!state.camera_frustum.intersects(aabb)) return;
    }

    add_atlas_sprite_to_batch(sprite, false, depth);
}

void Renderer::DrawAtlasSpriteUI(const TextureAtlasSprite& sprite, int depth) {
    const math::Rect aabb = sprite.calculate_aabb();
    if (!state.ui_frustum.intersects(aabb)) return;

    add_atlas_sprite_to_batch(sprite, true, depth);
}

void Renderer::DrawText(const char* text, uint32_t length, float size, const glm::vec2& position, const glm::vec3& color, FontKey key, bool is_ui, int depth) {
    const Font& font = Assets::GetFont(key);
    
    float x = position.x;
    float y = position.y;

    float scale = size / font.font_size;

    uint32_t order = depth;
    if (order != -1) {
        state.global_depth_index = glm::max(state.global_depth_index, order + 1);
    } else {
        order = state.global_depth_index++;
    }

    for (uint32_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '\n') {
            y += size;
            x = position.x;
            continue;
        }

        const Glyph& ch = font.glyphs.find(c)->second;

        if (c == ' ') {
            x += (ch.advance >> 6) * scale;
            continue;
        }

        float xpos = x + ch.bearing.x * scale;
        float ypos = y - ch.bearing.y * scale;
        glm::vec2 pos = glm::vec2(xpos, ypos);
        glm::vec2 size = glm::vec2(ch.size) * scale;

        state.glyph_batch.draw_glyph(pos, size, color, font.texture, ch.texture_coords, ch.tex_size, is_ui, order);

        x += (ch.advance >> 6) * scale;
    }
}

void Renderer::DrawBackground(const BackgroundLayer& layer) {
    const math::Rect aabb = math::Rect::from_top_left(layer.position() - layer.anchor().to_vec2() * layer.size(), layer.size());

    if (layer.nonscale()) {
        if (!state.nonscale_camera_frustum.intersects(aabb)) return;
    } else {
        if (!state.camera_frustum.intersects(aabb)) return;
    }

    state.background_renderer.draw_layer(layer);
}

void Renderer::Terminate() {
    Assets::DestroyShaders();

    state.sprite_batch.terminate();
    state.world_renderer.terminate();
    state.background_renderer.terminate();

    if (state.constant_buffer) state.context->Release(*state.constant_buffer);

    if (state.command_buffer) state.context->Release(*state.command_buffer);
    if (state.swap_chain) state.context->Release(*state.swap_chain);

    Assets::DestroyTextures();
    Assets::DestroySamplers();

    LLGL::RenderSystem::Unload(std::move(state.context));
}

//
// ------------------ Batch ---------------------
//

void RenderBatchSprite::init() {
    m_buffer = new SpriteVertex[MAX_QUADS];

    m_vertex_buffer = CreateVertexBuffer(MAX_QUADS * sizeof(SpriteVertex), SpriteVertexFormat(), "SpriteBatch VertexBuffer");

    const uint32_t samplerBinding = Renderer::Backend().IsOpenGL() ? 2 : 3;

    LLGL::PipelineLayoutDescriptor pipelineLayoutDesc;
    pipelineLayoutDesc.bindings = {
        LLGL::BindingDescriptor(
            "GlobalUniformBuffer",
            LLGL::ResourceType::Buffer,
            LLGL::BindFlags::ConstantBuffer,
            LLGL::StageFlags::GeometryStage,
            LLGL::BindingSlot(1)
        ),
        LLGL::BindingDescriptor("u_texture", LLGL::ResourceType::Texture, LLGL::BindFlags::Sampled, LLGL::StageFlags::FragmentStage, LLGL::BindingSlot(2)),
        LLGL::BindingDescriptor("u_sampler", LLGL::ResourceType::Sampler, 0, LLGL::StageFlags::FragmentStage, LLGL::BindingSlot(samplerBinding)),
    };

    LLGL::PipelineLayout* pipelineLayout = state.context->CreatePipelineLayout(pipelineLayoutDesc);

    const ShaderPipeline& sprite_shader = Assets::GetShader(ShaderAssetKey::SpriteShader);

    LLGL::GraphicsPipelineDescriptor pipelineDesc;
    pipelineLayoutDesc.debugName = "SpriteBatch Pipeline";
    pipelineDesc.vertexShader = sprite_shader.vs;
    pipelineDesc.fragmentShader = sprite_shader.ps;
    pipelineDesc.geometryShader = sprite_shader.gs;
    pipelineDesc.pipelineLayout = pipelineLayout;
    pipelineDesc.indexFormat = LLGL::Format::Undefined;
    pipelineDesc.primitiveTopology = LLGL::PrimitiveTopology::PointList;
    pipelineDesc.renderPass = state.swap_chain->GetRenderPass();
    pipelineDesc.rasterizer.frontCCW = true;
    pipelineDesc.depth = LLGL::DepthDescriptor {
        .testEnabled = true,
        .writeEnabled = true,
        .compareOp = LLGL::CompareOp::GreaterEqual
    };
    pipelineDesc.blend = LLGL::BlendDescriptor {
        .targets = {
            LLGL::BlendTargetDescriptor {
                .blendEnabled = true,
                .srcColor = LLGL::BlendOp::SrcAlpha,
                .dstColor = LLGL::BlendOp::InvSrcAlpha,
                .srcAlpha = LLGL::BlendOp::Zero,
                .dstAlpha = LLGL::BlendOp::One
            }
        }
    };

    m_pipeline = state.context->CreatePipelineState(pipelineDesc);

    if (const LLGL::Report* report = m_pipeline->GetReport()) {
        if (report->HasErrors()) LOG_ERROR("%s", report->GetText());
    }
}

void RenderBatchSprite::draw_sprite(const BaseSprite& sprite, const glm::vec4& uv_offset_scale, const tl::optional<Texture>& sprite_texture, bool is_ui, int depth) {
    if (m_sprites.size() >= MAX_QUADS) {
        render();
        begin();
    }

    uint32_t order = depth;
    if (order != -1) {
        state.global_depth_index = glm::max(state.global_depth_index, order + 1);
    } else {
        order = state.global_depth_index++;
    }

    m_sprites.push_back(SpriteData {
        .position = sprite.position(),
        .rotation = sprite.rotation(),
        .size = sprite.size(),
        .offset = sprite.anchor().to_vec2(),
        .uv_offset_scale = uv_offset_scale,
        .color = sprite.color(),
        .outline_color = sprite.outline_color(),
        .outline_thickness = sprite.outline_thickness(),
        .texture = sprite_texture,
        .order = order,
        .is_ui = is_ui,
        .is_nonscalable = sprite.nonscalable()
    });
}

void RenderBatchSprite::render() {
    if (is_empty()) return;

    std::vector<SpriteData> sorted_sprites = m_sprites;
    std::sort(
        sorted_sprites.begin(),
        sorted_sprites.end(),
        [](const SpriteData& a, const SpriteData& b) {
            const int a_id = a.texture.is_some() ? a.texture->id : -1;
            const int b_id = b.texture.is_some() ? b.texture->id : -1;

            if (!a.is_ui && b.is_ui) return true;
            if (a.is_ui && !b.is_ui) return false;

            return a_id < b_id;
        }
    );
    
    tl::optional<Texture> prev_texture = sorted_sprites[0].texture;
    uint32_t sprite_count = 0;
    uint32_t total_sprite_count = 0;
    int vertex_offset = 0;

    for (const SpriteData& sprite_data : sorted_sprites) {
        const int prev_texture_id = prev_texture.is_some() ? prev_texture->id : -1;
        const int curr_texture_id = sprite_data.texture.is_some() ? sprite_data.texture->id : -1;

        if (prev_texture_id >= 0 && prev_texture_id != curr_texture_id) {
            m_sprite_flush_queue.push_back(FlushData {
                .texture = prev_texture,
                .offset = vertex_offset,
                .count = sprite_count
            });
            sprite_count = 0;
            vertex_offset = total_sprite_count;
        }

        const float order = static_cast<float>(sprite_data.order) / static_cast<float>(state.global_depth_index);

        m_buffer_ptr->position = glm::vec3(sprite_data.position, order);
        m_buffer_ptr->rotation = sprite_data.rotation;
        m_buffer_ptr->size = sprite_data.size;
        m_buffer_ptr->offset = sprite_data.offset;
        m_buffer_ptr->uv_offset_scale = sprite_data.uv_offset_scale;
        m_buffer_ptr->color = sprite_data.color;
        m_buffer_ptr->outline_color = sprite_data.outline_color;
        m_buffer_ptr->outline_thickness = sprite_data.outline_thickness;
        m_buffer_ptr->has_texture = curr_texture_id >= 0;
        m_buffer_ptr->is_ui = sprite_data.is_ui;
        m_buffer_ptr->is_nonscalable = sprite_data.is_nonscalable;
        m_buffer_ptr++;

        sprite_count += 1;
        total_sprite_count += 1;

        prev_texture = sprite_data.texture;
    }

    m_sprite_flush_queue.push_back(FlushData {
        .texture = prev_texture,
        .offset = vertex_offset,
        .count = sprite_count
    });

    flush();
}

void RenderBatchSprite::flush() {
    auto* const commands = Renderer::CommandBuffer();

    const ptrdiff_t size = (uint8_t*) m_buffer_ptr - (uint8_t*) m_buffer;
    if (size <= (1 << 16)) {
        commands->UpdateBuffer(*m_vertex_buffer, 0, m_buffer, size);
    } else {
        Renderer::Context()->WriteBuffer(*m_vertex_buffer, 0, m_buffer, size);
    }
    
    commands->SetVertexBuffer(*m_vertex_buffer);

    commands->SetPipelineState(*m_pipeline);
    commands->SetResource(0, *state.constant_buffer);

    for (const FlushData& flush_data : m_sprite_flush_queue) {
        const Texture& t = flush_data.texture.is_some() ? flush_data.texture.get() : Assets::GetTexture(TextureKey::Stub);

        commands->SetResource(1, *t.texture);
        commands->SetResource(2, Assets::GetSampler(t.sampler));
        commands->Draw(flush_data.count, flush_data.offset);
    }
}

void RenderBatchSprite::begin() {
    m_buffer_ptr = m_buffer;
    m_sprite_flush_queue.clear();
    m_sprites.clear();
}

void RenderBatchSprite::terminate() {
    if (m_vertex_buffer) state.context->Release(*m_vertex_buffer);
    if (m_pipeline) state.context->Release(*m_pipeline);

    delete[] m_buffer;
}


void RenderBatchGlyph::init() {
    m_buffer = new GlyphVertex[MAX_VERTICES];

    uint32_t indices[MAX_INDICES];
    uint32_t offset = 0;
    for (size_t i = 0; i < MAX_INDICES; i += 6) {
        indices[i + 0] = 0 + offset;
        indices[i + 1] = 1 + offset;
        indices[i + 2] = 2 + offset;
        indices[i + 3] = 2 + offset;
        indices[i + 4] = 3 + offset;
        indices[i + 5] = 1 + offset;
        offset += 4;
    }

    m_vertex_buffer = CreateVertexBuffer(MAX_VERTICES * sizeof(GlyphVertex), GlyphVertexFormat(), "GlyphBatch VertexBuffer");
    m_index_buffer = CreateIndexBuffer(indices, LLGL::Format::R32UInt, "GlyphBatch IndexBuffer");

    const uint32_t samplerBinding = Renderer::Backend().IsOpenGL() ? 2 : 3;

    LLGL::PipelineLayoutDescriptor pipelineLayoutDesc;
    pipelineLayoutDesc.bindings = {
        LLGL::BindingDescriptor(
            "GlobalUniformBuffer",
            LLGL::ResourceType::Buffer,
            LLGL::BindFlags::ConstantBuffer,
            LLGL::StageFlags::VertexStage,
            LLGL::BindingSlot(1)
        ),
        LLGL::BindingDescriptor("u_texture", LLGL::ResourceType::Texture, LLGL::BindFlags::Sampled, LLGL::StageFlags::FragmentStage, LLGL::BindingSlot(2)),
        LLGL::BindingDescriptor("u_sampler", LLGL::ResourceType::Sampler, 0, LLGL::StageFlags::FragmentStage, LLGL::BindingSlot(samplerBinding)),
    };

    LLGL::PipelineLayout* pipelineLayout = state.context->CreatePipelineLayout(pipelineLayoutDesc);

    const ShaderPipeline& font_shader = Assets::GetShader(ShaderAssetKey::FontShader);

    LLGL::GraphicsPipelineDescriptor pipelineDesc;
    pipelineLayoutDesc.debugName = "GlyphBatch Pipeline";
    pipelineDesc.vertexShader = font_shader.vs;
    pipelineDesc.fragmentShader = font_shader.ps;
    pipelineDesc.geometryShader = font_shader.gs;
    pipelineDesc.pipelineLayout = pipelineLayout;
    pipelineDesc.indexFormat = LLGL::Format::R32UInt;
    pipelineDesc.primitiveTopology = LLGL::PrimitiveTopology::TriangleList;
    pipelineDesc.renderPass = state.swap_chain->GetRenderPass();
    pipelineDesc.rasterizer.frontCCW = true;
    pipelineDesc.depth = LLGL::DepthDescriptor {
        .testEnabled = true,
        .writeEnabled = true,
        .compareOp = LLGL::CompareOp::GreaterEqual
    };
    pipelineDesc.blend = LLGL::BlendDescriptor {
        .targets = {
            LLGL::BlendTargetDescriptor {
                .blendEnabled = true,
                .srcColor = LLGL::BlendOp::SrcAlpha,
                .dstColor = LLGL::BlendOp::InvSrcAlpha,
                .srcAlpha = LLGL::BlendOp::Zero,
                .dstAlpha = LLGL::BlendOp::One
            }
        }
    };

    m_pipeline = state.context->CreatePipelineState(pipelineDesc);

    if (const LLGL::Report* report = m_pipeline->GetReport()) {
        if (report->HasErrors()) LOG_ERROR("%s", report->GetText());
    }
}

void RenderBatchGlyph::draw_glyph(const glm::vec2& pos, const glm::vec2& size, const glm::vec3& color, const Texture& font_texture, const glm::vec2& tex_uv, const glm::vec2& tex_size, bool ui, uint32_t depth) {
    if (m_glyphs.size() >= MAX_QUADS) {
        render();
        begin();
    }

    m_glyphs.push_back(GlyphData {
        .texture = font_texture,
        .color = color,
        .pos = pos,
        .size = size,
        .tex_size = tex_size,
        .tex_uv = tex_uv,
        .order = depth,
        .is_ui = ui
    });
}

void RenderBatchGlyph::render() {
    if (is_empty()) return;

    std::vector<GlyphData> sorted_glyphs = m_glyphs;
    std::sort(
        sorted_glyphs.begin(),
        sorted_glyphs.end(),
        [](const GlyphData& a, const GlyphData& b) {
            return a.texture.id < b.texture.id;
        }
    );
    
    Texture prev_texture = sorted_glyphs[0].texture;
    uint32_t sprite_count = 0;
    uint32_t total_sprite_count = 0;
    int vertex_offset = 0;

    for (const GlyphData& glyph_data : sorted_glyphs) {
        if (prev_texture.id >= 0 && prev_texture.id != glyph_data.texture.id) {
            m_glyphs_flush_queue.push_back(FlushData {
                .texture = prev_texture,
                .offset = vertex_offset,
                .count = sprite_count * 6
            });
            sprite_count = 0;
            vertex_offset = total_sprite_count * 4;
        }

        const float order = static_cast<float>(glyph_data.order) / static_cast<float>(state.global_depth_index);

        m_buffer_ptr->color = glyph_data.color;
        m_buffer_ptr->pos = glm::vec3(glyph_data.pos, order);
        m_buffer_ptr->uv = glyph_data.tex_uv;
        m_buffer_ptr->is_ui = glyph_data.is_ui;
        m_buffer_ptr++;

        m_buffer_ptr->color = glyph_data.color;
        m_buffer_ptr->pos = glm::vec3(glyph_data.pos, order);
        m_buffer_ptr->pos.x += glyph_data.size.x;
        m_buffer_ptr->uv = glyph_data.tex_uv;
        m_buffer_ptr->uv.x += glyph_data.tex_size.x;
        m_buffer_ptr->is_ui = glyph_data.is_ui;
        m_buffer_ptr++;

        m_buffer_ptr->color = glyph_data.color;
        m_buffer_ptr->pos = glm::vec3(glyph_data.pos, order);
        m_buffer_ptr->pos.y += glyph_data.size.y;
        m_buffer_ptr->uv = glyph_data.tex_uv;
        m_buffer_ptr->uv.y += glyph_data.tex_size.y;
        m_buffer_ptr->is_ui = glyph_data.is_ui;
        m_buffer_ptr++;

        m_buffer_ptr->color = glyph_data.color;
        m_buffer_ptr->pos = glm::vec3(glyph_data.pos + glyph_data.size, order);
        m_buffer_ptr->uv = glyph_data.tex_uv + glyph_data.tex_size;
        m_buffer_ptr->is_ui = glyph_data.is_ui;
        m_buffer_ptr++;

        sprite_count += 1;
        total_sprite_count += 1;

        prev_texture = glyph_data.texture;
    }

    m_glyphs_flush_queue.push_back(FlushData {
        .texture = prev_texture,
        .offset = vertex_offset,
        .count = sprite_count * 6
    });

    flush();
}

void RenderBatchGlyph::flush() {
    auto* const commands = Renderer::CommandBuffer();

    const ptrdiff_t size = (uint8_t*) m_buffer_ptr - (uint8_t*) m_buffer;
    if (size <= (1 << 16)) {
        commands->UpdateBuffer(*m_vertex_buffer, 0, m_buffer, size);
    } else {
        Renderer::Context()->WriteBuffer(*m_vertex_buffer, 0, m_buffer, size);
    }
    
    commands->SetVertexBuffer(*m_vertex_buffer);
    commands->SetIndexBuffer(*m_index_buffer);

    commands->SetPipelineState(*m_pipeline);
    commands->SetResource(0, *state.constant_buffer);

    for (const FlushData& flush_data : m_glyphs_flush_queue) {
        const Texture& t = flush_data.texture.get();

        commands->SetResource(1, *t.texture);
        commands->SetResource(2, Assets::GetSampler(t.sampler));
        commands->DrawIndexed(flush_data.count, 0, flush_data.offset);
    }
}

void RenderBatchGlyph::begin() {
    m_buffer_ptr = m_buffer;
    m_glyphs_flush_queue.clear();
    m_glyphs.clear();
}

void RenderBatchGlyph::terminate() {
    if (m_vertex_buffer) state.context->Release(*m_vertex_buffer);
    if (m_pipeline) state.context->Release(*m_pipeline);

    delete[] m_buffer;
}