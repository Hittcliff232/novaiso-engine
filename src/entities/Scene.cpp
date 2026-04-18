#include "entities/Scene.h"

#include "core/Input.h"
#include "core/ThreadPool.h"
#include "renderer/Renderer2D.h"
#include "scripting/PythonScripting.h"

#include <glad/gl.h>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <string_view>
#include <unordered_set>

namespace novaiso::entities {

namespace {

constexpr float kFixedStep = 1.0f / 120.0f;

glm::vec2 TextureDrawSize(const renderer::Texture2D& texture, glm::vec2 scale) {
    glm::vec2 size = glm::vec2(texture.Size()) * scale;
    if (size.x < 2.0f || size.y < 2.0f) {
        size = {128.0f * scale.x, 128.0f * scale.y};
    }
    return size;
}

glm::vec2 ColliderPosition(const Entity& entity) {
    return entity.position + entity.collider_offset;
}

glm::vec2 ColliderSize(const Entity& entity) {
    return {
        std::max(entity.collider_size.x, 1.0f),
        std::max(entity.collider_size.y, 1.0f)
    };
}

bool Intersects(glm::vec2 a_position, glm::vec2 a_size, glm::vec2 b_position, glm::vec2 b_size) {
    return a_position.x < b_position.x + b_size.x &&
           a_position.x + a_size.x > b_position.x &&
           a_position.y < b_position.y + b_size.y &&
           a_position.y + a_size.y > b_position.y;
}

bool Vec2Equals(glm::vec2 lhs, glm::vec2 rhs) {
    return std::abs(lhs.x - rhs.x) < 0.001f && std::abs(lhs.y - rhs.y) < 0.001f;
}

bool Vec4Equals(glm::vec4 lhs, glm::vec4 rhs) {
    return std::abs(lhs.x - rhs.x) < 0.001f &&
           std::abs(lhs.y - rhs.y) < 0.001f &&
           std::abs(lhs.z - rhs.z) < 0.001f &&
           std::abs(lhs.w - rhs.w) < 0.001f;
}

float Distance(glm::vec2 lhs, glm::vec2 rhs) {
    return glm::length(lhs - rhs);
}

bool IsTileOccluder(const assets::LevelData& level, const assets::TileLayer& layer, int tile_id) {
    if (tile_id <= 0) {
        return false;
    }
    if (layer.collidable) {
        return true;
    }
    const auto it = level.tileset.tile_info.find(tile_id);
    if (it == level.tileset.tile_info.end()) {
        return false;
    }
    return it->second.collision != assets::CollisionType::Empty;
}

void MergeJsonDefaults(nlohmann::json& destination, const nlohmann::json& defaults) {
    if (!defaults.is_object()) {
        return;
    }
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!destination.contains(it.key())) {
            destination[it.key()] = it.value();
        }
    }
}

const assets::SpriteAnimation* FindAnimation(const assets::SpriteAsset& sprite, const std::string& name) {
    for (const auto& animation : sprite.animations) {
        if (animation.name == name) {
            return &animation;
        }
    }
    if (!sprite.animations.empty()) {
        return &sprite.animations.front();
    }
    return nullptr;
}

const assets::SpriteAnimation* FindAnimationExact(const assets::SpriteAsset& sprite, std::string_view name) {
    for (const auto& animation : sprite.animations) {
        if (animation.name == name) {
            return &animation;
        }
    }
    return nullptr;
}

void UpdateAutomaticAnimation(Entity& entity, const assets::SpriteAsset& sprite) {
    if (!entity.dynamic) {
        return;
    }

    const bool has_idle = FindAnimationExact(sprite, "idle") != nullptr;
    const bool has_run = FindAnimationExact(sprite, "run") != nullptr;
    const bool has_walk = FindAnimationExact(sprite, "walk") != nullptr;
    const bool has_jump = FindAnimationExact(sprite, "jump") != nullptr;
    if (!has_idle && !has_run && !has_walk && !has_jump) {
        return;
    }

    std::string target = entity.animation;
    const float horizontal_speed = std::abs(entity.velocity.x);
    const float vertical_speed = std::abs(entity.velocity.y);
    if (has_jump && !entity.grounded && (vertical_speed > 1.0f || entity.animation == "jump")) {
        target = "jump";
    } else if (horizontal_speed > 5.0f) {
        target = has_run ? "run" : (has_walk ? "walk" : entity.animation);
    } else if (has_idle) {
        target = "idle";
    } else if (has_run) {
        target = "run";
    } else if (has_walk) {
        target = "walk";
    }

    if (target != entity.animation) {
        entity.animation = target;
        entity.animation_frame = 0;
        entity.animation_time = 0.0f;
    }
}

std::vector<std::string> GatherResources(
    const std::filesystem::path& project_root,
    const std::vector<std::string>& listed,
    const std::filesystem::path& folder,
    std::string_view extension
) {
    std::vector<std::string> result = listed;
    std::unordered_set<std::string> seen(listed.begin(), listed.end());

    if (std::filesystem::exists(folder)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
            if (!entry.is_regular_file() || entry.path().extension().string() != extension) {
                continue;
            }
            const std::string relative = std::filesystem::relative(entry.path(), project_root).generic_string();
            if (seen.insert(relative).second) {
                result.push_back(relative);
            }
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::pair<glm::vec2, glm::vec2> VisibleWorldBounds(const camera::Camera2D& camera) {
    const glm::vec2 viewport = camera.Viewport();
    const std::array<glm::vec2, 4> corners{
        camera.ScreenToWorld({0.0f, 0.0f}),
        camera.ScreenToWorld({viewport.x, 0.0f}),
        camera.ScreenToWorld({0.0f, viewport.y}),
        camera.ScreenToWorld({viewport.x, viewport.y})
    };

    glm::vec2 minimum = corners.front();
    glm::vec2 maximum = corners.front();
    for (const auto& corner : corners) {
        minimum.x = std::min(minimum.x, corner.x);
        minimum.y = std::min(minimum.y, corner.y);
        maximum.x = std::max(maximum.x, corner.x);
        maximum.y = std::max(maximum.y, corner.y);
    }
    return {minimum, maximum};
}

bool MatchesIdOrName(std::string_view value, std::string_view id, std::string_view name) {
    return (!value.empty() && (value == id || value == name));
}

bool IsInternalNonPostEffect(std::string_view effect_name) {
    return effect_name == "sprite" ||
           effect_name == "light" ||
           effect_name == "wet_reflection" ||
           effect_name == "post";
}

float SampleFloatKeys(const std::vector<assets::AnimationFloatKey>& keys, float normalized_time, float fallback) {
    if (keys.empty()) {
        return fallback;
    }
    const float t = std::clamp(normalized_time, 0.0f, 1.0f);
    if (keys.size() == 1 || t <= keys.front().time) {
        return keys.front().value;
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        const auto& previous = keys[i - 1];
        const auto& current = keys[i];
        if (t <= current.time) {
            const float span = std::max(current.time - previous.time, 0.0001f);
            const float local = std::clamp((t - previous.time) / span, 0.0f, 1.0f);
            return previous.value + (current.value - previous.value) * local;
        }
    }
    return keys.back().value;
}

glm::vec2 SampleVec2Keys(const std::vector<assets::AnimationVec2Key>& keys, float normalized_time, glm::vec2 fallback) {
    if (keys.empty()) {
        return fallback;
    }
    const float t = std::clamp(normalized_time, 0.0f, 1.0f);
    if (keys.size() == 1 || t <= keys.front().time) {
        return keys.front().value;
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        const auto& previous = keys[i - 1];
        const auto& current = keys[i];
        if (t <= current.time) {
            const float span = std::max(current.time - previous.time, 0.0001f);
            const float local = std::clamp((t - previous.time) / span, 0.0f, 1.0f);
            return previous.value + (current.value - previous.value) * local;
        }
    }
    return keys.back().value;
}

glm::vec2 SamplePath(const std::vector<glm::vec2>& points, float normalized_time) {
    if (points.empty()) {
        return {0.0f, 0.0f};
    }
    if (points.size() == 1) {
        return points.front();
    }

    const float t = std::clamp(normalized_time, 0.0f, 1.0f);
    const float scaled = t * static_cast<float>(points.size() - 1);
    const int segment = std::clamp(static_cast<int>(std::floor(scaled)), 0, static_cast<int>(points.size()) - 2);
    const float local = scaled - static_cast<float>(segment);

    const glm::vec2 p0 = points[static_cast<std::size_t>(std::max(segment - 1, 0))];
    const glm::vec2 p1 = points[static_cast<std::size_t>(segment)];
    const glm::vec2 p2 = points[static_cast<std::size_t>(std::min(segment + 1, static_cast<int>(points.size()) - 1))];
    const glm::vec2 p3 = points[static_cast<std::size_t>(std::min(segment + 2, static_cast<int>(points.size()) - 1))];

    const float local2 = local * local;
    const float local3 = local2 * local;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * local +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * local2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * local3);
}

glm::vec4 EntityUv(const Entity& entity) {
    glm::vec4 uv = entity.uv;
    if (!entity.facing_right) {
        std::swap(uv.x, uv.z);
    }
    return uv;
}

glm::vec4 EntitySurfaceMaterial(const Entity& entity, float bump_scale = 1.0f, float parallax_scale = 1.0f) {
    if (!entity.relief_enabled) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return {
        std::clamp(entity.bump_strength * bump_scale, 0.0f, 4.0f),
        std::clamp(entity.relief_depth, 0.0f, 0.12f),
        std::clamp(entity.parallax_depth * parallax_scale, 0.0f, 0.08f),
        std::clamp(entity.relief_contrast, 0.2f, 4.0f)
    };
}

std::array<glm::vec2, 4> BuildEntityQuad(const Entity& entity) {
    const glm::vec2 center = entity.position + entity.size * 0.5f;
    const glm::vec2 half = entity.size * 0.5f;
    const float rotation_radians = entity.rotation * 0.01745329252f;
    const float cosine = std::cos(rotation_radians);
    const float sine = std::sin(rotation_radians);
    const std::array<glm::vec2, 4> local_corners{
        glm::vec2(-half.x, -half.y),
        glm::vec2(half.x, -half.y),
        glm::vec2(half.x, half.y),
        glm::vec2(-half.x, half.y)
    };

    std::array<glm::vec2, 4> world_corners{};
    for (std::size_t i = 0; i < local_corners.size(); ++i) {
        glm::vec2 local = local_corners[i];
        const float original_x = local.x;
        const float original_y = local.y;
        local.x += original_y * entity.skew.x;
        local.y += original_x * entity.skew.y;
        world_corners[i] = center + glm::vec2{
            local.x * cosine - local.y * sine,
            local.x * sine + local.y * cosine
        };
    }
    return world_corners;
}

glm::vec4 ScaleRgb(glm::vec4 color, float factor) {
    color.r = std::max(color.r * factor, 0.0f);
    color.g = std::max(color.g * factor, 0.0f);
    color.b = std::max(color.b * factor, 0.0f);
    return color;
}

glm::vec4 BiasRgb(glm::vec4 color, float bias) {
    color.r = std::max(color.r + bias, 0.0f);
    color.g = std::max(color.g + bias, 0.0f);
    color.b = std::max(color.b + bias, 0.0f);
    return color;
}

glm::vec4 ClampRgb(glm::vec4 color, float maximum) {
    color.r = std::clamp(color.r, 0.0f, maximum);
    color.g = std::clamp(color.g, 0.0f, maximum);
    color.b = std::clamp(color.b, 0.0f, maximum);
    return color;
}

bool IsFlashlight(const assets::LightDefinition& light) {
    std::string type = light.type;
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return type == "flashlight" || type == "spot" || type == "spotlight" || type == "directional";
}

glm::vec2 LightDirection(const assets::LightDefinition& light) {
    const float radians = glm::radians(light.direction_degrees);
    glm::vec2 direction{std::cos(radians), std::sin(radians)};
    if (glm::length(direction) < 0.001f) {
        direction = {1.0f, 0.0f};
    }
    return glm::normalize(direction);
}

float LightRange(const assets::LightDefinition& light) {
    return IsFlashlight(light)
        ? std::max(light.length, 24.0f)
        : std::max(light.radius, 8.0f);
}

bool IsParticleEmitterEntity(const nlohmann::json& properties, std::string_view archetype) {
    return archetype == "particle_emitter" ||
           (properties.is_object() && properties.value("particle_emitter", false));
}

bool IsParticleEmitterEntity(const Entity& entity) {
    return IsParticleEmitterEntity(entity.properties, entity.archetype);
}

std::string ParticleEmitterAssetPath(const nlohmann::json& properties) {
    if (!properties.is_object()) {
        return {};
    }
    return properties.value("particle_asset", std::string{});
}

bool JsonBoolValue(const nlohmann::json& properties, std::string_view key, bool fallback) {
    if (!properties.is_object()) {
        return fallback;
    }
    return properties.value(std::string(key), fallback);
}

float JsonFloatValue(const nlohmann::json& properties, std::string_view key, float fallback) {
    if (!properties.is_object()) {
        return fallback;
    }
    return properties.value(std::string(key), fallback);
}

int JsonIntValue(const nlohmann::json& properties, std::string_view key, int fallback) {
    if (!properties.is_object()) {
        return fallback;
    }
    return properties.value(std::string(key), fallback);
}

std::string ToLowerCopy(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return lowered;
}

bool JsonStringEquals(const nlohmann::json& properties, std::string_view key, std::string_view expected) {
    if (!properties.is_object()) {
        return false;
    }
    const auto it = properties.find(std::string(key));
    if (it == properties.end() || !it->is_string()) {
        return false;
    }
    return ToLowerCopy(it->get_ref<const std::string&>()) == ToLowerCopy(expected);
}

bool EntityMatchesToken(const Entity& entity, std::string_view raw_token) {
    const std::string token = ToLowerCopy(raw_token);
    if (token.empty()) {
        return false;
    }
    return ToLowerCopy(entity.id) == token ||
           ToLowerCopy(entity.archetype) == token ||
           JsonStringEquals(entity.properties, "preset", token) ||
           JsonStringEquals(entity.properties, "role", token);
}

bool EntityLooksLikePlayer(const Entity& entity) {
    if (ToLowerCopy(entity.id) == "player" || ToLowerCopy(entity.archetype) == "player") {
        return true;
    }
    if (!entity.script.empty() && entity.script.ends_with("preset_player.py")) {
        return true;
    }
    return JsonBoolValue(entity.properties, "is_player", false) ||
           JsonStringEquals(entity.properties, "preset", "player") ||
           JsonStringEquals(entity.properties, "role", "player");
}

std::string StripAttachmentPrefix(std::string_view target) {
    std::string lowered = ToLowerCopy(target);
    if (lowered.rfind("parallax:", 0) == 0) {
        return std::string(target.substr(9));
    }
    if (lowered.rfind("layer:", 0) == 0) {
        return std::string(target.substr(6));
    }
    return std::string(target);
}

const assets::ParallaxLayer* FindAttachedParallaxLayer(const assets::LevelData& level, std::string_view target) {
    if (target.empty()) {
        return nullptr;
    }
    const std::string normalized = ToLowerCopy(StripAttachmentPrefix(target));
    for (const auto& layer : level.parallax_layers) {
        if ((!layer.id.empty() && ToLowerCopy(layer.id) == normalized) ||
            (!layer.name.empty() && ToLowerCopy(layer.name) == normalized)) {
            return &layer;
        }
    }
    return nullptr;
}

glm::vec2 ResolveParallaxAnchorPosition(const assets::ParallaxLayer& layer, const camera::Camera2D& camera) {
    return layer.offset + camera.Position() * (layer.speed * layer.zoom_factor);
}

}  // namespace

bool Scene::Load(
    const std::filesystem::path& project_root,
    const assets::ProjectData& project,
    const std::string& level_relative_path,
    assets::AssetManager& asset_manager,
    scripting::PythonScripting& scripting
) {
    project_root_ = project_root;
    level_path_ = project_root / level_relative_path;
    project_ = project;
    asset_manager_ = &asset_manager;
    scripting_ = &scripting;
    level_ = assets::LoadLevel(level_path_);
    level_.post_effects.erase(
        std::remove_if(level_.post_effects.begin(), level_.post_effects.end(),
            [](const std::string& effect) { return IsInternalNonPostEffect(effect); }),
        level_.post_effects.end());

    SetCameraMode(project.camera_mode == "isometric" ? camera::Mode::Isometric : camera::Mode::Side);
    camera_mode_before_virtual_camera_ = GetCameraMode();
    camera_follow_target_id_ = "player";
    LoadResourceLibraries();
    trigger_system_.Reset();
    ResetSimulation();
    return true;
}

void Scene::SaveLevelToDisk() const {
    assets::SaveLevel(level_path_, level_);
}

void Scene::ResetSimulation(bool reset_camera) {
    accumulator_ = 0.0f;
    scene_time_seconds_ = 0.0f;
    trigger_system_.Reset();
    if (asset_manager_ != nullptr) {
        asset_manager_->StopAllAudio();
    }
    audio_source_states_.clear();
    audio_pak_states_.clear();
    pending_trigger_actions_.clear();
    trigger_last_fired_time_.clear();
    active_virtual_camera_index_ = -1;
    RebuildRuntimeEntities();
    if (reset_camera) {
        camera_.SetPosition(level_.player_spawn);
    }
    camera_zoom_target_ = camera_.Zoom();
    camera_zoom_speed_ = 0.0f;
    camera_follow_target_id_ = level_.player_camera.follow_target.empty() ? "player" : level_.player_camera.follow_target;
}

void Scene::RefreshResources(const assets::ProjectData& project, bool reset_camera) {
    project_ = project;
    LoadResourceLibraries();
    ResetSimulation(reset_camera);
}

void Scene::Update(float delta_time, const core::Input& input) {
    input_ = &input;
    scene_time_seconds_ += delta_time;

    if (debug_trace_enabled_) {
        Trace("Frame update dt=" + std::to_string(delta_time) + " scene_t=" + std::to_string(scene_time_seconds_));
    }

    if (scripting_ != nullptr) {
        scripting_->ReloadChanged();
    }

    accumulator_ += delta_time;
    while (accumulator_ >= kFixedStep) {
        StepFixed(kFixedStep);
        accumulator_ -= kFixedStep;
    }

    UpdateSceneAnimations(delta_time);
    UpdateAnimations(delta_time);
    UpdateParticles(delta_time);

    if (camera_zoom_speed_ > 0.0f && active_virtual_camera_index_ < 0) {
        const float current = camera_.Zoom();
        const float blend = std::clamp(delta_time * camera_zoom_speed_, 0.0f, 1.0f);
        const float next = current + (camera_zoom_target_ - current) * blend;
        camera_.SetZoom(next);
        if (std::abs(camera_zoom_target_ - camera_.Zoom()) < 0.0025f) {
            camera_.SetZoom(camera_zoom_target_);
            camera_zoom_speed_ = 0.0f;
        }
    }

    UpdateVirtualCamera(delta_time);
    UpdatePlayerCamera(delta_time);

    UpdateAudio(delta_time);

    for (auto it = pending_trigger_actions_.begin(); it != pending_trigger_actions_.end();) {
        it->remaining_time -= delta_time;
        if (it->remaining_time <= 0.0f) {
            if (scripting_ != nullptr) {
                if (debug_trace_enabled_) {
                    Trace("Execute delayed action `" + it->action.function + "` from trigger `" + it->trigger_name + "`");
                }
                scripting_->ExecuteAction(it->action, *this, it->trigger_name);
            }
            it = pending_trigger_actions_.erase(it);
        } else {
            ++it;
        }
    }

    if (messages_.size() > 160) {
        messages_.erase(messages_.begin(), messages_.begin() + static_cast<std::ptrdiff_t>(messages_.size() - 160));
    }
}

void Scene::Render(renderer::Renderer2D& renderer, bool draw_debug) const {
    const auto total_start = std::chrono::steady_clock::now();
    last_render_pass_timings_.clear();
    auto measure_pass = [&](std::string name, auto&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        last_render_pass_timings_.push_back({
            std::move(name),
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count()
        });
    };

    const std::vector<renderer::Renderer2D::LightSource> active_lights = BuildActiveLights();
    const std::vector<const Entity*> visible_entities = BuildVisibleEntities();

    renderer.SetLightingState(
        level_.lighting.enabled || !active_lights.empty(),
        level_.lighting.rt_enabled,
        level_.lighting.ambient_color,
        level_.lighting.ambient_intensity,
        active_lights);
    measure_pass("Parallax", [&] { RenderParallax(renderer); });
    measure_pass("Tiles", [&] { RenderTileLayers(renderer); });
    measure_pass("Entities", [&] { RenderEntities(renderer, visible_entities); });
    measure_pass("Particles", [&] { RenderParticles(renderer); });
    measure_pass("Lights", [&] { RenderLights(renderer); });
    measure_pass("Shadows", [&] { RenderLightShadows(renderer); });
    if (draw_debug) {
        measure_pass("Debug", [&] { RenderDebug(renderer); });
    }
    last_render_total_cpu_ms_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - total_start).count();
}

std::vector<renderer::Renderer2D::LightSource> Scene::BuildActiveLights() const {
    std::vector<renderer::Renderer2D::LightSource> result;
    if (level_.lights.empty()) {
        return result;
    }

    const bool multithreaded = project_.multithreading_enabled && core::ThreadPool::Shared().Enabled() && level_.lights.size() >= 2;
    if (!multithreaded) {
        result.reserve(level_.lights.size());
        for (const auto& light : level_.lights) {
            const float range = LightRange(light);
            if (!light.enabled || range <= 1.0f || light.intensity <= 0.0f) {
                continue;
            }
            result.push_back({
                .position = ResolveLightPosition(light),
                .color = light.color,
                .direction = LightDirection(light),
                .radius = range,
                .length = range,
                .source_radius = light.source_radius,
                .scatter = light.scatter,
                .cone_angle = light.cone_angle,
                .cone_softness = light.cone_softness,
                .intensity = light.intensity,
                .type = IsFlashlight(light) ? 1.0f : 0.0f
            });
        }
        return result;
    }

    const std::size_t chunk_count = std::max<std::size_t>(1, core::ThreadPool::Shared().WorkerCount() + 1);
    std::vector<std::vector<renderer::Renderer2D::LightSource>> partials(chunk_count);
    core::ThreadPool::Shared().ParallelFor(level_.lights.size(), [&](std::size_t begin, std::size_t end) {
        const std::size_t range_index = std::min(begin / std::max<std::size_t>((level_.lights.size() + chunk_count - 1) / chunk_count, 1), chunk_count - 1);
        auto& bucket = partials[range_index];
        bucket.reserve(end - begin);
        for (std::size_t index = begin; index < end; ++index) {
            const auto& light = level_.lights[index];
            const float range = LightRange(light);
            if (!light.enabled || range <= 1.0f || light.intensity <= 0.0f) {
                continue;
            }
            bucket.push_back({
                .position = ResolveLightPosition(light),
                .color = light.color,
                .direction = LightDirection(light),
                .radius = range,
                .length = range,
                .source_radius = light.source_radius,
                .scatter = light.scatter,
                .cone_angle = light.cone_angle,
                .cone_softness = light.cone_softness,
                .intensity = light.intensity,
                .type = IsFlashlight(light) ? 1.0f : 0.0f
            });
        }
    }, 4);

    std::size_t total = 0;
    for (const auto& bucket : partials) {
        total += bucket.size();
    }
    result.reserve(total);
    for (auto& bucket : partials) {
        result.insert(result.end(), bucket.begin(), bucket.end());
    }
    return result;
}

std::vector<const Entity*> Scene::BuildVisibleEntities() const {
    std::vector<const Entity*> visible_entities;
    if (runtime_entities_.empty()) {
        return visible_entities;
    }

    const auto [minimum, maximum] = VisibleWorldBounds(camera_);
    const glm::vec2 cull_margin{256.0f, 256.0f};
    const glm::vec2 cull_min = minimum - cull_margin;
    const glm::vec2 cull_max = maximum + cull_margin;
    const bool multithreaded =
        project_.multithreading_enabled &&
        core::ThreadPool::Shared().Enabled() &&
        runtime_entities_.size() >= 32;

    if (!multithreaded) {
        visible_entities.reserve(runtime_entities_.size());
        for (const auto& entity : runtime_entities_) {
            if (!entity.active || !entity.visible) {
                continue;
            }
            const glm::vec2 entity_min = entity.position;
            const glm::vec2 entity_max = entity.position + entity.size;
            if (entity_max.x < cull_min.x || entity_max.y < cull_min.y || entity_min.x > cull_max.x || entity_min.y > cull_max.y) {
                continue;
            }
            visible_entities.push_back(&entity);
        }
    } else {
        const std::size_t chunk_count = std::max<std::size_t>(1, core::ThreadPool::Shared().WorkerCount() + 1);
        std::vector<std::vector<const Entity*>> partials(chunk_count);
        core::ThreadPool::Shared().ParallelFor(runtime_entities_.size(), [&](std::size_t begin, std::size_t end) {
            const std::size_t range_index = std::min(begin / std::max<std::size_t>((runtime_entities_.size() + chunk_count - 1) / chunk_count, 1), chunk_count - 1);
            auto& bucket = partials[range_index];
            bucket.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index) {
                const auto& entity = runtime_entities_[index];
                if (!entity.active || !entity.visible) {
                    continue;
                }
                const glm::vec2 entity_min = entity.position;
                const glm::vec2 entity_max = entity.position + entity.size;
                if (entity_max.x < cull_min.x || entity_max.y < cull_min.y || entity_min.x > cull_max.x || entity_min.y > cull_max.y) {
                    continue;
                }
                bucket.push_back(&entity);
            }
        }, 32);

        std::size_t total = 0;
        for (const auto& bucket : partials) {
            total += bucket.size();
        }
        visible_entities.reserve(total);
        for (auto& bucket : partials) {
            visible_entities.insert(visible_entities.end(), bucket.begin(), bucket.end());
        }
    }

    std::sort(visible_entities.begin(), visible_entities.end(), [](const Entity* lhs, const Entity* rhs) {
        if (lhs->layer != rhs->layer) {
            return lhs->layer < rhs->layer;
        }
        if (std::abs(lhs->position.y - rhs->position.y) > 0.001f) {
            return lhs->position.y < rhs->position.y;
        }
        return lhs->id < rhs->id;
    });
    return visible_entities;
}

Entity* Scene::FindEntity(const std::string& id) {
    auto it = std::find_if(runtime_entities_.begin(), runtime_entities_.end(), [&](const Entity& entity) { return entity.id == id; });
    if (it != runtime_entities_.end()) {
        return &(*it);
    }
    if (ToLowerCopy(id) == "player") {
        return ResolvePlayerEntity();
    }
    return nullptr;
}

const Entity* Scene::FindEntity(const std::string& id) const {
    auto it = std::find_if(runtime_entities_.begin(), runtime_entities_.end(), [&](const Entity& entity) { return entity.id == id; });
    if (it != runtime_entities_.end()) {
        return &(*it);
    }
    if (ToLowerCopy(id) == "player") {
        return ResolvePlayerEntity();
    }
    return nullptr;
}

Entity* Scene::Player() {
    return ResolvePlayerEntity();
}

const Entity* Scene::Player() const {
    return ResolvePlayerEntity();
}

void Scene::ToggleCameraMode() {
    SetCameraMode(GetCameraMode() == camera::Mode::Side ? camera::Mode::Isometric : camera::Mode::Side);
}

void Scene::SetCameraMode(camera::Mode mode) {
    camera_.SetMode(mode);
}

camera::Mode Scene::GetCameraMode() const {
    return camera_.GetMode();
}

std::string Scene::CameraModeName() const {
    return GetCameraMode() == camera::Mode::Isometric ? "isometric" : "side";
}

bool Scene::ActionDown(const std::string& action) const {
    return input_ != nullptr && input_->ActionDown(action);
}

void Scene::PlaySound(const std::string& path) {
    if (asset_manager_ != nullptr) {
        asset_manager_->PlaySound(path);
    }
}

void Scene::PlayMusic(const std::string& path, bool loop) {
    if (asset_manager_ != nullptr) {
        asset_manager_->PlayMusic(path, loop);
    }
}

void Scene::StopAllAudio() {
    if (asset_manager_ != nullptr) {
        asset_manager_->StopAllAudio();
    }
}

void Scene::Log(const std::string& message) {
    messages_.push_back(message);
}

void Scene::ScheduleTriggerAction(const assets::TriggerAction& action, const std::string& trigger_name, float delay_seconds) {
    if (debug_trace_enabled_) {
        Trace("Schedule action `" + action.function + "` from trigger `" + trigger_name + "` after " + std::to_string(std::max(delay_seconds, 0.0f)) + "s");
    }
    pending_trigger_actions_.push_back({
        .action = action,
        .trigger_name = trigger_name,
        .remaining_time = std::max(delay_seconds, 0.0f)
    });
}

void Scene::MarkTriggerFired(const std::string& trigger_name) {
    trigger_last_fired_time_[trigger_name] = scene_time_seconds_;
    if (debug_trace_enabled_) {
        Trace("Trigger fired: `" + trigger_name + "`");
    }
}

float Scene::TriggerLastFiredTime(const std::string& trigger_name) const {
    const auto it = trigger_last_fired_time_.find(trigger_name);
    return it != trigger_last_fired_time_.end() ? it->second : -1.0f;
}

float Scene::TimeSeconds() const {
    return scene_time_seconds_;
}

const std::vector<std::string>& Scene::Messages() const {
    return messages_;
}

void Scene::ClearMessages() {
    messages_.clear();
}

void Scene::SetDebugTraceEnabled(bool enabled) {
    debug_trace_enabled_ = enabled;
}

bool Scene::DebugTraceEnabled() const {
    return debug_trace_enabled_;
}

void Scene::Trace(std::string_view message) {
    if (!debug_trace_enabled_) {
        return;
    }
    Log("[Trace] " + std::string(message));
}

assets::LevelData& Scene::EditableLevel() {
    return level_;
}

const assets::LevelData& Scene::Level() const {
    return level_;
}

std::vector<Entity>& Scene::RuntimeEntities() {
    return runtime_entities_;
}

const std::vector<Entity>& Scene::RuntimeEntities() const {
    return runtime_entities_;
}

const std::filesystem::path& Scene::ProjectRoot() const {
    return project_root_;
}

const std::filesystem::path& Scene::LevelPath() const {
    return level_path_;
}

const assets::ProjectData& Scene::Project() const {
    return project_;
}

camera::Camera2D& Scene::Camera() {
    return camera_;
}

const camera::Camera2D& Scene::Camera() const {
    return camera_;
}

void Scene::SetCameraFollowEnabled(bool enabled) {
    camera_follow_enabled_ = enabled;
    if (enabled && camera_follow_target_id_.empty()) {
        camera_follow_target_id_ = "player";
    }
}

bool Scene::CameraFollowEnabled() const {
    return camera_follow_enabled_;
}

void Scene::SetCameraZoom(float zoom) {
    camera_.SetZoom(zoom);
    camera_zoom_target_ = camera_.Zoom();
    camera_zoom_speed_ = 0.0f;
}

void Scene::SetCameraZoomSmooth(float zoom, float speed) {
    camera_zoom_target_ = std::max(zoom, 0.05f);
    camera_zoom_speed_ = std::max(speed, 0.0f);
    if (camera_zoom_speed_ <= 0.0f) {
        camera_.SetZoom(camera_zoom_target_);
    }
}

float Scene::CameraZoom() const {
    return camera_.Zoom();
}

int Scene::AnimatedEntityCount() const {
    int count = 0;
    for (const auto& entity : runtime_entities_) {
        const assets::SpriteAsset* sprite_asset = FindSpriteAsset(entity.sprite_asset);
        if (sprite_asset == nullptr) {
            continue;
        }
        const assets::SpriteAnimation* animation = FindAnimation(*sprite_asset, entity.animation);
        if (animation != nullptr && !animation->frames.empty()) {
            ++count;
        }
    }
    return count;
}

int Scene::ActiveAnimatedEntityCount() const {
    int count = 0;
    for (const auto& entity : runtime_entities_) {
        if (!entity.active) {
            continue;
        }
        const assets::SpriteAsset* sprite_asset = FindSpriteAsset(entity.sprite_asset);
        if (sprite_asset == nullptr) {
            continue;
        }
        const assets::SpriteAnimation* animation = FindAnimation(*sprite_asset, entity.animation);
        if (animation != nullptr && !animation->frames.empty()) {
            ++count;
        }
    }
    return count;
}

int Scene::SceneAnimationCount() const {
    return static_cast<int>(level_.animations.size());
}

int Scene::PlayingSceneAnimationCount() const {
    int count = 0;
    for (const auto& state : runtime_animation_states_) {
        if (state.playing) {
            ++count;
        }
    }
    return count;
}

int Scene::ActiveParticleCount() const {
    int count = 0;
    for (const auto& [_, emitter] : runtime_particle_emitters_) {
        count += static_cast<int>(emitter.particles.size());
    }
    return count;
}

const std::vector<Scene::RenderPassTiming>& Scene::LastRenderPassTimings() const {
    return last_render_pass_timings_;
}

float Scene::LastRenderTotalCpuMs() const {
    return last_render_total_cpu_ms_;
}

glm::vec2 Scene::WorldSize() const {
    if (level_.tile_layers.empty()) {
        return {2048.0f, 1024.0f};
    }
    return {
        static_cast<float>(level_.tile_layers.front().width * level_.tile_width),
        static_cast<float>(level_.tile_layers.front().height * level_.tile_height)
    };
}

assets::AssetManager& Scene::Assets() {
    return *asset_manager_;
}

scripting::PythonScripting& Scene::Scripts() {
    return *scripting_;
}

const assets::SpriteAsset* Scene::FindSpriteAsset(const std::string& path) const {
    const auto it = sprite_assets_.find(path);
    return it != sprite_assets_.end() ? &it->second : nullptr;
}

const assets::ObjectAsset* Scene::FindObjectAsset(const std::string& path) const {
    const auto it = object_assets_.find(path);
    return it != object_assets_.end() ? &it->second : nullptr;
}

const assets::TriggerAsset* Scene::FindTriggerAsset(const std::string& path) const {
    const auto it = trigger_assets_.find(path);
    return it != trigger_assets_.end() ? &it->second : nullptr;
}

assets::LightDefinition* Scene::FindLight(const std::string& name) {
    auto it = std::find_if(level_.lights.begin(), level_.lights.end(), [&](const assets::LightDefinition& light) { return light.name == name; });
    return it != level_.lights.end() ? &(*it) : nullptr;
}

const assets::LightDefinition* Scene::FindLight(const std::string& name) const {
    auto it = std::find_if(level_.lights.begin(), level_.lights.end(), [&](const assets::LightDefinition& light) { return light.name == name; });
    return it != level_.lights.end() ? &(*it) : nullptr;
}

assets::AudioSourceDefinition* Scene::FindAudioSource(const std::string& id) {
    auto it = std::find_if(level_.audio_sources.begin(), level_.audio_sources.end(),
        [&](const assets::AudioSourceDefinition& source) { return source.id == id; });
    return it != level_.audio_sources.end() ? &(*it) : nullptr;
}

const assets::AudioSourceDefinition* Scene::FindAudioSource(const std::string& id) const {
    auto it = std::find_if(level_.audio_sources.begin(), level_.audio_sources.end(),
        [&](const assets::AudioSourceDefinition& source) { return source.id == id; });
    return it != level_.audio_sources.end() ? &(*it) : nullptr;
}

assets::AudioPakDefinition* Scene::FindAudioPak(const std::string& id) {
    auto it = std::find_if(level_.audio_paks.begin(), level_.audio_paks.end(),
        [&](const assets::AudioPakDefinition& pak) { return pak.id == id; });
    return it != level_.audio_paks.end() ? &(*it) : nullptr;
}

const assets::AudioPakDefinition* Scene::FindAudioPak(const std::string& id) const {
    auto it = std::find_if(level_.audio_paks.begin(), level_.audio_paks.end(),
        [&](const assets::AudioPakDefinition& pak) { return pak.id == id; });
    return it != level_.audio_paks.end() ? &(*it) : nullptr;
}

assets::VirtualCameraDefinition* Scene::FindVirtualCamera(const std::string& id) {
    auto it = std::find_if(level_.virtual_cameras.begin(), level_.virtual_cameras.end(),
        [&](const assets::VirtualCameraDefinition& camera) { return MatchesIdOrName(id, camera.id, camera.name); });
    return it != level_.virtual_cameras.end() ? &(*it) : nullptr;
}

const assets::VirtualCameraDefinition* Scene::FindVirtualCamera(const std::string& id) const {
    auto it = std::find_if(level_.virtual_cameras.begin(), level_.virtual_cameras.end(),
        [&](const assets::VirtualCameraDefinition& camera) { return MatchesIdOrName(id, camera.id, camera.name); });
    return it != level_.virtual_cameras.end() ? &(*it) : nullptr;
}

assets::SceneAnimationDefinition* Scene::FindSceneAnimation(const std::string& id) {
    auto it = std::find_if(level_.animations.begin(), level_.animations.end(),
        [&](const assets::SceneAnimationDefinition& animation) { return MatchesIdOrName(id, animation.id, animation.name); });
    return it != level_.animations.end() ? &(*it) : nullptr;
}

const assets::SceneAnimationDefinition* Scene::FindSceneAnimation(const std::string& id) const {
    auto it = std::find_if(level_.animations.begin(), level_.animations.end(),
        [&](const assets::SceneAnimationDefinition& animation) { return MatchesIdOrName(id, animation.id, animation.name); });
    return it != level_.animations.end() ? &(*it) : nullptr;
}

const assets::ObjectAnimationAsset* Scene::FindObjectAnimationAsset(const std::string& path) const {
    const auto it = animation_assets_.find(path);
    return it != animation_assets_.end() ? &it->second : nullptr;
}

const assets::ParticleEffectAsset* Scene::FindParticleEffectAsset(const std::string& path) const {
    if (const auto it = particle_assets_.find(path); it != particle_assets_.end()) {
        return &it->second;
    }
    auto it = std::find_if(particle_assets_.begin(), particle_assets_.end(),
        [&](const auto& entry) { return entry.second.name == path; });
    return it != particle_assets_.end() ? &it->second : nullptr;
}

bool Scene::ActivateVirtualCamera(const std::string& id_or_name) {
    for (int i = 0; i < static_cast<int>(level_.virtual_cameras.size()); ++i) {
        const auto& camera = level_.virtual_cameras[static_cast<std::size_t>(i)];
        if (!camera.enabled || !MatchesIdOrName(id_or_name, camera.id, camera.name)) {
            continue;
        }
        if (active_virtual_camera_index_ != i && camera.override_mode) {
            camera_mode_before_virtual_camera_ = GetCameraMode();
        }
        active_virtual_camera_index_ = i;
        if (debug_trace_enabled_) {
            Trace("Activate virtual camera `" + camera.id + "` target=`" + camera.follow_target + "`");
        }
        return true;
    }
    return false;
}

void Scene::ReleaseVirtualCamera() {
    if (active_virtual_camera_index_ >= 0 &&
        active_virtual_camera_index_ < static_cast<int>(level_.virtual_cameras.size()) &&
        level_.virtual_cameras[static_cast<std::size_t>(active_virtual_camera_index_)].override_mode) {
        SetCameraMode(camera_mode_before_virtual_camera_);
    }
    if (debug_trace_enabled_ && active_virtual_camera_index_ >= 0 &&
        active_virtual_camera_index_ < static_cast<int>(level_.virtual_cameras.size())) {
        Trace("Release virtual camera `" + level_.virtual_cameras[static_cast<std::size_t>(active_virtual_camera_index_)].id + "`");
    }
    active_virtual_camera_index_ = -1;
}

bool Scene::HasActiveVirtualCamera() const {
    return active_virtual_camera_index_ >= 0 &&
           active_virtual_camera_index_ < static_cast<int>(level_.virtual_cameras.size());
}

std::string Scene::ActiveVirtualCameraId() const {
    if (!HasActiveVirtualCamera()) {
        return {};
    }
    return level_.virtual_cameras[static_cast<std::size_t>(active_virtual_camera_index_)].id;
}

bool Scene::PlaySceneAnimation(const std::string& id_or_name, bool restart) {
    for (int i = 0; i < static_cast<int>(level_.animations.size()); ++i) {
        const auto& animation = level_.animations[static_cast<std::size_t>(i)];
        if (!animation.enabled || !MatchesIdOrName(id_or_name, animation.id, animation.name)) {
            continue;
        }
        auto& state = runtime_animation_states_[static_cast<std::size_t>(i)];
        if (restart) {
            state.time = 0.0f;
            state.completed = false;
            state.captured = false;
        }
        state.playing = true;
        return true;
    }
    return false;
}

bool Scene::StopSceneAnimation(const std::string& id_or_name, bool restore_state) {
    for (int i = 0; i < static_cast<int>(level_.animations.size()); ++i) {
        const auto& animation = level_.animations[static_cast<std::size_t>(i)];
        if (!MatchesIdOrName(id_or_name, animation.id, animation.name)) {
            continue;
        }
        auto& state = runtime_animation_states_[static_cast<std::size_t>(i)];
        if (restore_state) {
            if (Entity* entity = FindEntity(animation.target_entity); entity != nullptr && state.captured) {
                entity->position = state.base_position;
                entity->size = state.base_size;
                entity->tint = state.base_tint;
                entity->rotation = state.base_rotation;
                entity->skew = state.base_skew;
            }
        }
        state.playing = false;
        state.completed = false;
        state.time = 0.0f;
        state.captured = false;
        return true;
    }
    return false;
}

bool Scene::PlayParticleEmitter(const std::string& id_or_name, bool restart) {
    for (auto& [_, emitter] : runtime_particle_emitters_) {
        if (!MatchesIdOrName(id_or_name, emitter.id, emitter.id)) {
            continue;
        }
        emitter.enabled = true;
        emitter.playing = true;
        emitter.burst_started = false;
        emitter.spawn_accumulator = 0.0f;
        emitter.pending_burst_count = 0;
        if (restart) {
            emitter.particles.clear();
        }
        if (const assets::ParticleEffectAsset* effect = FindParticleEffectAsset(emitter.asset_path);
            effect != nullptr && !effect->sound.empty()) {
            PlaySound(effect->sound);
        }
        return true;
    }
    return false;
}

bool Scene::StopParticleEmitter(const std::string& id_or_name) {
    for (auto& [_, emitter] : runtime_particle_emitters_) {
        if (!MatchesIdOrName(id_or_name, emitter.id, emitter.id)) {
            continue;
        }
        emitter.playing = false;
        emitter.enabled = false;
        emitter.spawn_accumulator = 0.0f;
        emitter.pending_burst_count = 0;
        return true;
    }
    return false;
}

bool Scene::BurstParticleEmitter(const std::string& id_or_name, int override_count) {
    for (auto& [_, emitter] : runtime_particle_emitters_) {
        if (!MatchesIdOrName(id_or_name, emitter.id, emitter.id)) {
            continue;
        }
        emitter.enabled = true;
        emitter.playing = false;
        emitter.burst_started = true;
        emitter.pending_burst_count = override_count;
        if (const assets::ParticleEffectAsset* effect = FindParticleEffectAsset(emitter.asset_path);
            effect != nullptr && !effect->sound.empty()) {
            PlaySound(effect->sound);
        }
        return true;
    }
    return false;
}

void Scene::SpawnParticleBurstAt(const std::string& asset_path, glm::vec2 position, int count) {
    if (asset_path.empty()) {
        return;
    }
    const assets::ParticleEffectAsset* effect = FindParticleEffectAsset(asset_path);
    if (effect == nullptr) {
        return;
    }

    RuntimeParticleEmitterState emitter;
    emitter.id = "__burst_" + std::to_string(runtime_particle_emitters_.size()) + "_" +
                 std::to_string(static_cast<int>(scene_time_seconds_ * 1000.0f));
    emitter.asset_path = asset_path;
    emitter.enabled = true;
    emitter.autoplay = false;
    emitter.playing = false;
    emitter.burst_on_start = false;
    emitter.burst_started = false;
    emitter.transient = true;
    emitter.transient_position = position;
    emitter.pending_burst_count = count;
    runtime_particle_emitters_[emitter.id] = std::move(emitter);

    if (!effect->sound.empty()) {
        PlaySound(effect->sound);
    }
}

void Scene::StepFixed(float delta_time) {
    for (auto& entity : runtime_entities_) {
        if (!entity.active) {
            continue;
        }
        entity.previous_position = entity.position;
        entity.was_grounded = entity.grounded;

        if (scripting_ != nullptr && !entity.script.empty()) {
            if (debug_trace_enabled_) {
                Trace("Entity update `" + entity.id + "` script=`" + entity.script + "`");
            }
            scripting_->CallEntityUpdate(entity, *this, delta_time);
        }
        if (!entity.attached_to.empty()) {
            continue;
        }
        if (entity.velocity.x > 0.01f) {
            entity.facing_right = true;
        } else if (entity.velocity.x < -0.01f) {
            entity.facing_right = false;
        }
        physics_.Step(entity, level_, delta_time);
    }

    ResolveEntityCollisions();
    UpdateAttachments();
    for (auto& entity : runtime_entities_) {
        if (!entity.active || entity.attached_to.size() > 0) {
            continue;
        }
        if (entity.was_grounded || !entity.grounded || !entity.properties.is_object()) {
            continue;
        }

        const std::string landing_sound = entity.properties.value("landing_sound", std::string{});
        if (!landing_sound.empty()) {
            PlaySound(landing_sound);
        }

        const std::string landing_particle_asset = entity.properties.value("landing_particle_asset", std::string{});
        if (!landing_particle_asset.empty()) {
            glm::vec2 burst_position = entity.position + glm::vec2(entity.size.x * 0.5f, entity.size.y);
            burst_position.y += entity.properties.value("landing_particle_offset_y", -2.0f);
            SpawnParticleBurstAt(
                landing_particle_asset,
                burst_position,
                entity.properties.value("landing_particle_count", -1));
        }
    }
    trigger_system_.Update(*this, level_.triggers);
}

void Scene::RebuildRuntimeEntities() {
    runtime_entities_.clear();
    for (std::size_t i = 0; i < level_.parallax_layers.size(); ++i) {
        auto& layer = level_.parallax_layers[i];
        if (layer.id.empty()) {
            layer.id = "parallax_" + std::to_string(i);
        }
        if (layer.name.empty()) {
            layer.name = layer.id;
        }
    }
    for (const auto& definition : level_.entities) {
        Entity entity;
        entity.id = definition.id;
        entity.archetype = definition.archetype;
        entity.object_asset = definition.object_asset;
        entity.sprite_asset = definition.sprite_asset;
        entity.animation = definition.animation;
        entity.position = definition.position;
        entity.previous_position = definition.position;
        entity.size = definition.size;
        entity.velocity = definition.velocity;
        entity.tint = definition.tint;
        entity.texture = definition.texture;
        entity.sound = definition.sound;
        entity.uv = definition.uv;
        entity.rotation = 0.0f;
        entity.skew = {0.0f, 0.0f};
        entity.layer = definition.layer;
        entity.dynamic = definition.dynamic;
        entity.collidable = definition.collidable;
        entity.visible = definition.visible;
        entity.reflection = definition.reflection;
        entity.normal_map = definition.normal_map;
        entity.height_map = definition.height_map;
        entity.displacement_map = definition.displacement_map;
        entity.relief_enabled = definition.relief_enabled;
        entity.bump_strength = definition.bump_strength;
        entity.relief_depth = definition.relief_depth;
        entity.parallax_depth = definition.parallax_depth;
        entity.relief_contrast = definition.relief_contrast;
        entity.pseudo_3d = definition.pseudo_3d;
        entity.collider_offset = definition.collider_offset;
        entity.collider_size = definition.collider_size;
        entity.pseudo_3d_height = definition.pseudo_3d_height;
        entity.animation_speed = definition.animation_speed;
        entity.script = definition.script;
        entity.on_trigger = definition.on_trigger;
        entity.attached_to = definition.attached_to;
        entity.attach_offset = definition.attach_offset;
        entity.properties = definition.properties;

        if (const assets::ObjectAsset* object_asset = FindObjectAsset(entity.object_asset); object_asset != nullptr) {
            const std::string resolved_preset = !object_asset->preset.empty() ? object_asset->preset : "none";
            const std::string resolved_archetype = resolved_preset != "none" ? resolved_preset : object_asset->name;
            if (entity.archetype.empty() || entity.archetype == "generic" || entity.archetype == object_asset->name) {
                entity.archetype = resolved_archetype;
            }
            if (entity.sprite_asset.empty()) {
                entity.sprite_asset = object_asset->sprite;
            }
            if (definition.animation == "idle" && object_asset->default_animation != "idle") {
                entity.animation = object_asset->default_animation;
            }
            if (Vec2Equals(entity.size, {32.0f, 32.0f}) && !Vec2Equals(object_asset->size, {32.0f, 32.0f})) {
                entity.size = object_asset->size;
            }
            entity.dynamic = entity.dynamic || object_asset->dynamic;
            entity.collidable = entity.collidable || object_asset->collidable;
            entity.reflection = std::max(entity.reflection, object_asset->reflection);
            if (entity.normal_map.empty()) {
                entity.normal_map = object_asset->normal_map;
            }
            if (entity.height_map.empty()) {
                entity.height_map = object_asset->height_map;
            }
            if (entity.displacement_map.empty()) {
                entity.displacement_map = object_asset->displacement_map;
            }
            entity.relief_enabled = entity.relief_enabled || object_asset->relief_enabled;
            if (std::abs(entity.bump_strength - 1.0f) < 0.001f && std::abs(object_asset->bump_strength - 1.0f) > 0.001f) {
                entity.bump_strength = object_asset->bump_strength;
            }
            if (std::abs(entity.relief_depth - 0.035f) < 0.0001f && std::abs(object_asset->relief_depth - 0.035f) > 0.0001f) {
                entity.relief_depth = object_asset->relief_depth;
            }
            if (std::abs(entity.parallax_depth - 0.018f) < 0.0001f && std::abs(object_asset->parallax_depth - 0.018f) > 0.0001f) {
                entity.parallax_depth = object_asset->parallax_depth;
            }
            if (std::abs(entity.relief_contrast - 1.35f) < 0.001f && std::abs(object_asset->relief_contrast - 1.35f) > 0.001f) {
                entity.relief_contrast = object_asset->relief_contrast;
            }
            entity.pseudo_3d = entity.pseudo_3d || object_asset->pseudo_3d;
            if (Vec2Equals(entity.collider_offset, {0.0f, 0.0f})) {
                entity.collider_offset = object_asset->collider_offset;
            }
            if (Vec2Equals(entity.collider_size, {32.0f, 32.0f}) && !Vec2Equals(object_asset->collider_size, {32.0f, 32.0f})) {
                entity.collider_size = object_asset->collider_size;
            }
            if (std::abs(entity.pseudo_3d_height - 16.0f) < 0.001f && std::abs(object_asset->pseudo_3d_height - 16.0f) > 0.001f) {
                entity.pseudo_3d_height = object_asset->pseudo_3d_height;
            }
            if (entity.script.empty()) {
                entity.script = object_asset->script;
            }
            if (entity.sound.empty()) {
                entity.sound = object_asset->sound;
            }
            entity.layer = object_asset->layer;
            entity.tint *= object_asset->tint;
            MergeJsonDefaults(entity.properties, object_asset->properties);
        }

        entity.reflection = std::clamp(entity.reflection, 0.0f, 1.5f);
        entity.bump_strength = std::clamp(entity.bump_strength, 0.0f, 4.0f);
        entity.relief_depth = std::clamp(entity.relief_depth, 0.0f, 0.12f);
        entity.parallax_depth = std::clamp(entity.parallax_depth, 0.0f, 0.08f);
        entity.relief_contrast = std::clamp(entity.relief_contrast, 0.2f, 4.0f);
        entity.pseudo_3d_height = std::max(entity.pseudo_3d_height, 0.0f);

        if (const assets::SpriteAsset* sprite_asset = FindSpriteAsset(entity.sprite_asset); sprite_asset != nullptr) {
            if (entity.animation.empty()) {
                entity.animation = sprite_asset->default_animation;
            }
            if (const assets::SpriteAnimation* animation = FindAnimation(*sprite_asset, entity.animation); animation != nullptr && !animation->frames.empty()) {
                entity.animation_frame = std::clamp(entity.animation_frame, 0, static_cast<int>(animation->frames.size()) - 1);
                entity.texture = animation->frames[entity.animation_frame].texture;
                entity.tint *= sprite_asset->tint;
            }
        }

        runtime_entities_.push_back(std::move(entity));
    }

    UpdateAttachments();
    for (auto& trigger : level_.triggers) {
        if (trigger.id.empty()) {
            trigger.id = "trigger_" + std::to_string(&trigger - level_.triggers.data());
        }
        if (trigger.name.empty()) {
            trigger.name = trigger.id;
        }
        if (const assets::TriggerAsset* trigger_asset = FindTriggerAsset(trigger.asset); trigger_asset != nullptr) {
            if (Vec2Equals(trigger.size, {64.0f, 64.0f})) {
                trigger.size = trigger_asset->default_size;
            }
            if (Vec4Equals(trigger.color, {1.0f, 0.7f, 0.1f, 1.0f})) {
                trigger.color = trigger_asset->color;
            }
            trigger.once = trigger.once || trigger_asset->once;
            trigger.enabled = trigger.enabled && trigger_asset->enabled;
            if (trigger.conditions.empty()) {
                trigger.conditions = trigger_asset->conditions;
            }
            if (trigger.actions.empty()) {
                trigger.actions = trigger_asset->actions;
            }
        }
        trigger.fired = false;
    }

    for (std::size_t i = 0; i < level_.lights.size(); ++i) {
        auto& light = level_.lights[i];
        if (light.id.empty()) {
            light.id = "light_" + std::to_string(i);
        }
        if (light.name.empty()) {
            light.name = light.id;
        }
        light.radius = std::max(light.radius, 8.0f);
        light.source_radius = std::clamp(light.source_radius, 0.0f, light.radius);
        light.scatter = std::clamp(light.scatter, 0.2f, 2.8f);
        light.intensity = std::max(light.intensity, 0.0f);
    }

    for (std::size_t i = 0; i < level_.audio_sources.size(); ++i) {
        auto& source = level_.audio_sources[i];
        if (source.id.empty()) {
            source.id = "audio_source_" + std::to_string(i);
        }
        if (source.name.empty()) {
            source.name = source.id;
        }
        source.radius = std::max(source.radius, 1.0f);
        source.distance = std::max(source.distance, source.radius);
        source.volume = std::clamp(source.volume, 0.0f, 1.0f);
        audio_source_states_[source.id] = {};
    }

    for (std::size_t i = 0; i < level_.audio_paks.size(); ++i) {
        auto& pak = level_.audio_paks[i];
        if (pak.id.empty()) {
            pak.id = "audio_pak_" + std::to_string(i);
        }
        if (pak.name.empty()) {
            pak.name = pak.id;
        }
        pak.radius = std::max(pak.radius, 1.0f);
        pak.distance = std::max(pak.distance, pak.radius);
        pak.volume = std::clamp(pak.volume, 0.0f, 1.0f);
        audio_pak_states_[pak.id] = {};
    }

    for (std::size_t i = 0; i < level_.virtual_cameras.size(); ++i) {
        auto& camera = level_.virtual_cameras[i];
        if (camera.id.empty()) {
            camera.id = "vcam_" + std::to_string(i);
        }
        if (camera.name.empty()) {
            camera.name = camera.id;
        }
        camera.size.x = std::max(camera.size.x, 64.0f);
        camera.size.y = std::max(camera.size.y, 64.0f);
        camera.zoom = std::max(camera.zoom, 0.05f);
        camera.follow_lag = std::max(camera.follow_lag, 0.0f);
        camera.zoom_lag = std::max(camera.zoom_lag, 0.0f);
    }

    level_.player_camera.zoom = std::max(level_.player_camera.zoom, 0.05f);
    level_.player_camera.follow_lag = std::max(level_.player_camera.follow_lag, 0.0f);
    level_.player_camera.zoom_lag = std::max(level_.player_camera.zoom_lag, 0.0f);
    level_.player_camera.dead_zone.x = std::max(level_.player_camera.dead_zone.x, 0.0f);
    level_.player_camera.dead_zone.y = std::max(level_.player_camera.dead_zone.y, 0.0f);

    runtime_animation_states_.clear();
    runtime_animation_states_.resize(level_.animations.size());
    for (std::size_t i = 0; i < level_.animations.size(); ++i) {
        auto& animation = level_.animations[i];
        if (animation.id.empty()) {
            animation.id = "scene_animation_" + std::to_string(i);
        }
        if (animation.name.empty()) {
            animation.name = animation.id;
        }
        animation.speed = std::max(animation.speed, 0.01f);
        auto& state = runtime_animation_states_[i];
        state.id = animation.id;
        state.playing = animation.enabled && animation.play_on_start;
        state.completed = false;
    }

    runtime_particle_emitters_.clear();
    for (const auto& entity : runtime_entities_) {
        if (!IsParticleEmitterEntity(entity)) {
            continue;
        }
        RuntimeParticleEmitterState emitter;
        emitter.id = entity.id;
        emitter.asset_path = ParticleEmitterAssetPath(entity.properties);
        emitter.enabled = JsonBoolValue(entity.properties, "emitter_enabled", true);
        emitter.autoplay = JsonBoolValue(entity.properties, "particle_autoplay", true);
        emitter.playing = emitter.enabled && emitter.autoplay;
        emitter.burst_on_start = JsonBoolValue(entity.properties, "burst_on_start", false);
        runtime_particle_emitters_[emitter.id] = std::move(emitter);
    }

    for (auto& entity : runtime_entities_) {
        if (scripting_ != nullptr && !entity.script.empty()) {
            scripting_->CallEntitySpawn(entity, *this);
        }
    }

    UpdateAttachments();
    UpdateAnimations(0.0f);
}

void Scene::LoadResourceLibraries() {
    sprite_assets_.clear();
    object_assets_.clear();
    trigger_assets_.clear();
    animation_assets_.clear();
    particle_assets_.clear();

    for (const auto& relative : GatherResources(project_root_, project_.sprite_assets, project_root_ / "resources/sprites", ".nsprite")) {
        try {
            sprite_assets_[relative] = assets::LoadSpriteAsset(project_root_ / relative);
        } catch (...) {
        }
    }

    for (const auto& relative : GatherResources(project_root_, project_.object_assets, project_root_ / "resources/objects", ".nobject")) {
        try {
            object_assets_[relative] = assets::LoadObjectAsset(project_root_ / relative);
        } catch (...) {
        }
    }

    for (const auto& relative : GatherResources(project_root_, project_.trigger_assets, project_root_ / "resources/triggers", ".ntrigger")) {
        try {
            trigger_assets_[relative] = assets::LoadTriggerAsset(project_root_ / relative);
        } catch (...) {
        }
    }

    for (const auto& relative : GatherResources(project_root_, project_.animation_assets, project_root_ / "resources/animations", ".nanim")) {
        try {
            animation_assets_[relative] = assets::LoadObjectAnimationAsset(project_root_ / relative);
        } catch (...) {
        }
    }

    for (const auto& relative : GatherResources(project_root_, project_.particle_assets, project_root_ / "resources/particles", ".nparticle")) {
        try {
            particle_assets_[relative] = assets::LoadParticleEffectAsset(project_root_ / relative);
        } catch (...) {
        }
    }
}

void Scene::UpdateAnimations(float delta_time) {
    auto update_range = [&](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            auto& entity = runtime_entities_[index];
            const assets::SpriteAsset* sprite_asset = FindSpriteAsset(entity.sprite_asset);
            if (sprite_asset == nullptr) {
                continue;
            }

            UpdateAutomaticAnimation(entity, *sprite_asset);
            const assets::SpriteAnimation* animation = FindAnimation(*sprite_asset, entity.animation);
            if (animation == nullptr || animation->frames.empty()) {
                continue;
            }

            entity.animation_frame = std::clamp(entity.animation_frame, 0, static_cast<int>(animation->frames.size()) - 1);
            const float speed = std::max(entity.animation_speed, 0.01f);
            entity.animation_time += delta_time * speed;

            while (entity.animation_time >= std::max(animation->frames[entity.animation_frame].duration, 0.01f)) {
                entity.animation_time -= std::max(animation->frames[entity.animation_frame].duration, 0.01f);
                ++entity.animation_frame;
                if (entity.animation_frame >= static_cast<int>(animation->frames.size())) {
                    entity.animation_frame = animation->loop ? 0 : static_cast<int>(animation->frames.size()) - 1;
                    if (!animation->loop) {
                        entity.animation_time = 0.0f;
                        break;
                    }
                }
            }

            entity.texture = animation->frames[entity.animation_frame].texture;
        }
    };

    if (project_.multithreading_enabled && core::ThreadPool::Shared().Enabled() && runtime_entities_.size() >= 24) {
        core::ThreadPool::Shared().ParallelFor(runtime_entities_.size(), update_range, 8);
    } else {
        update_range(0, runtime_entities_.size());
    }
}

void Scene::UpdatePlayerCamera(float delta_time) {
    if (!camera_follow_enabled_ || active_virtual_camera_index_ >= 0) {
        return;
    }

    const auto& player_camera = level_.player_camera;
    if (!player_camera.enabled) {
        Entity* target = ResolveEntityTarget(camera_follow_target_id_);
        if (target != nullptr) {
            camera_.SetPosition(target->position + target->size * 0.5f);
        }
        return;
    }

    if (player_camera.override_mode) {
        SetCameraMode(player_camera.camera_mode == "isometric" ? camera::Mode::Isometric : camera::Mode::Side);
    }

    const Entity* target = ResolveEntityTarget(player_camera.follow_target.empty() ? camera_follow_target_id_ : player_camera.follow_target);
    if (debug_trace_enabled_) {
        Trace("Player camera follow_target=`" + player_camera.follow_target + "` resolved=`" +
              (target != nullptr ? target->id : std::string("none")) + "`");
    }

    glm::vec2 target_point = target != nullptr
        ? (target->position + target->size * 0.5f)
        : camera_.Position();
    target_point += player_camera.follow_offset;

    glm::vec2 desired = camera_.Position();
    const glm::vec2 current = camera_.Position();
    const glm::vec2 delta = target_point - current;
    const glm::vec2 dead_zone{
        std::max(player_camera.dead_zone.x, 0.0f),
        std::max(player_camera.dead_zone.y, 0.0f)
    };
    desired.x = std::abs(delta.x) > dead_zone.x * 0.5f
        ? (target_point.x - std::copysign(dead_zone.x * 0.5f, delta.x))
        : current.x;
    desired.y = std::abs(delta.y) > dead_zone.y * 0.5f
        ? (target_point.y - std::copysign(dead_zone.y * 0.5f, delta.y))
        : current.y;

    const float desired_zoom = std::max(player_camera.zoom, 0.05f);
    if (player_camera.zoom_lag > 0.0f) {
        const float current_zoom = camera_.Zoom();
        const float zoom_blend = std::clamp(delta_time * player_camera.zoom_lag, 0.0f, 1.0f);
        camera_.SetZoom(current_zoom + (desired_zoom - current_zoom) * zoom_blend);
    } else {
        camera_.SetZoom(desired_zoom);
    }

    if (player_camera.clamp_to_world) {
        const glm::vec2 world_size = WorldSize();
        const glm::vec2 half_view = camera_.Viewport() * 0.5f / std::max(camera_.Zoom(), 0.05f);
        if (world_size.x <= half_view.x * 2.0f) {
            desired.x = world_size.x * 0.5f;
        } else {
            desired.x = std::clamp(desired.x, half_view.x, world_size.x - half_view.x);
        }
        if (world_size.y <= half_view.y * 2.0f) {
            desired.y = world_size.y * 0.5f;
        } else {
            desired.y = std::clamp(desired.y, half_view.y, world_size.y - half_view.y);
        }
    }

    if (player_camera.follow_lag > 0.0f) {
        const float follow_blend = std::clamp(delta_time * player_camera.follow_lag, 0.0f, 1.0f);
        camera_.SetPosition(current + (desired - current) * follow_blend);
    } else {
        camera_.SetPosition(desired);
    }
}

void Scene::UpdateVirtualCamera(float delta_time) {
    auto point_in_rect = [](glm::vec2 point, glm::vec2 position, glm::vec2 size) {
        return point.x >= position.x && point.y >= position.y &&
               point.x <= position.x + size.x && point.y <= position.y + size.y;
    };

    const Entity* focus_entity = ResolveEntityTarget({});
    const glm::vec2 focus_point = focus_entity != nullptr
        ? (focus_entity->position + focus_entity->size * 0.5f)
        : camera_.Position();

    int auto_camera = -1;
    float auto_camera_area = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(level_.virtual_cameras.size()); ++i) {
        const auto& camera = level_.virtual_cameras[static_cast<std::size_t>(i)];
        if (!camera.enabled || !camera.auto_activate) {
            continue;
        }
        if (!point_in_rect(focus_point, camera.position, camera.size)) {
            continue;
        }
        const float area = camera.size.x * camera.size.y;
        if (area < auto_camera_area) {
            auto_camera_area = area;
            auto_camera = i;
        }
    }

    if (HasActiveVirtualCamera()) {
        const auto& active = level_.virtual_cameras[static_cast<std::size_t>(active_virtual_camera_index_)];
        if (!active.enabled) {
            ReleaseVirtualCamera();
        } else if (active.release_on_exit && !point_in_rect(focus_point, active.position, active.size)) {
            if (auto_camera >= 0 && auto_camera != active_virtual_camera_index_) {
                ActivateVirtualCamera(level_.virtual_cameras[static_cast<std::size_t>(auto_camera)].id);
            } else {
                ReleaseVirtualCamera();
            }
        }
    } else if (auto_camera >= 0) {
        ActivateVirtualCamera(level_.virtual_cameras[static_cast<std::size_t>(auto_camera)].id);
    }

    if (!HasActiveVirtualCamera()) {
        return;
    }

    const auto& virtual_camera = level_.virtual_cameras[static_cast<std::size_t>(active_virtual_camera_index_)];
    if (virtual_camera.override_mode) {
        SetCameraMode(virtual_camera.camera_mode == "isometric" ? camera::Mode::Isometric : camera::Mode::Side);
    }

    const Entity* target = ResolveEntityTarget(virtual_camera.follow_target);
    if (debug_trace_enabled_) {
        Trace("Camera `" + virtual_camera.id + "` follow_target=`" + virtual_camera.follow_target + "` resolved=`" +
              (target != nullptr ? target->id : std::string("none")) + "`");
    }
    glm::vec2 target_point = target != nullptr
        ? (target->position + target->size * 0.5f)
        : (virtual_camera.position + virtual_camera.size * 0.5f);
    target_point += virtual_camera.follow_offset;

    glm::vec2 desired = camera_.Position();
    const glm::vec2 current = camera_.Position();
    const glm::vec2 delta = target_point - current;
    const glm::vec2 dead_zone{
        std::max(virtual_camera.dead_zone.x, 0.0f),
        std::max(virtual_camera.dead_zone.y, 0.0f)
    };
    desired.x = std::abs(delta.x) > dead_zone.x * 0.5f
        ? (target_point.x - std::copysign(dead_zone.x * 0.5f, delta.x))
        : current.x;
    desired.y = std::abs(delta.y) > dead_zone.y * 0.5f
        ? (target_point.y - std::copysign(dead_zone.y * 0.5f, delta.y))
        : current.y;

    const float desired_zoom = std::max(virtual_camera.zoom, 0.05f);
    if (virtual_camera.zoom_lag > 0.0f) {
        const float current_zoom = camera_.Zoom();
        const float zoom_blend = std::clamp(delta_time * virtual_camera.zoom_lag, 0.0f, 1.0f);
        camera_.SetZoom(current_zoom + (desired_zoom - current_zoom) * zoom_blend);
    } else {
        camera_.SetZoom(desired_zoom);
    }

    const glm::vec2 half_view = camera_.Viewport() * 0.5f / std::max(camera_.Zoom(), 0.05f);
    const glm::vec2 bounds_min = virtual_camera.position;
    const glm::vec2 bounds_max = virtual_camera.position + virtual_camera.size;
    if (virtual_camera.size.x <= half_view.x * 2.0f) {
        desired.x = virtual_camera.position.x + virtual_camera.size.x * 0.5f;
    } else {
        desired.x = std::clamp(desired.x, bounds_min.x + half_view.x, bounds_max.x - half_view.x);
    }
    if (virtual_camera.size.y <= half_view.y * 2.0f) {
        desired.y = virtual_camera.position.y + virtual_camera.size.y * 0.5f;
    } else {
        desired.y = std::clamp(desired.y, bounds_min.y + half_view.y, bounds_max.y - half_view.y);
    }

    if (virtual_camera.follow_lag > 0.0f) {
        const float follow_blend = std::clamp(delta_time * virtual_camera.follow_lag, 0.0f, 1.0f);
        camera_.SetPosition(current + (desired - current) * follow_blend);
    } else {
        camera_.SetPosition(desired);
    }
}

void Scene::UpdateSceneAnimations(float delta_time) {
    if (runtime_animation_states_.size() != level_.animations.size()) {
        runtime_animation_states_.resize(level_.animations.size());
    }

    for (std::size_t i = 0; i < level_.animations.size(); ++i) {
        const auto& animation = level_.animations[i];
        auto& state = runtime_animation_states_[i];
        if (!animation.enabled) {
            if (state.playing) {
                StopSceneAnimation(animation.id, true);
            }
            continue;
        }

        const assets::ObjectAnimationAsset* asset = FindObjectAnimationAsset(animation.asset);
        Entity* entity = FindEntity(animation.target_entity);
        if (asset == nullptr || entity == nullptr) {
            state.playing = false;
            continue;
        }

        if (!state.captured) {
            state.base_position = entity->position;
            state.base_size = entity->size;
            state.base_tint = entity->tint;
            state.base_rotation = entity->rotation;
            state.base_skew = entity->skew;
            state.captured = true;
        }

        if (!state.playing) {
            continue;
        }

        const float duration = std::max(asset->duration, 0.01f);
        state.time += delta_time * std::max(animation.speed, 0.01f);
        if (state.time >= duration) {
            if (animation.loop) {
                state.time = std::fmod(state.time, duration);
                state.completed = false;
            } else {
                state.time = duration;
                state.playing = false;
                state.completed = true;
            }
        }

        const float normalized = std::clamp(state.time / duration, 0.0f, 1.0f);
        entity->position = state.base_position;
        entity->size = state.base_size;
        entity->tint = state.base_tint;
        entity->rotation = state.base_rotation;
        entity->skew = state.base_skew;

        if (asset->affect_position) {
            entity->position = state.base_position + SamplePath(asset->path_points, normalized);
        }
        if (asset->affect_scale) {
            const glm::vec2 scale = SampleVec2Keys(asset->scale_keys, normalized, {1.0f, 1.0f});
            entity->size = {
                std::max(state.base_size.x * scale.x, 4.0f),
                std::max(state.base_size.y * scale.y, 4.0f)
            };
        }
        if (asset->affect_opacity) {
            entity->tint.a = std::clamp(state.base_tint.a * SampleFloatKeys(asset->opacity_keys, normalized, 1.0f), 0.0f, 1.0f);
        }
        if (asset->affect_rotation) {
            entity->rotation = state.base_rotation + SampleFloatKeys(asset->rotation_keys, normalized, 0.0f);
        }
        if (asset->affect_skew) {
            entity->skew = state.base_skew + SampleVec2Keys(asset->skew_keys, normalized, {0.0f, 0.0f});
        }
    }
}

void Scene::UpdateParticles(float delta_time) {
    if (runtime_particle_emitters_.empty()) {
        return;
    }

    static thread_local std::mt19937 rng{std::random_device{}()};
    auto random_range = [&](float minimum, float maximum) {
        if (maximum < minimum) {
            std::swap(minimum, maximum);
        }
        std::uniform_real_distribution<float> distribution(minimum, maximum);
        return distribution(rng);
    };
    auto emit_particles = [&](RuntimeParticleEmitterState& emitter, glm::vec2 center, const assets::ParticleEffectAsset& effect, int count) {
        if (count <= 0) {
            return;
        }
        emitter.particles.reserve(emitter.particles.size() + static_cast<std::size_t>(count));
        constexpr float kPi = 3.14159265359f;
        for (int index = 0; index < count; ++index) {
            const float radius = std::max(effect.emission_radius, 0.0f) * std::sqrt(random_range(0.0f, 1.0f));
            const float emission_angle = random_range(0.0f, kPi * 2.0f);
            const float velocity_angle = glm::radians(random_range(effect.velocity_angle_min, effect.velocity_angle_max));
            const float speed = std::max(random_range(effect.speed_min, effect.speed_max), 0.0f);
            ParticleInstance particle;
            particle.texture = effect.texture;
            particle.position = center + glm::vec2(std::cos(emission_angle), std::sin(emission_angle)) * radius;
            particle.velocity = glm::vec2(std::cos(velocity_angle), std::sin(velocity_angle)) * speed;
            particle.acceleration = effect.acceleration;
            particle.start_size = effect.start_size;
            particle.end_size = effect.end_size;
            particle.start_color = effect.start_color;
            particle.end_color = effect.end_color;
            particle.age = 0.0f;
            particle.lifetime = std::max(random_range(effect.lifetime_min, effect.lifetime_max), 0.05f);
            particle.rotation = random_range(0.0f, 360.0f);
            particle.angular_velocity = random_range(effect.angular_velocity_min, effect.angular_velocity_max);
            particle.additive = effect.additive;
            particle.align_to_velocity = effect.align_to_velocity;
            emitter.particles.push_back(std::move(particle));
        }
    };

    std::unordered_map<std::string, Entity*> entity_lookup;
    entity_lookup.reserve(runtime_entities_.size());
    for (auto& entity : runtime_entities_) {
        entity_lookup.emplace(entity.id, &entity);
    }

    std::vector<std::pair<std::string, RuntimeParticleEmitterState*>> emitters;
    emitters.reserve(runtime_particle_emitters_.size());
    for (auto& [emitter_id, emitter] : runtime_particle_emitters_) {
        emitters.push_back({emitter_id, &emitter});
    }

    auto update_range = [&](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            const std::string& emitter_id = emitters[index].first;
            RuntimeParticleEmitterState& emitter = *emitters[index].second;
            Entity* entity = nullptr;
            if (const auto found = entity_lookup.find(emitter_id); found != entity_lookup.end()) {
                entity = found->second;
            }
            glm::vec2 emission_center = emitter.transient_position;
            if (!emitter.transient) {
                if (entity == nullptr || !entity->active) {
                    emitter.particles.clear();
                    continue;
                }

                emitter.asset_path = ParticleEmitterAssetPath(entity->properties);
                emitter.enabled = JsonBoolValue(entity->properties, "emitter_enabled", true);
                emitter.autoplay = JsonBoolValue(entity->properties, "particle_autoplay", true);
                emitter.burst_on_start = JsonBoolValue(entity->properties, "burst_on_start", false);
                emission_center = entity->position + entity->size * 0.5f;
            }

            const assets::ParticleEffectAsset* effect = FindParticleEffectAsset(emitter.asset_path);
            if (effect == nullptr) {
                emitter.particles.clear();
                continue;
            }

            if (!emitter.enabled) {
                emitter.playing = false;
            }

            if (emitter.enabled && emitter.burst_on_start && !emitter.burst_started) {
                emit_particles(emitter, emission_center, *effect, std::max(effect->burst_count, 1));
                emitter.burst_started = true;
            }

            if (emitter.enabled && emitter.pending_burst_count != 0) {
                const int burst_count = emitter.pending_burst_count > 0 ? emitter.pending_burst_count : std::max(effect->burst_count, 1);
                emit_particles(emitter, emission_center, *effect, burst_count);
                emitter.pending_burst_count = 0;
            }

            if (emitter.enabled && emitter.autoplay && effect->loop) {
                emitter.playing = true;
            }

            if (emitter.enabled && emitter.playing) {
                if (effect->loop) {
                    emitter.spawn_accumulator += delta_time * std::max(effect->spawn_rate, 0.0f);
                    const int spawn_count = std::clamp(static_cast<int>(std::floor(emitter.spawn_accumulator)), 0, 256);
                    if (spawn_count > 0) {
                        emit_particles(emitter, emission_center, *effect, spawn_count);
                        emitter.spawn_accumulator -= static_cast<float>(spawn_count);
                    }
                } else if (!emitter.burst_started) {
                    emit_particles(emitter, emission_center, *effect, std::max(effect->burst_count, 1));
                    emitter.burst_started = true;
                    emitter.playing = false;
                }
            }

            emitter.particles.erase(
                std::remove_if(emitter.particles.begin(), emitter.particles.end(), [&](ParticleInstance& particle) {
                    particle.age += delta_time;
                    if (particle.age >= particle.lifetime) {
                        return true;
                    }
                    particle.velocity += particle.acceleration * delta_time;
                    const float drag = std::clamp(effect->drag, 0.0f, 8.0f);
                    particle.velocity *= std::max(0.0f, 1.0f - drag * delta_time);
                    particle.position += particle.velocity * delta_time;
                    particle.rotation += particle.angular_velocity * delta_time;
                    return false;
                }),
                emitter.particles.end());
        }
    };

    if (project_.multithreading_enabled && core::ThreadPool::Shared().Enabled() && emitters.size() >= 4) {
        core::ThreadPool::Shared().ParallelFor(emitters.size(), update_range, 1);
    } else {
        update_range(0, emitters.size());
    }

    for (auto it = runtime_particle_emitters_.begin(); it != runtime_particle_emitters_.end();) {
        if (it->second.transient &&
            it->second.particles.empty() &&
            !it->second.playing &&
            it->second.pending_burst_count == 0) {
            it = runtime_particle_emitters_.erase(it);
        } else {
            ++it;
        }
    }
}

void Scene::ResolveEntityCollisions() {
    for (auto& entity : runtime_entities_) {
        if (!entity.active || !entity.dynamic || !entity.attached_to.empty()) {
            continue;
        }

        for (const auto& other : runtime_entities_) {
            if (&entity == &other || !other.active || !other.collidable || (other.dynamic && other.attached_to.empty())) {
                continue;
            }

            const glm::vec2 entity_collider_position = ColliderPosition(entity);
            const glm::vec2 entity_collider_size = ColliderSize(entity);
            const glm::vec2 other_collider_position = ColliderPosition(other);
            const glm::vec2 other_collider_size = ColliderSize(other);
            if (!Intersects(entity_collider_position, entity_collider_size, other_collider_position, other_collider_size)) {
                continue;
            }

            const glm::vec2 previous_collider_position = entity.previous_position + entity.collider_offset;
            if (previous_collider_position.y + entity_collider_size.y <= other_collider_position.y + 1.0f) {
                entity.position.y = other_collider_position.y - entity_collider_size.y - entity.collider_offset.y;
                entity.velocity.y = 0.0f;
                entity.grounded = true;
            } else if (previous_collider_position.y >= other_collider_position.y + other_collider_size.y - 1.0f) {
                entity.position.y = other_collider_position.y + other_collider_size.y - entity.collider_offset.y;
                entity.velocity.y = 0.0f;
            } else if (previous_collider_position.x + entity_collider_size.x <= other_collider_position.x + 1.0f) {
                entity.position.x = other_collider_position.x - entity_collider_size.x - entity.collider_offset.x;
                entity.velocity.x = 0.0f;
            } else if (previous_collider_position.x >= other_collider_position.x + other_collider_size.x - 1.0f) {
                entity.position.x = other_collider_position.x + other_collider_size.x - entity.collider_offset.x;
                entity.velocity.x = 0.0f;
            } else {
                const float overlap_left = (entity_collider_position.x + entity_collider_size.x) - other_collider_position.x;
                const float overlap_right = (other_collider_position.x + other_collider_size.x) - entity_collider_position.x;
                const float overlap_top = (entity_collider_position.y + entity_collider_size.y) - other_collider_position.y;
                const float overlap_bottom = (other_collider_position.y + other_collider_size.y) - entity_collider_position.y;

                const float minimum_x = std::min(overlap_left, overlap_right);
                const float minimum_y = std::min(overlap_top, overlap_bottom);
                if (minimum_x < minimum_y) {
                    entity.position.x += overlap_left < overlap_right ? -minimum_x : minimum_x;
                    entity.velocity.x = 0.0f;
                } else {
                    entity.position.y += overlap_top < overlap_bottom ? -minimum_y : minimum_y;
                    entity.velocity.y = 0.0f;
                    entity.grounded = overlap_top < overlap_bottom;
                }
            }
        }
    }
}

void Scene::UpdateAttachments() {
    for (auto& entity : runtime_entities_) {
        if (entity.attached_to.empty()) {
            continue;
        }

        const Entity* parent = FindEntity(entity.attached_to);
        if (parent == nullptr || parent == &entity) {
            continue;
        }

        entity.position = ResolveAttachedEntityPosition(entity);
        entity.previous_position = entity.position;
        entity.velocity = parent->velocity;
        entity.grounded = parent->grounded;
        entity.active = parent->active;
        if (std::abs(parent->velocity.x) > 0.01f) {
            entity.facing_right = parent->facing_right;
        }
    }
}

void Scene::UpdateAudio(float) {
    if (asset_manager_ == nullptr) {
        return;
    }

    const Entity* listener_entity = ResolveEntityTarget({});
    const glm::vec2 listener = listener_entity != nullptr
        ? (listener_entity->position + listener_entity->size * 0.5f)
        : camera_.Position();

    auto attenuation = [&](glm::vec2 position, float max_distance, float base_volume) {
        const float range = std::max(max_distance, 1.0f);
        const float distance = Distance(listener, position);
        const float normalized = std::clamp(distance / range, 0.0f, 1.0f);
        const float gain = (1.0f - normalized) * std::clamp(base_volume, 0.0f, 1.0f);
        return std::pair<float, float>{gain, normalized};
    };

    for (const auto& source : level_.audio_sources) {
        auto& state = audio_source_states_[source.id];
        const bool channel_playing = asset_manager_->IsChannelPlaying(state.channel);
        if (!channel_playing) {
            state.channel = -1;
        }

        if (!source.enabled || source.audio.empty()) {
            if (state.channel >= 0) {
                asset_manager_->HaltChannel(state.channel);
                state.channel = -1;
            }
            state.was_active = false;
            continue;
        }

        const float trigger_distance = Distance(listener, source.position);
        const bool active_now = source.always || trigger_distance <= source.radius;
        const bool should_start = active_now && (!state.was_active || (source.always && source.loop && state.channel < 0));
        if (should_start) {
            state.channel = asset_manager_->PlaySoundChannel(source.audio, source.loop ? -1 : 0, source.volume);
        }

        if (state.channel >= 0) {
            const auto [gain, normalized] = attenuation(source.position, source.distance, source.volume);
            asset_manager_->SetChannelVolume(state.channel, gain);
            asset_manager_->SetChannelDistance(state.channel, normalized);
            if (!active_now && source.stop_on_exit) {
                asset_manager_->HaltChannel(state.channel);
                state.channel = -1;
            }
        }

        state.was_active = active_now;
    }

    auto next_track_index = [](const assets::AudioPakDefinition& pak, int current_track) {
        if (pak.tracks.empty()) {
            return -1;
        }
        if (pak.shuffle) {
            if (pak.tracks.size() == 1) {
                return 0;
            }
            int choice = current_track;
            while (choice == current_track) {
                choice = std::rand() % static_cast<int>(pak.tracks.size());
            }
            return choice;
        }
        if (current_track < 0) {
            return 0;
        }
        const int next = current_track + 1;
        if (next < static_cast<int>(pak.tracks.size())) {
            return next;
        }
        return pak.repeat_playlist ? 0 : -1;
    };

    for (const auto& pak : level_.audio_paks) {
        auto& state = audio_pak_states_[pak.id];
        const bool channel_playing = asset_manager_->IsChannelPlaying(state.channel);
        if (!channel_playing) {
            state.channel = -1;
        }

        if (!pak.enabled || pak.tracks.empty()) {
            if (state.channel >= 0) {
                asset_manager_->HaltChannel(state.channel);
                state.channel = -1;
            }
            state.was_active = false;
            state.current_track = -1;
            state.finished = false;
            continue;
        }

        const float trigger_distance = Distance(listener, pak.position);
        const bool active_now = pak.always || trigger_distance <= pak.radius;
        if (!active_now && pak.stop_on_exit && state.channel >= 0) {
            asset_manager_->HaltChannel(state.channel);
            state.channel = -1;
        }

        if (active_now) {
            if (state.channel < 0) {
                int track_index = state.current_track;
                if (track_index < 0 || !state.was_active) {
                    track_index = next_track_index(pak, -1);
                } else if (!asset_manager_->IsChannelPlaying(state.channel)) {
                    if (pak.loop && state.current_track >= 0) {
                        track_index = state.current_track;
                    } else {
                        track_index = next_track_index(pak, state.current_track);
                    }
                }

                if (track_index >= 0 && track_index < static_cast<int>(pak.tracks.size())) {
                    state.current_track = track_index;
                    state.channel = asset_manager_->PlaySoundChannel(pak.tracks[static_cast<std::size_t>(track_index)], 0, pak.volume);
                    state.finished = state.channel < 0;
                } else {
                    state.finished = true;
                }
            }

            if (state.channel >= 0) {
                const auto [gain, normalized] = attenuation(pak.position, pak.distance, pak.volume);
                asset_manager_->SetChannelVolume(state.channel, gain);
                asset_manager_->SetChannelDistance(state.channel, normalized);
            }
        }

        state.was_active = active_now;
    }
}

Entity* Scene::ResolvePlayerEntity() {
    auto exact_it = std::find_if(runtime_entities_.begin(), runtime_entities_.end(), [](const Entity& entity) {
        return ToLowerCopy(entity.id) == "player";
    });
    if (exact_it != runtime_entities_.end()) {
        return &(*exact_it);
    }
    auto player_it = std::find_if(runtime_entities_.begin(), runtime_entities_.end(), [](const Entity& entity) {
        return EntityLooksLikePlayer(entity);
    });
    if (player_it != runtime_entities_.end()) {
        return &(*player_it);
    }
    return runtime_entities_.empty() ? nullptr : &runtime_entities_.front();
}

const Entity* Scene::ResolvePlayerEntity() const {
    auto exact_it = std::find_if(runtime_entities_.begin(), runtime_entities_.end(), [](const Entity& entity) {
        return ToLowerCopy(entity.id) == "player";
    });
    if (exact_it != runtime_entities_.end()) {
        return &(*exact_it);
    }
    auto player_it = std::find_if(runtime_entities_.begin(), runtime_entities_.end(), [](const Entity& entity) {
        return EntityLooksLikePlayer(entity);
    });
    if (player_it != runtime_entities_.end()) {
        return &(*player_it);
    }
    return runtime_entities_.empty() ? nullptr : &runtime_entities_.front();
}

Entity* Scene::ResolveEntityTarget(std::string_view target) {
    if (target.empty() || ToLowerCopy(target) == "player") {
        return ResolvePlayerEntity();
    }
    if (Entity* entity = FindEntity(std::string(target)); entity != nullptr) {
        return entity;
    }
    auto it = std::find_if(runtime_entities_.begin(), runtime_entities_.end(), [&](const Entity& entity) {
        return EntityMatchesToken(entity, target);
    });
    if (it != runtime_entities_.end()) {
        return &(*it);
    }
    return nullptr;
}

const Entity* Scene::ResolveEntityTarget(std::string_view target) const {
    if (target.empty() || ToLowerCopy(target) == "player") {
        return ResolvePlayerEntity();
    }
    if (const Entity* entity = FindEntity(std::string(target)); entity != nullptr) {
        return entity;
    }
    auto it = std::find_if(runtime_entities_.begin(), runtime_entities_.end(), [&](const Entity& entity) {
        return EntityMatchesToken(entity, target);
    });
    if (it != runtime_entities_.end()) {
        return &(*it);
    }
    return nullptr;
}

glm::vec2 Scene::ResolveAttachedEntityPosition(const Entity& entity) const {
    if (entity.attached_to.empty()) {
        return entity.position;
    }

    const Entity* parent = FindEntity(entity.attached_to);
    if (parent == nullptr || parent == &entity) {
        return entity.position;
    }

    return ResolveAttachedEntityPosition(*parent) + entity.attach_offset;
}

glm::vec2 Scene::ResolveLightPosition(const assets::LightDefinition& light) const {
    if (light.attached_to.empty()) {
        return light.position;
    }

    const Entity* parent = FindEntity(light.attached_to);
    if (parent != nullptr) {
        return ResolveAttachedEntityPosition(*parent) + parent->size * 0.5f + light.attach_offset;
    }
    if (const assets::ParallaxLayer* layer = FindAttachedParallaxLayer(level_, light.attached_to); layer != nullptr) {
        return ResolveParallaxAnchorPosition(*layer, camera_) + light.attach_offset;
    }
    return light.position;
}

void Scene::RenderParallax(renderer::Renderer2D& renderer) const {
    const auto [minimum, maximum] = VisibleWorldBounds(camera_);

    for (const auto& layer : level_.parallax_layers) {
        if (!layer.visible) {
            continue;
        }

        renderer::Texture2D& texture = asset_manager_->LoadTexture(layer.texture);
        const glm::vec2 draw_size = TextureDrawSize(texture, layer.scale);
        if (draw_size.x <= 1.0f || draw_size.y <= 1.0f) {
            continue;
        }

        const glm::vec2 base = layer.offset + camera_.Position() * (layer.speed * layer.zoom_factor);
        const glm::vec4 lighting{
            layer.receives_lighting ? std::max(layer.lighting_response, 0.0f) * 1.35f : 0.0f,
            1.0f,
            0.0f,
            0.0f
        };
        const glm::vec4 emissive = (layer.artificial_light && layer.artificial_light_strength > 0.001f)
            ? glm::vec4(layer.artificial_light_color.r, layer.artificial_light_color.g, layer.artificial_light_color.b, std::max(layer.artificial_light_strength, 0.0f))
            : glm::vec4(0.0f);

        if (layer.repeat) {
            const float start_x = base.x + std::floor((minimum.x - base.x) / draw_size.x) * draw_size.x - draw_size.x;
            const int tile_count = std::max(4, static_cast<int>(std::ceil((maximum.x - minimum.x) / draw_size.x)) + 3);
            for (int i = 0; i < tile_count; ++i) {
                renderer.DrawSprite(
                    texture,
                    {start_x + i * draw_size.x, base.y},
                    draw_size,
                    layer.tint,
                    layer.depth,
                    {0.0f, 0.0f, 1.0f, 1.0f},
                    0.0f,
                    0.0f,
                    {0.0f, 0.0f, 0.0f, 1.0f},
                    nullptr,
                    nullptr,
                    nullptr,
                    lighting,
                    emissive
                );
            }
        } else {
            renderer.DrawSprite(
                texture,
                base,
                draw_size,
                layer.tint,
                layer.depth,
                {0.0f, 0.0f, 1.0f, 1.0f},
                0.0f,
                0.0f,
                {0.0f, 0.0f, 0.0f, 1.0f},
                nullptr,
                nullptr,
                nullptr,
                lighting,
                emissive);
        }
    }
}

void Scene::RenderTileLayers(renderer::Renderer2D& renderer) const {
    if (level_.tileset.texture.empty()) {
        return;
    }
    renderer::Texture2D& tileset_texture = asset_manager_->LoadTexture(level_.tileset.texture);
    const int columns = std::max(1, level_.tileset.columns);
    const int rows = std::max(1, level_.tileset.rows);
    const glm::vec2 uv_step{1.0f / static_cast<float>(columns), 1.0f / static_cast<float>(rows)};
    const auto [minimum, maximum] = VisibleWorldBounds(camera_);
    const float tile_width = static_cast<float>(std::max(level_.tile_width, 1));
    const float tile_height = static_cast<float>(std::max(level_.tile_height, 1));
    struct TileDrawCommand {
        glm::vec2 position{0.0f, 0.0f};
        glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
        float depth = 0.0f;
    };

    for (const auto& layer : level_.tile_layers) {
        if (!layer.visible) {
            continue;
        }

        const int start_x = std::max(0, static_cast<int>(std::floor((minimum.x - tile_width) / tile_width)));
        const int end_x = std::min(layer.width - 1, static_cast<int>(std::ceil((maximum.x + tile_width) / tile_width)));
        const int start_y = std::max(0, static_cast<int>(std::floor((minimum.y - tile_height) / tile_height)));
        const int end_y = std::min(layer.height - 1, static_cast<int>(std::ceil((maximum.y + tile_height) / tile_height)));
        if (start_x > end_x || start_y > end_y) {
            continue;
        }

        const int visible_rows = end_y - start_y + 1;
        const int visible_columns = end_x - start_x + 1;
        const bool multithreaded =
            project_.multithreading_enabled &&
            core::ThreadPool::Shared().Enabled() &&
            visible_rows * visible_columns >= 256;
        std::vector<TileDrawCommand> commands;

        if (multithreaded) {
            const std::size_t chunk_count = std::max<std::size_t>(1, core::ThreadPool::Shared().WorkerCount() + 1);
            std::vector<std::vector<TileDrawCommand>> partials(chunk_count);
            core::ThreadPool::Shared().ParallelFor(static_cast<std::size_t>(visible_rows), [&](std::size_t begin, std::size_t end) {
                const std::size_t range_index = std::min(begin / std::max<std::size_t>((static_cast<std::size_t>(visible_rows) + chunk_count - 1) / chunk_count, 1), chunk_count - 1);
                auto& bucket = partials[range_index];
                bucket.reserve((end - begin) * static_cast<std::size_t>(visible_columns));
                for (std::size_t row = begin; row < end; ++row) {
                    const int y = start_y + static_cast<int>(row);
                    for (int x = start_x; x <= end_x; ++x) {
                        const int index = y * layer.width + x;
                        if (index < 0 || index >= static_cast<int>(layer.tiles.size())) {
                            continue;
                        }
                        const int tile_id = layer.tiles[index];
                        if (tile_id <= 0) {
                            continue;
                        }

                        const int tile_index = tile_id - 1;
                        const int uv_x = tile_index % columns;
                        const int uv_y = tile_index / columns;
                        bucket.push_back({
                            {static_cast<float>(x) * tile_width, static_cast<float>(y) * tile_height},
                            {
                                uv_x * uv_step.x,
                                uv_y * uv_step.y,
                                (uv_x + 1) * uv_step.x,
                                (uv_y + 1) * uv_step.y
                            },
                            layer.depth
                        });
                    }
                }
            }, 1);

            std::size_t total = 0;
            for (const auto& bucket : partials) {
                total += bucket.size();
            }
            commands.reserve(total);
            for (auto& bucket : partials) {
                commands.insert(commands.end(), bucket.begin(), bucket.end());
            }
        } else {
            commands.reserve(static_cast<std::size_t>(visible_rows * visible_columns));
            for (int y = start_y; y <= end_y; ++y) {
                for (int x = start_x; x <= end_x; ++x) {
                    const int index = y * layer.width + x;
                    if (index < 0 || index >= static_cast<int>(layer.tiles.size())) {
                        continue;
                    }
                    const int tile_id = layer.tiles[index];
                    if (tile_id <= 0) {
                        continue;
                    }

                    const int tile_index = tile_id - 1;
                    const int uv_x = tile_index % columns;
                    const int uv_y = tile_index / columns;
                    commands.push_back({
                        {static_cast<float>(x) * tile_width, static_cast<float>(y) * tile_height},
                        {
                            uv_x * uv_step.x,
                            uv_y * uv_step.y,
                            (uv_x + 1) * uv_step.x,
                            (uv_y + 1) * uv_step.y
                        },
                        layer.depth
                    });
                }
            }
        }

        for (const auto& command : commands) {
            renderer.DrawSprite(
                tileset_texture,
                command.position,
                {tile_width, tile_height},
                {1.0f, 1.0f, 1.0f, 1.0f},
                command.depth,
                command.uv
            );
        }
    }
}

void Scene::RenderEntities(renderer::Renderer2D& renderer, const std::vector<const Entity*>& visible_entities) const {
    for (const Entity* entity_ptr : visible_entities) {
        const auto& entity = *entity_ptr;

        renderer::Texture2D& texture = entity.texture.empty() ? asset_manager_->FallbackTexture() : asset_manager_->LoadTexture(entity.texture);
        renderer::Texture2D* normal_map = nullptr;
        renderer::Texture2D* height_map = nullptr;
        renderer::Texture2D* displacement_map = nullptr;
        if (!entity.normal_map.empty()) {
            normal_map = &asset_manager_->LoadTexture(entity.normal_map);
        }
        if (!entity.height_map.empty()) {
            height_map = &asset_manager_->LoadTexture(entity.height_map);
        }
        if (!entity.displacement_map.empty()) {
            displacement_map = &asset_manager_->LoadTexture(entity.displacement_map);
        }
        const glm::vec4 uv = EntityUv(entity);
        const std::array<glm::vec2, 4> front_face = BuildEntityQuad(entity);
        const glm::vec4 front_material = EntitySurfaceMaterial(entity);

        bool pseudo_3d_enabled = false;
        glm::vec2 pseudo_3d_offset{-18.0f, -14.0f};
        float pseudo_3d_top_tint = 1.08f;
        float pseudo_3d_side_tint = 0.58f;
        if (active_virtual_camera_index_ >= 0 &&
            active_virtual_camera_index_ < static_cast<int>(level_.virtual_cameras.size()) &&
            level_.virtual_cameras[static_cast<std::size_t>(active_virtual_camera_index_)].pseudo_3d_enabled) {
            const auto& pseudo_camera = level_.virtual_cameras[static_cast<std::size_t>(active_virtual_camera_index_)];
            pseudo_3d_enabled = true;
            pseudo_3d_offset = pseudo_camera.pseudo_3d_offset;
            pseudo_3d_top_tint = pseudo_camera.pseudo_3d_top_tint;
            pseudo_3d_side_tint = pseudo_camera.pseudo_3d_side_tint;
        } else if (level_.player_camera.enabled && level_.player_camera.pseudo_3d_enabled) {
            pseudo_3d_enabled = true;
            pseudo_3d_offset = level_.player_camera.pseudo_3d_offset;
            pseudo_3d_top_tint = level_.player_camera.pseudo_3d_top_tint;
            pseudo_3d_side_tint = level_.player_camera.pseudo_3d_side_tint;
        }

        if (pseudo_3d_enabled && entity.pseudo_3d && entity.pseudo_3d_height > 0.01f) {
            const glm::vec2 extrusion = pseudo_3d_offset * (entity.pseudo_3d_height / 16.0f);
            if (glm::length(extrusion) > 0.01f) {
                std::array<glm::vec2, 4> back_face{};
                for (std::size_t i = 0; i < front_face.size(); ++i) {
                    back_face[i] = front_face[i] + extrusion;
                }

                if (std::abs(extrusion.y) > 0.001f) {
                    const std::array<glm::vec2, 4> top_face =
                        extrusion.y < 0.0f
                            ? std::array<glm::vec2, 4>{back_face[0], back_face[1], front_face[1], front_face[0]}
                            : std::array<glm::vec2, 4>{front_face[3], front_face[2], back_face[2], back_face[3]};
                    glm::vec4 top_color = ClampRgb(BiasRgb(ScaleRgb(entity.tint, pseudo_3d_top_tint), 0.12f), 1.6f);
                    renderer.DrawSpriteQuad(texture, top_face, top_color, 19.60f, uv, entity.reflection * 0.9f, EntitySurfaceMaterial(entity, 0.72f, 0.42f), normal_map, height_map, displacement_map);
                }

                if (std::abs(extrusion.x) > 0.001f) {
                    const std::array<glm::vec2, 4> side_face =
                        extrusion.x < 0.0f
                            ? std::array<glm::vec2, 4>{back_face[1], back_face[2], front_face[2], front_face[1]}
                            : std::array<glm::vec2, 4>{front_face[0], front_face[3], back_face[3], back_face[0]};
                    glm::vec4 side_color = ClampRgb(ScaleRgb(entity.tint, pseudo_3d_side_tint), 1.2f);
                    renderer.DrawSpriteQuad(texture, side_face, side_color, 19.70f, uv, entity.reflection * 0.6f, EntitySurfaceMaterial(entity, 0.84f, 0.30f), normal_map, height_map, displacement_map);
                }
            }
        }

        renderer.DrawSpriteQuad(texture, front_face, entity.tint, 20.0f, uv, entity.reflection, front_material, normal_map, height_map, displacement_map);
    }
}

void Scene::RenderParticles(renderer::Renderer2D& renderer) const {
    if (runtime_particle_emitters_.empty()) {
        return;
    }

    std::vector<const ParticleInstance*> alpha_particles;
    std::vector<const ParticleInstance*> additive_particles;
    for (const auto& [_, emitter] : runtime_particle_emitters_) {
        for (const auto& particle : emitter.particles) {
            if (particle.additive) {
                additive_particles.push_back(&particle);
            } else {
                alpha_particles.push_back(&particle);
            }
        }
    }

    auto draw_batch = [&](const std::vector<const ParticleInstance*>& particles, bool additive) {
        if (particles.empty()) {
            return;
        }
        if (additive) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        }
        for (const ParticleInstance* particle_ptr : particles) {
            const auto& particle = *particle_ptr;
            const float t = std::clamp(particle.age / std::max(particle.lifetime, 0.0001f), 0.0f, 1.0f);
            const glm::vec2 size = particle.start_size + (particle.end_size - particle.start_size) * t;
            const glm::vec4 color = particle.start_color + (particle.end_color - particle.start_color) * t;
            const float rotation = particle.align_to_velocity && glm::length(particle.velocity) > 0.01f
                ? std::atan2(particle.velocity.y, particle.velocity.x)
                : particle.rotation * 0.01745329252f;
            renderer::Texture2D& texture = particle.texture.empty()
                ? renderer.RadialLightTexture()
                : asset_manager_->LoadTexture(particle.texture);
            renderer.DrawSprite(
                texture,
                particle.position - size * 0.5f,
                {std::max(size.x, 1.0f), std::max(size.y, 1.0f)},
                color,
                20.25f,
                {0.0f, 0.0f, 1.0f, 1.0f},
                rotation
            );
        }
        renderer.Flush();
        if (additive) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    };

    draw_batch(alpha_particles, false);
    draw_batch(additive_particles, true);
}

void Scene::RenderLights(renderer::Renderer2D& renderer) const {
    const bool lighting_enabled = level_.lighting.enabled || !level_.lights.empty();
    if (!lighting_enabled) {
        return;
    }

    const auto [minimum, maximum] = VisibleWorldBounds(camera_);
    const glm::vec2 size = maximum - minimum;

    if (level_.lighting.enabled) {
        const float ambient_alpha = std::clamp(1.0f - level_.lighting.ambient_intensity, 0.0f, 1.0f);
        glm::vec4 ambient = level_.lighting.ambient_color;
        ambient.a *= level_.lighting.rt_enabled ? ambient_alpha * 0.18f : ambient_alpha;
        renderer.DrawSprite(renderer.WhiteTexture(), minimum, size, ambient, 120.0f);
        renderer.Flush();
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    for (const auto& light : level_.lights) {
        const bool flashlight = IsFlashlight(light);
        const float range = LightRange(light);
        if (!light.enabled || range <= 1.0f || light.intensity <= 0.0f) {
            continue;
        }
        const glm::vec2 resolved_position = ResolveLightPosition(light);
        const glm::vec2 direction = LightDirection(light);
        glm::vec4 color = light.color;
        if (level_.lighting.rt_enabled) {
            glm::vec4 core = color;
            core.a = std::clamp(
                light.color.a * light.intensity * (flashlight ? 0.30f : 0.56f),
                0.0f,
                1.0f);
            if (!flashlight) {
                glm::vec4 halo = color;
                halo.a = std::clamp(light.color.a * light.intensity * 0.28f, 0.0f, 1.0f);
                renderer.DrawLight(
                    resolved_position - glm::vec2(range * 1.35f, range * 1.35f),
                    glm::vec2(range * 2.7f, range * 2.7f),
                    halo,
                    light.source_radius * 1.15f,
                    light.scatter,
                    121.0f,
                    true,
                    0.0f,
                    direction,
                    light.cone_angle,
                    light.cone_softness
                );
            }
            renderer.DrawLight(
                resolved_position - glm::vec2(range, range),
                glm::vec2(range * 2.0f, range * 2.0f),
                core,
                light.source_radius,
                light.scatter,
                121.1f,
                true,
                flashlight ? 1.0f : 0.0f,
                direction,
                light.cone_angle,
                light.cone_softness
            );
        } else {
            color.a = std::clamp(light.color.a * light.intensity, 0.0f, 1.0f);
            renderer.DrawLight(
                resolved_position - glm::vec2(range, range),
                glm::vec2(range * 2.0f, range * 2.0f),
                color,
                light.source_radius,
                light.scatter,
                121.0f,
                false,
                flashlight ? 1.0f : 0.0f,
                direction,
                light.cone_angle,
                light.cone_softness
            );
        }
    }
    renderer.Flush();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Scene::RenderLightShadows(renderer::Renderer2D& renderer) const {
    if (!level_.lighting.rt_enabled || level_.lights.empty()) {
        return;
    }

    struct OccluderRect {
        glm::vec2 position;
        glm::vec2 size;
    };

    struct ShadowGradientCommand {
        std::array<glm::vec2, 4> points{};
        std::array<glm::vec4, 4> colors{};
        float depth = 122.0f;
    };

    constexpr float kPi = 3.14159265359f;
    const auto point_in_rect = [](glm::vec2 point, glm::vec2 position, glm::vec2 size) {
        return point.x >= position.x && point.y >= position.y &&
               point.x <= position.x + size.x && point.y <= position.y + size.y;
    };

    std::vector<std::vector<ShadowGradientCommand>> light_commands(level_.lights.size());
    auto build_light_commands = [&](std::size_t begin, std::size_t end) {
        for (std::size_t light_index = begin; light_index < end; ++light_index) {
            const auto& light = level_.lights[light_index];
            const bool flashlight = IsFlashlight(light);
            const float range = LightRange(light);
            if (!light.enabled || range <= 1.0f || light.intensity <= 0.0f) {
                continue;
            }

            const glm::vec2 light_position = ResolveLightPosition(light);
            const glm::vec2 light_direction = LightDirection(light);
            const float outer_half_angle = glm::radians(std::clamp(light.cone_angle, 4.0f, 170.0f) * 0.5f);
            const float outer_cone_cos = std::cos(outer_half_angle + glm::radians(10.0f));
            const auto occluder_in_range = [&](glm::vec2 center, glm::vec2 size) {
                if (Distance(center, light_position) > range + glm::length(size) * 0.75f) {
                    return false;
                }
                if (!flashlight) {
                    return true;
                }
                glm::vec2 to_center = center - light_position;
                const float distance = glm::length(to_center);
                if (distance < 0.001f) {
                    return true;
                }
                to_center /= distance;
                return glm::dot(light_direction, to_center) >= outer_cone_cos;
            };
            std::vector<OccluderRect> occluders;

            for (const auto& entity : runtime_entities_) {
                if (!entity.active || !entity.visible || !entity.collidable) {
                    continue;
                }
                const glm::vec2 position = ColliderPosition(entity);
                const glm::vec2 size = ColliderSize(entity);
                const glm::vec2 center = position + size * 0.5f;
                if (occluder_in_range(center, size)) {
                    occluders.push_back({position, size});
                }
            }

            const int tile_w = std::max(level_.tile_width, 1);
            const int tile_h = std::max(level_.tile_height, 1);
            const int min_x = static_cast<int>(std::floor((light_position.x - range) / static_cast<float>(tile_w))) - 1;
            const int max_x = static_cast<int>(std::ceil((light_position.x + range) / static_cast<float>(tile_w))) + 1;
            const int min_y = static_cast<int>(std::floor((light_position.y - range) / static_cast<float>(tile_h))) - 1;
            const int max_y = static_cast<int>(std::ceil((light_position.y + range) / static_cast<float>(tile_h))) + 1;
            for (const auto& layer : level_.tile_layers) {
                for (int y = min_y; y <= max_y; ++y) {
                    if (y < 0 || y >= layer.height) {
                        continue;
                    }
                    for (int x = min_x; x <= max_x; ++x) {
                        if (x < 0 || x >= layer.width) {
                            continue;
                        }
                        const int index = y * layer.width + x;
                        if (index < 0 || index >= static_cast<int>(layer.tiles.size())) {
                            continue;
                        }
                        const int tile_id = layer.tiles[static_cast<std::size_t>(index)];
                        if (!IsTileOccluder(level_, layer, tile_id)) {
                            continue;
                        }
                        const glm::vec2 position{x * static_cast<float>(tile_w), y * static_cast<float>(tile_h)};
                        const glm::vec2 size{static_cast<float>(tile_w), static_cast<float>(tile_h)};
                        if (occluder_in_range(position + size * 0.5f, size)) {
                            occluders.push_back({position, size});
                        }
                    }
                }
            }

            auto& commands = light_commands[light_index];
            commands.reserve(occluders.size() * static_cast<std::size_t>(1 + std::max(level_.lighting.shadow_samples, 1) * 2));

            const float shadow_strength = std::clamp(level_.lighting.shadow_strength, 0.05f, 2.4f);
            const float softness = std::clamp(light.scatter * std::max(level_.lighting.shadow_softness, 0.1f), 0.2f, 4.2f);
            const float diffusion = std::clamp(level_.lighting.shadow_diffusion, 0.0f, 2.8f);
            const float source_softness = std::clamp(
                (light.source_radius / std::max(range, 1.0f)) * (0.7f + std::max(level_.lighting.shadow_softness, 0.1f) * 0.45f),
                0.0f,
                0.85f
            );
            const int shadow_samples = std::clamp(level_.lighting.shadow_samples, 1, 16);
            const float shadow_length = range * (1.18f + softness * 0.16f + source_softness * 0.44f + diffusion * 0.12f);
            const float sample_radius = std::max(light.source_radius * (0.2f + level_.lighting.shadow_softness * 0.28f + diffusion * 0.12f), 1.5f);

            for (const auto& occluder : occluders) {
                if (point_in_rect(light_position, occluder.position, occluder.size)) {
                    continue;
                }

                const glm::vec2 center = occluder.position + occluder.size * 0.5f;
                const float light_distance = Distance(center, light_position);
                if (light_distance > range + glm::length(occluder.size)) {
                    continue;
                }

                std::array<glm::vec2, 4> corners{
                    occluder.position,
                    occluder.position + glm::vec2(occluder.size.x, 0.0f),
                    occluder.position + occluder.size,
                    occluder.position + glm::vec2(0.0f, occluder.size.y)
                };
                const float reference = std::atan2(center.y - light_position.y, center.x - light_position.x);
                std::sort(corners.begin(), corners.end(), [&](glm::vec2 lhs, glm::vec2 rhs) {
                    float a = std::atan2(lhs.y - light_position.y, lhs.x - light_position.x) - reference;
                    float b = std::atan2(rhs.y - light_position.y, rhs.x - light_position.x) - reference;
                    while (a > kPi) {
                        a -= kPi * 2.0f;
                    }
                    while (a < -kPi) {
                        a += kPi * 2.0f;
                    }
                    while (b > kPi) {
                        b -= kPi * 2.0f;
                    }
                    while (b < -kPi) {
                        b += kPi * 2.0f;
                    }
                    return a < b;
                });

                const glm::vec2 p0 = corners.front();
                const glm::vec2 p1 = corners.back();
                glm::vec2 dir0 = p0 - light_position;
                glm::vec2 dir1 = p1 - light_position;
                if (glm::length(dir0) < 0.001f || glm::length(dir1) < 0.001f) {
                    continue;
                }
                dir0 = glm::normalize(dir0);
                dir1 = glm::normalize(dir1);
                const glm::vec2 p0_far = p0 + dir0 * shadow_length;
                const glm::vec2 p1_far = p1 + dir1 * shadow_length;
                const float occlusion = std::clamp(1.0f - light_distance / std::max(range, 1.0f), 0.0f, 1.0f);
                const float alpha = std::clamp((0.14f + occlusion * 0.42f * light.intensity) * shadow_strength, 0.0f, 0.9f);
                glm::vec2 edge_dir = p1 - p0;
                if (glm::length(edge_dir) < 0.001f) {
                    continue;
                }
                edge_dir = glm::normalize(edge_dir);
                const float base_spread = 3.5f + softness * 3.0f + sample_radius * 0.12f + diffusion * 4.8f;
                const glm::vec2 p0_base = p0 - edge_dir * base_spread;
                const glm::vec2 p1_base = p1 + edge_dir * base_spread;
                const glm::vec4 near_color{0.0f, 0.0f, 0.0f, alpha * (0.54f - source_softness * 0.12f)};
                const glm::vec4 far_color{0.0f, 0.0f, 0.0f, alpha * 0.01f};
                commands.push_back({
                    {p0_base, p1_base, p1_far, p0_far},
                    {near_color, near_color, far_color, far_color},
                    122.0f
                });

                const float feather_start = 0.09f + softness * 0.045f + diffusion * 0.05f;
                const float feather_end = 1.05f + softness * 0.24f + source_softness * 0.32f + diffusion * 0.28f;
                const float base_phase = (light_position.x * 0.017f + light_position.y * 0.011f + center.x * 0.007f) * 0.5f;
                for (int sample_index = 0; sample_index < shadow_samples; ++sample_index) {
                    const float t = shadow_samples == 1 ? 0.0f : static_cast<float>(sample_index) / static_cast<float>(shadow_samples - 1);
                    const float angle = base_phase + t * kPi * 2.0f;
                    const glm::vec2 sample_offset{std::cos(angle) * sample_radius, std::sin(angle) * sample_radius};
                    const glm::vec2 sample_light = light_position + sample_offset;

                    auto sample_corners = corners;
                    const float sample_reference = std::atan2(center.y - sample_light.y, center.x - sample_light.x);
                    std::sort(sample_corners.begin(), sample_corners.end(), [&](glm::vec2 lhs, glm::vec2 rhs) {
                        float a = std::atan2(lhs.y - sample_light.y, lhs.x - sample_light.x) - sample_reference;
                        float b = std::atan2(rhs.y - sample_light.y, rhs.x - sample_light.x) - sample_reference;
                        while (a > kPi) {
                            a -= kPi * 2.0f;
                        }
                        while (a < -kPi) {
                            a += kPi * 2.0f;
                        }
                        while (b > kPi) {
                            b -= kPi * 2.0f;
                        }
                        while (b < -kPi) {
                            b += kPi * 2.0f;
                        }
                        return a < b;
                    });

                    const glm::vec2 sample_p0 = sample_corners.front();
                    const glm::vec2 sample_p1 = sample_corners.back();
                    glm::vec2 sample_dir0 = sample_p0 - sample_light;
                    glm::vec2 sample_dir1 = sample_p1 - sample_light;
                    if (glm::length(sample_dir0) < 0.001f || glm::length(sample_dir1) < 0.001f) {
                        continue;
                    }
                    sample_dir0 = glm::normalize(sample_dir0);
                    sample_dir1 = glm::normalize(sample_dir1);

                    glm::vec2 sample_edge = sample_p1 - sample_p0;
                    if (glm::length(sample_edge) < 0.001f) {
                        continue;
                    }
                    sample_edge = glm::normalize(sample_edge);
                    const float sample_spread = base_spread * (1.2f + t * 0.55f + diffusion * 0.18f);
                    const glm::vec2 sample_p0_base = sample_p0 - sample_edge * sample_spread;
                    const glm::vec2 sample_p1_base = sample_p1 + sample_edge * sample_spread;

                    const glm::vec2 sample_p0_mid = sample_p0_base + sample_dir0 * (shadow_length * feather_start);
                    const glm::vec2 sample_p1_mid = sample_p1_base + sample_dir1 * (shadow_length * feather_start);
                    const glm::vec2 sample_p0_soft = sample_p0_base + sample_dir0 * (shadow_length * feather_end * (1.0f + t * 0.18f + diffusion * 0.08f));
                    const glm::vec2 sample_p1_soft = sample_p1_base + sample_dir1 * (shadow_length * feather_end * (1.0f + t * 0.18f + diffusion * 0.08f));
                    const float sample_alpha = alpha * (0.26f + softness * 0.035f + diffusion * 0.04f) / static_cast<float>(shadow_samples);
                    const glm::vec4 soft_near{0.0f, 0.0f, 0.0f, sample_alpha};
                    const glm::vec4 soft_far{0.0f, 0.0f, 0.0f, 0.0f};
                    commands.push_back({
                        {sample_p0_mid, sample_p1_mid, sample_p1_soft, sample_p0_soft},
                        {soft_near, soft_near, soft_far, soft_far},
                        122.02f + t * 0.002f
                    });

                    if (diffusion > 0.01f) {
                        glm::vec2 dir_center = (sample_dir0 + sample_dir1) * 0.5f;
                        if (glm::length(dir_center) > 0.001f) {
                            dir_center = glm::normalize(dir_center);
                            const glm::vec2 fan_offset = glm::vec2(-dir_center.y, dir_center.x) * (sample_spread * (0.22f + diffusion * 0.24f));
                            const glm::vec4 fan_near{0.0f, 0.0f, 0.0f, sample_alpha * 0.58f};
                            commands.push_back({
                                {sample_p0_mid - fan_offset, sample_p1_mid + fan_offset, sample_p1_soft + fan_offset * 1.7f, sample_p0_soft - fan_offset * 1.7f},
                                {fan_near, fan_near, soft_far, soft_far},
                                122.08f + t * 0.002f
                            });
                        }
                    }
                }
            }
        }
    };

    const bool multithreaded =
        project_.multithreading_enabled &&
        core::ThreadPool::Shared().Enabled() &&
        level_.lights.size() >= 2;
    if (multithreaded) {
        core::ThreadPool::Shared().ParallelFor(level_.lights.size(), build_light_commands, 1);
    } else {
        build_light_commands(0, level_.lights.size());
    }

    for (const auto& commands : light_commands) {
        for (const auto& command : commands) {
            renderer.DrawQuadGradient(command.points, command.colors, command.depth);
        }
    }
    renderer.Flush();
}

void Scene::RenderDebug(renderer::Renderer2D& renderer) const {
    for (const auto& trigger : level_.triggers) {
        glm::vec4 color = trigger.fired ? glm::vec4(0.2f, 1.0f, 0.2f, 1.0f) : trigger.color;
        renderer.DrawRectOutline(trigger.position, trigger.size, color, 2.0f, 40.0f);
    }

    for (const auto& entity : runtime_entities_) {
        if (entity.collidable) {
            renderer.DrawRectOutline(ColliderPosition(entity), ColliderSize(entity), {1.0f, 0.25f, 0.8f, 1.0f}, 1.5f, 39.0f);
        }
    }

    for (const auto& light : level_.lights) {
        if (light.enabled) {
            const glm::vec2 resolved_position = ResolveLightPosition(light);
            const float range = LightRange(light);
            renderer.DrawRectOutline(
                resolved_position - glm::vec2(range, range),
                glm::vec2(range * 2.0f, range * 2.0f),
                {1.0f, 0.92f, 0.35f, 0.9f},
                1.0f,
                38.5f
            );
            if (IsFlashlight(light)) {
                const glm::vec2 direction = LightDirection(light);
                const float half_angle = glm::radians(std::clamp(light.cone_angle, 4.0f, 170.0f) * 0.5f);
                const auto rotate = [&](glm::vec2 vector, float angle) {
                    const float cosine = std::cos(angle);
                    const float sine = std::sin(angle);
                    return glm::vec2{vector.x * cosine - vector.y * sine, vector.x * sine + vector.y * cosine};
                };
                const glm::vec2 left = resolved_position + rotate(direction, -half_angle) * range;
                const glm::vec2 right = resolved_position + rotate(direction, half_angle) * range;
                renderer.DrawRectOutline(resolved_position - glm::vec2(3.0f, 3.0f), {6.0f, 6.0f}, {1.0f, 0.92f, 0.35f, 1.0f}, 1.0f, 38.55f);
                renderer.DrawQuadGradient(
                    {resolved_position, left, right, resolved_position},
                    std::array<glm::vec4, 4>{
                        glm::vec4{1.0f, 0.92f, 0.35f, 0.18f},
                        glm::vec4{1.0f, 0.92f, 0.35f, 0.02f},
                        glm::vec4{1.0f, 0.92f, 0.35f, 0.02f},
                        glm::vec4{1.0f, 0.92f, 0.35f, 0.18f}
                    },
                    38.45f
                );
            }
        }
    }

    for (const auto& source : level_.audio_sources) {
        if (source.enabled) {
            renderer.DrawRectOutline(
                source.position - glm::vec2(source.radius, source.radius),
                glm::vec2(source.radius * 2.0f, source.radius * 2.0f),
                {0.34f, 0.96f, 0.66f, 0.85f},
                1.0f,
                38.0f
            );
        }
    }

    for (const auto& pak : level_.audio_paks) {
        if (pak.enabled) {
            renderer.DrawRectOutline(
                pak.position - glm::vec2(pak.radius, pak.radius),
                glm::vec2(pak.radius * 2.0f, pak.radius * 2.0f),
                {0.74f, 0.62f, 1.0f, 0.85f},
                1.0f,
                37.9f
            );
        }
    }

    for (std::size_t i = 0; i < level_.virtual_cameras.size(); ++i) {
        const auto& camera = level_.virtual_cameras[i];
        const bool active = active_virtual_camera_index_ == static_cast<int>(i);
        renderer.DrawRectOutline(
            camera.position,
            camera.size,
            active ? glm::vec4(0.30f, 0.85f, 1.0f, 1.0f) : glm::vec4(0.32f, 0.60f, 0.96f, 0.82f),
            active ? 2.5f : 1.0f,
            37.8f
        );
        const glm::vec2 center = camera.position + camera.size * 0.5f;
        renderer.DrawRectOutline(
            center - camera.dead_zone * 0.5f,
            camera.dead_zone,
            active ? glm::vec4(0.55f, 0.95f, 0.92f, 0.95f) : glm::vec4(0.45f, 0.88f, 0.84f, 0.65f),
            1.0f,
            37.7f
        );
    }

    if (!HasActiveVirtualCamera() && level_.player_camera.enabled) {
        const glm::vec2 center = camera_.Position();
        renderer.DrawRectOutline(
            center - level_.player_camera.dead_zone * 0.5f,
            level_.player_camera.dead_zone,
            {0.98f, 0.56f, 0.22f, 0.95f},
            1.6f,
            37.65f
        );
    }

    if (const Entity* player = Player(); player != nullptr) {
        renderer.DrawRectOutline(player->position, player->size, {0.2f, 0.9f, 1.0f, 1.0f}, 2.0f, 41.0f);
    }
}

}  // namespace novaiso::entities
