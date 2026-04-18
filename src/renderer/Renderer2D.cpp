#include "renderer/Renderer2D.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <string>
#include <vector>

namespace novaiso::renderer {

bool Renderer2D::Initialize(const std::filesystem::path& shader_root) {
    if (!sprite_shader_.Load(shader_root / "sprite.vert", shader_root / "sprite.frag")) {
        return false;
    }
    if (!light_shader_.Load(shader_root / "sprite.vert", shader_root / "light.frag")) {
        return false;
    }

    white_texture_.CreateWhitePixel();
    {
        static constexpr std::uint8_t flat_normal[] = {128, 128, 255, 255};
        flat_normal_texture_.CreateFromRgba(1, 1, flat_normal, true, false);
        static constexpr std::uint8_t neutral_gray[] = {128, 128, 128, 255};
        neutral_height_texture_.CreateFromRgba(1, 1, neutral_gray, true, false);
        neutral_displacement_texture_.CreateFromRgba(1, 1, neutral_gray, true, false);
    }
    {
        constexpr int size = 128;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size * size * 4), 0);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                const float dx = u * 2.0f - 1.0f;
                const float dy = v * 2.0f - 1.0f;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float alpha = std::clamp(1.0f - distance, 0.0f, 1.0f);
                const float falloff = alpha * alpha * (3.0f - 2.0f * alpha);
                const std::uint8_t value = static_cast<std::uint8_t>(falloff * 255.0f);
                const std::size_t index = static_cast<std::size_t>((y * size + x) * 4);
                pixels[index + 0] = 255;
                pixels[index + 1] = 255;
                pixels[index + 2] = 255;
                pixels[index + 3] = value;
            }
        }
        radial_light_texture_.CreateFromRgba(size, size, pixels.data(), true, false);
    }

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glCreateBuffers(1, &ebo_);

    glNamedBufferData(vbo_, sizeof(Vertex) * 4096, nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(ebo_, sizeof(std::uint32_t) * 6144, nullptr, GL_DYNAMIC_DRAW);

    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));
    glEnableVertexArrayAttrib(vao_, 0);
    glEnableVertexArrayAttrib(vao_, 1);
    glEnableVertexArrayAttrib(vao_, 2);
    glEnableVertexArrayAttrib(vao_, 3);
    glEnableVertexArrayAttrib(vao_, 4);
    glEnableVertexArrayAttrib(vao_, 5);
    glEnableVertexArrayAttrib(vao_, 6);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribFormat(vao_, 1, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribFormat(vao_, 2, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, color));
    glVertexArrayAttribFormat(vao_, 3, 1, GL_FLOAT, GL_FALSE, offsetof(Vertex, reflection));
    glVertexArrayAttribFormat(vao_, 4, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, material));
    glVertexArrayAttribFormat(vao_, 5, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, lighting));
    glVertexArrayAttribFormat(vao_, 6, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, emissive));
    glVertexArrayAttribBinding(vao_, 0, 0);
    glVertexArrayAttribBinding(vao_, 1, 0);
    glVertexArrayAttribBinding(vao_, 2, 0);
    glVertexArrayAttribBinding(vao_, 3, 0);
    glVertexArrayAttribBinding(vao_, 4, 0);
    glVertexArrayAttribBinding(vao_, 5, 0);
    glVertexArrayAttribBinding(vao_, 6, 0);
    glVertexArrayElementBuffer(vao_, ebo_);
    return true;
}

void Renderer2D::Shutdown() {
    sprite_shader_.Destroy();
    light_shader_.Destroy();
    white_texture_.Destroy();
    radial_light_texture_.Destroy();
    flat_normal_texture_.Destroy();
    neutral_height_texture_.Destroy();
    neutral_displacement_texture_.Destroy();
    if (ebo_ != 0) {
        glDeleteBuffers(1, &ebo_);
        ebo_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

void Renderer2D::Begin(const camera::Camera2D& camera) {
    sprite_shader_.ReloadIfChanged();
    light_shader_.ReloadIfChanged();
    view_projection_ = camera.ViewProjection();
    lighting_enabled_ = false;
    rt_enabled_ = false;
    ambient_color_ = {0.08f, 0.09f, 0.14f, 1.0f};
    ambient_intensity_ = 0.35f;
    active_lights_.clear();
    vertices_.clear();
    indices_.clear();
    active_texture_ = nullptr;
    active_normal_texture_ = nullptr;
    active_height_texture_ = nullptr;
    active_displacement_texture_ = nullptr;
}

void Renderer2D::SetLightingState(bool lighting_enabled, bool rt_enabled, glm::vec4 ambient_color, float ambient_intensity, const std::vector<LightSource>& lights) {
    lighting_enabled_ = lighting_enabled;
    rt_enabled_ = rt_enabled;
    ambient_color_ = ambient_color;
    ambient_intensity_ = ambient_intensity;
    active_lights_.assign(lights.begin(), lights.begin() + std::min<std::size_t>(lights.size(), 16));
}

void Renderer2D::DrawSprite(
    const Texture2D& texture,
    glm::vec2 position,
    glm::vec2 size,
    glm::vec4 color,
    float depth,
    glm::vec4 uv,
    float rotation_radians,
    float reflection,
    glm::vec4 material,
    const Texture2D* normal_map,
    const Texture2D* height_map,
    const Texture2D* displacement_map,
    glm::vec4 lighting,
    glm::vec4 emissive
) {
    const glm::vec2 pivot = position + size * 0.5f;
    const glm::vec2 half = size * 0.5f;
    const float cosine = glm::cos(rotation_radians);
    const float sine = glm::sin(rotation_radians);

    auto rotate = [&](glm::vec2 point) {
        const glm::vec2 translated = point - pivot;
        return glm::vec2{
            translated.x * cosine - translated.y * sine,
            translated.x * sine + translated.y * cosine
        } + pivot;
    };

    const std::array<Vertex, 4> quad = {
        Vertex{glm::vec3(rotate(pivot + glm::vec2(-half.x, -half.y)), depth), {uv.x, uv.y}, color, reflection, material, lighting, emissive},
        Vertex{glm::vec3(rotate(pivot + glm::vec2(half.x, -half.y)), depth), {uv.z, uv.y}, color, reflection, material, lighting, emissive},
        Vertex{glm::vec3(rotate(pivot + glm::vec2(half.x, half.y)), depth), {uv.z, uv.w}, color, reflection, material, lighting, emissive},
        Vertex{glm::vec3(rotate(pivot + glm::vec2(-half.x, half.y)), depth), {uv.x, uv.w}, color, reflection, material, lighting, emissive},
    };
    PushQuad(texture, normal_map, height_map, displacement_map, quad);
}

void Renderer2D::DrawSpriteQuad(
    const Texture2D& texture,
    const std::array<glm::vec2, 4>& points,
    glm::vec4 color,
    float depth,
    glm::vec4 uv,
    float reflection,
    glm::vec4 material,
    const Texture2D* normal_map,
    const Texture2D* height_map,
    const Texture2D* displacement_map,
    glm::vec4 lighting,
    glm::vec4 emissive
) {
    const std::array<Vertex, 4> quad = {
        Vertex{glm::vec3(points[0], depth), {uv.x, uv.y}, color, reflection, material, lighting, emissive},
        Vertex{glm::vec3(points[1], depth), {uv.z, uv.y}, color, reflection, material, lighting, emissive},
        Vertex{glm::vec3(points[2], depth), {uv.z, uv.w}, color, reflection, material, lighting, emissive},
        Vertex{glm::vec3(points[3], depth), {uv.x, uv.w}, color, reflection, material, lighting, emissive},
    };
    PushQuad(texture, normal_map, height_map, displacement_map, quad);
}

void Renderer2D::DrawLight(
    glm::vec2 position,
    glm::vec2 size,
    glm::vec4 color,
    float source_radius,
    float scatter,
    float depth,
    bool rt_enabled,
    float light_type,
    glm::vec2 direction,
    float cone_angle,
    float cone_softness
) {
    FlushInternal();

    light_shader_.Bind();
    light_shader_.SetMat4("u_viewProjection", view_projection_);
    light_shader_.SetInt("u_rtEnabled", rt_enabled ? 1 : 0);
    light_shader_.SetInt("u_lightType", light_type >= 0.5f ? 1 : 0);
    const float normalized_source_radius = std::clamp(source_radius / std::max(size.x * 0.5f, 1.0f), 0.0f, 0.94f);
    light_shader_.SetFloat("u_sourceRadius", normalized_source_radius);
    light_shader_.SetFloat("u_scatter", scatter);
    if (glm::length(direction) < 0.001f) {
        direction = {1.0f, 0.0f};
    } else {
        direction = glm::normalize(direction);
    }
    const float half_angle_radians = glm::radians(std::clamp(cone_angle, 4.0f, 170.0f) * 0.5f);
    const float softness = std::clamp(cone_softness, 0.01f, 0.95f);
    const float outer_cos = std::cos(half_angle_radians);
    const float inner_cos = std::cos(std::max(half_angle_radians * (1.0f - softness), 0.01f));
    light_shader_.SetVec2("u_lightDirection", direction);
    light_shader_.SetFloat("u_coneInnerCos", inner_cos);
    light_shader_.SetFloat("u_coneOuterCos", outer_cos);

    const std::array<Vertex, 4> quad = {
        Vertex{glm::vec3(position.x, position.y, depth), {0.0f, 0.0f}, color, 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(position.x + size.x, position.y, depth), {1.0f, 0.0f}, color, 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(position.x + size.x, position.y + size.y, depth), {1.0f, 1.0f}, color, 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(position.x, position.y + size.y, depth), {0.0f, 1.0f}, color, 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
    };
    static constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};

    glNamedBufferSubData(vbo_, 0, static_cast<GLsizeiptr>(quad.size() * sizeof(Vertex)), quad.data());
    glNamedBufferSubData(ebo_, 0, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data());
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
}

void Renderer2D::DrawQuad(const std::array<glm::vec2, 4>& points, glm::vec4 color, float depth) {
    const std::array<Vertex, 4> quad = {
        Vertex{glm::vec3(points[0], depth), {0.0f, 0.0f}, color, 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(points[1], depth), {1.0f, 0.0f}, color, 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(points[2], depth), {1.0f, 1.0f}, color, 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(points[3], depth), {0.0f, 1.0f}, color, 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
    };
    PushQuad(white_texture_, nullptr, nullptr, nullptr, quad);
}

void Renderer2D::DrawQuadGradient(const std::array<glm::vec2, 4>& points, const std::array<glm::vec4, 4>& colors, float depth) {
    const std::array<Vertex, 4> quad = {
        Vertex{glm::vec3(points[0], depth), {0.0f, 0.0f}, colors[0], 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(points[1], depth), {1.0f, 0.0f}, colors[1], 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(points[2], depth), {1.0f, 1.0f}, colors[2], 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3(points[3], depth), {0.0f, 1.0f}, colors[3], 0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},
    };
    PushQuad(white_texture_, nullptr, nullptr, nullptr, quad);
}

void Renderer2D::DrawReflectiveSpriteQuad(
    const Texture2D& texture,
    const std::array<glm::vec2, 4>& points,
    glm::vec4 color,
    float reflection,
    float depth,
    glm::vec4 uv,
    glm::vec4 material,
    const Texture2D* normal_map,
    const Texture2D* height_map,
    const Texture2D* displacement_map,
    glm::vec4 lighting,
    glm::vec4 emissive
) {
    DrawSpriteQuad(texture, points, color, depth, uv, reflection, material, normal_map, height_map, displacement_map, lighting, emissive);
}

void Renderer2D::DrawRectOutline(glm::vec2 position, glm::vec2 size, glm::vec4 color, float thickness, float depth) {
    DrawSprite(white_texture_, position, {size.x, thickness}, color, depth);
    DrawSprite(white_texture_, {position.x, position.y + size.y - thickness}, {size.x, thickness}, color, depth);
    DrawSprite(white_texture_, position, {thickness, size.y}, color, depth);
    DrawSprite(white_texture_, {position.x + size.x - thickness, position.y}, {thickness, size.y}, color, depth);
}

void Renderer2D::Flush() {
    FlushInternal();
}

Texture2D& Renderer2D::WhiteTexture() {
    return white_texture_;
}

Texture2D& Renderer2D::RadialLightTexture() {
    return radial_light_texture_;
}

void Renderer2D::FlushInternal() {
    if (vertices_.empty() || active_texture_ == nullptr) {
        return;
    }

    sprite_shader_.Bind();
    sprite_shader_.SetMat4("u_viewProjection", view_projection_);
    active_texture_->Bind(0);
    (active_normal_texture_ != nullptr ? active_normal_texture_ : &flat_normal_texture_)->Bind(1);
    (active_height_texture_ != nullptr ? active_height_texture_ : &neutral_height_texture_)->Bind(2);
    (active_displacement_texture_ != nullptr ? active_displacement_texture_ : &neutral_displacement_texture_)->Bind(3);
    sprite_shader_.SetInt("u_texture", 0);
    sprite_shader_.SetInt("u_normalTexture", 1);
    sprite_shader_.SetInt("u_heightTexture", 2);
    sprite_shader_.SetInt("u_displacementTexture", 3);
    sprite_shader_.SetInt("u_lightingEnabled", lighting_enabled_ ? 1 : 0);
    sprite_shader_.SetInt("u_rtEnabled", rt_enabled_ ? 1 : 0);
    sprite_shader_.SetInt("u_lightCount", static_cast<int>(lighting_enabled_ ? active_lights_.size() : 0));
    sprite_shader_.SetVec4("u_ambientColor", ambient_color_);
    sprite_shader_.SetFloat("u_ambientIntensity", ambient_intensity_);
    for (int i = 0; i < static_cast<int>(active_lights_.size()); ++i) {
        const auto& light = active_lights_[static_cast<std::size_t>(i)];
        sprite_shader_.SetVec2(("u_lightPositions[" + std::to_string(i) + "]").c_str(), light.position);
        sprite_shader_.SetVec4(("u_lightColors[" + std::to_string(i) + "]").c_str(), light.color);
        sprite_shader_.SetVec2(("u_lightParams[" + std::to_string(i) + "]").c_str(), {light.radius, light.intensity});
        sprite_shader_.SetVec2(("u_lightShapeParams[" + std::to_string(i) + "]").c_str(), {light.source_radius, light.scatter});
        sprite_shader_.SetVec4(
            ("u_lightDirectionParams[" + std::to_string(i) + "]").c_str(),
            {light.direction.x, light.direction.y, light.cone_angle, light.cone_softness});
        sprite_shader_.SetVec2(("u_lightExtraParams[" + std::to_string(i) + "]").c_str(), {light.type, light.length});
    }

    glNamedBufferSubData(vbo_, 0, static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex)), vertices_.data());
    glNamedBufferSubData(ebo_, 0, static_cast<GLsizeiptr>(indices_.size() * sizeof(std::uint32_t)), indices_.data());
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices_.size()), GL_UNSIGNED_INT, nullptr);

    vertices_.clear();
    indices_.clear();
}

void Renderer2D::PushQuad(
    const Texture2D& texture,
    const Texture2D* normal_map,
    const Texture2D* height_map,
    const Texture2D* displacement_map,
    const std::array<Vertex, 4>& quad) {
    const Texture2D* resolved_normal = normal_map != nullptr ? normal_map : &flat_normal_texture_;
    const Texture2D* resolved_height = height_map != nullptr ? height_map : &neutral_height_texture_;
    const Texture2D* resolved_displacement = displacement_map != nullptr ? displacement_map : &neutral_displacement_texture_;
    if (active_texture_ != nullptr &&
        (active_texture_->Id() != texture.Id() ||
         active_normal_texture_->Id() != resolved_normal->Id() ||
         active_height_texture_->Id() != resolved_height->Id() ||
         active_displacement_texture_->Id() != resolved_displacement->Id())) {
        FlushInternal();
    }

    active_texture_ = &texture;
    active_normal_texture_ = resolved_normal;
    active_height_texture_ = resolved_height;
    active_displacement_texture_ = resolved_displacement;
    const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
    vertices_.insert(vertices_.end(), quad.begin(), quad.end());
    indices_.insert(indices_.end(), {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});

    if (vertices_.size() >= 4092) {
        FlushInternal();
    }
}

}  // namespace novaiso::renderer
