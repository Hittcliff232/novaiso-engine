#pragma once

#include "assets/AssetManager.h"
#include "assets/Project.h"
#include "camera/Camera2D.h"
#include "entities/Entity.h"
#include "physics/PlatformerPhysics.h"
#include "renderer/Renderer2D.h"
#include "triggers/TriggerSystem.h"

#include <glm/vec2.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace novaiso::core {
class Input;
}

namespace novaiso::scripting {
class PythonScripting;
}

namespace novaiso::entities {

class Scene {
public:
    struct RenderPassTiming {
        std::string name;
        float cpu_ms = 0.0f;
    };

    bool Load(
        const std::filesystem::path& project_root,
        const assets::ProjectData& project,
        const std::string& level_relative_path,
        assets::AssetManager& asset_manager,
        scripting::PythonScripting& scripting
    );

    void SaveLevelToDisk() const;
    void ResetSimulation(bool reset_camera = true);
    void RefreshResources(const assets::ProjectData& project, bool reset_camera = true);
    void Update(float delta_time, const core::Input& input);
    void Render(renderer::Renderer2D& renderer, bool draw_debug) const;

    [[nodiscard]] Entity* FindEntity(const std::string& id);
    [[nodiscard]] const Entity* FindEntity(const std::string& id) const;
    [[nodiscard]] Entity* Player();
    [[nodiscard]] const Entity* Player() const;

    void ToggleCameraMode();
    void SetCameraMode(camera::Mode mode);
    [[nodiscard]] camera::Mode GetCameraMode() const;
    [[nodiscard]] std::string CameraModeName() const;
    [[nodiscard]] bool ActionDown(const std::string& action) const;

    void PlaySound(const std::string& path);
    void PlayMusic(const std::string& path, bool loop);
    void StopAllAudio();
    void Log(const std::string& message);
    void ScheduleTriggerAction(const assets::TriggerAction& action, const std::string& trigger_name, float delay_seconds);
    void MarkTriggerFired(const std::string& trigger_name);
    [[nodiscard]] float TriggerLastFiredTime(const std::string& trigger_name) const;
    [[nodiscard]] float TimeSeconds() const;

    [[nodiscard]] const std::vector<std::string>& Messages() const;
    void ClearMessages();

    [[nodiscard]] assets::LevelData& EditableLevel();
    [[nodiscard]] const assets::LevelData& Level() const;
    [[nodiscard]] std::vector<Entity>& RuntimeEntities();
    [[nodiscard]] const std::vector<Entity>& RuntimeEntities() const;
    [[nodiscard]] const std::filesystem::path& ProjectRoot() const;
    [[nodiscard]] const std::filesystem::path& LevelPath() const;
    [[nodiscard]] const assets::ProjectData& Project() const;
    [[nodiscard]] camera::Camera2D& Camera();
    [[nodiscard]] const camera::Camera2D& Camera() const;
    void SetCameraFollowEnabled(bool enabled);
    [[nodiscard]] bool CameraFollowEnabled() const;
    void SetCameraZoom(float zoom);
    void SetCameraZoomSmooth(float zoom, float speed);
    [[nodiscard]] float CameraZoom() const;
    [[nodiscard]] int AnimatedEntityCount() const;
    [[nodiscard]] int ActiveAnimatedEntityCount() const;
    [[nodiscard]] glm::vec2 WorldSize() const;
    [[nodiscard]] assets::AssetManager& Assets();
    [[nodiscard]] scripting::PythonScripting& Scripts();
    [[nodiscard]] const assets::SpriteAsset* FindSpriteAsset(const std::string& path) const;
    [[nodiscard]] const assets::ObjectAsset* FindObjectAsset(const std::string& path) const;
    [[nodiscard]] const assets::TriggerAsset* FindTriggerAsset(const std::string& path) const;
    [[nodiscard]] assets::LightDefinition* FindLight(const std::string& name);
    [[nodiscard]] const assets::LightDefinition* FindLight(const std::string& name) const;
    [[nodiscard]] assets::AudioSourceDefinition* FindAudioSource(const std::string& id);
    [[nodiscard]] const assets::AudioSourceDefinition* FindAudioSource(const std::string& id) const;
    [[nodiscard]] assets::AudioPakDefinition* FindAudioPak(const std::string& id);
    [[nodiscard]] const assets::AudioPakDefinition* FindAudioPak(const std::string& id) const;
    [[nodiscard]] assets::VirtualCameraDefinition* FindVirtualCamera(const std::string& id);
    [[nodiscard]] const assets::VirtualCameraDefinition* FindVirtualCamera(const std::string& id) const;
    [[nodiscard]] assets::SceneAnimationDefinition* FindSceneAnimation(const std::string& id);
    [[nodiscard]] const assets::SceneAnimationDefinition* FindSceneAnimation(const std::string& id) const;
    [[nodiscard]] const assets::ObjectAnimationAsset* FindObjectAnimationAsset(const std::string& path) const;
    [[nodiscard]] const assets::ParticleEffectAsset* FindParticleEffectAsset(const std::string& path) const;
    bool ActivateVirtualCamera(const std::string& id_or_name);
    void ReleaseVirtualCamera();
    [[nodiscard]] bool HasActiveVirtualCamera() const;
    [[nodiscard]] std::string ActiveVirtualCameraId() const;
    bool PlaySceneAnimation(const std::string& id_or_name, bool restart = true);
    bool StopSceneAnimation(const std::string& id_or_name, bool restore_state = true);
    bool PlayParticleEmitter(const std::string& id_or_name, bool restart = true);
    bool StopParticleEmitter(const std::string& id_or_name);
    bool BurstParticleEmitter(const std::string& id_or_name, int override_count = -1);
    [[nodiscard]] int SceneAnimationCount() const;
    [[nodiscard]] int PlayingSceneAnimationCount() const;
    [[nodiscard]] int ActiveParticleCount() const;
    void SetDebugTraceEnabled(bool enabled);
    [[nodiscard]] bool DebugTraceEnabled() const;
    void Trace(std::string_view message);
    [[nodiscard]] const std::vector<RenderPassTiming>& LastRenderPassTimings() const;
    [[nodiscard]] float LastRenderTotalCpuMs() const;

private:
    struct AudioSourceState {
        int channel = -1;
        bool was_active = false;
    };

    struct AudioPakState {
        int channel = -1;
        bool was_active = false;
        int current_track = -1;
        bool finished = false;
    };

    struct PendingTriggerAction {
        assets::TriggerAction action;
        std::string trigger_name;
        float remaining_time = 0.0f;
    };

    struct RuntimeSceneAnimationState {
        std::string id;
        bool playing = false;
        bool completed = false;
        float time = 0.0f;
        glm::vec2 base_position{0.0f, 0.0f};
        glm::vec2 base_size{32.0f, 32.0f};
        glm::vec4 base_tint{1.0f, 1.0f, 1.0f, 1.0f};
        float base_rotation = 0.0f;
        glm::vec2 base_skew{0.0f, 0.0f};
        bool captured = false;
    };

    struct ParticleInstance {
        std::string texture;
        glm::vec2 position{0.0f, 0.0f};
        glm::vec2 velocity{0.0f, 0.0f};
        glm::vec2 acceleration{0.0f, 0.0f};
        glm::vec2 start_size{12.0f, 12.0f};
        glm::vec2 end_size{4.0f, 4.0f};
        glm::vec4 start_color{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 end_color{1.0f, 1.0f, 1.0f, 0.0f};
        float age = 0.0f;
        float lifetime = 1.0f;
        float rotation = 0.0f;
        float angular_velocity = 0.0f;
        bool additive = false;
        bool align_to_velocity = false;
    };

    struct RuntimeParticleEmitterState {
        std::string id;
        std::string asset_path;
        bool enabled = true;
        bool autoplay = true;
        bool playing = true;
        bool burst_on_start = false;
        bool burst_started = false;
        bool transient = false;
        glm::vec2 transient_position{0.0f, 0.0f};
        float spawn_accumulator = 0.0f;
        int pending_burst_count = 0;
        std::vector<ParticleInstance> particles;
    };

    void StepFixed(float delta_time);
    void RebuildRuntimeEntities();
    void LoadResourceLibraries();
    void UpdateAnimations(float delta_time);
    void UpdatePlayerCamera(float delta_time);
    void UpdateVirtualCamera(float delta_time);
    void UpdateSceneAnimations(float delta_time);
    void UpdateParticles(float delta_time);
    void UpdateAttachments();
    void UpdateAudio(float delta_time);
    void ResolveEntityCollisions();
    void SpawnParticleBurstAt(const std::string& asset_path, glm::vec2 position, int count = -1);
    [[nodiscard]] Entity* ResolvePlayerEntity();
    [[nodiscard]] const Entity* ResolvePlayerEntity() const;
    [[nodiscard]] Entity* ResolveEntityTarget(std::string_view target);
    [[nodiscard]] const Entity* ResolveEntityTarget(std::string_view target) const;
    [[nodiscard]] glm::vec2 ResolveAttachedEntityPosition(const Entity& entity) const;
    [[nodiscard]] glm::vec2 ResolveLightPosition(const assets::LightDefinition& light) const;
    void RenderParallax(renderer::Renderer2D& renderer) const;
    void RenderTileLayers(renderer::Renderer2D& renderer) const;
    void RenderEntities(renderer::Renderer2D& renderer, const std::vector<const Entity*>& visible_entities) const;
    void RenderParticles(renderer::Renderer2D& renderer) const;
    void RenderLights(renderer::Renderer2D& renderer) const;
    void RenderLightShadows(renderer::Renderer2D& renderer) const;
    void RenderDebug(renderer::Renderer2D& renderer) const;
    [[nodiscard]] std::vector<renderer::Renderer2D::LightSource> BuildActiveLights() const;
    [[nodiscard]] std::vector<const Entity*> BuildVisibleEntities() const;

    std::filesystem::path project_root_;
    std::filesystem::path level_path_;
    assets::ProjectData project_;
    assets::LevelData level_;
    assets::AssetManager* asset_manager_ = nullptr;
    scripting::PythonScripting* scripting_ = nullptr;
    std::vector<Entity> runtime_entities_;
    std::unordered_map<std::string, assets::SpriteAsset> sprite_assets_;
    std::unordered_map<std::string, assets::ObjectAsset> object_assets_;
    std::unordered_map<std::string, assets::TriggerAsset> trigger_assets_;
    std::unordered_map<std::string, assets::ObjectAnimationAsset> animation_assets_;
    std::unordered_map<std::string, assets::ParticleEffectAsset> particle_assets_;
    physics::PlatformerPhysics physics_;
    triggers::TriggerSystem trigger_system_;
    camera::Camera2D camera_;
    bool camera_follow_enabled_ = true;
    std::string camera_follow_target_id_ = "player";
    const core::Input* input_ = nullptr;
    std::vector<std::string> messages_;
    std::unordered_map<std::string, AudioSourceState> audio_source_states_;
    std::unordered_map<std::string, AudioPakState> audio_pak_states_;
    std::vector<PendingTriggerAction> pending_trigger_actions_;
    std::unordered_map<std::string, float> trigger_last_fired_time_;
    float accumulator_ = 0.0f;
    float scene_time_seconds_ = 0.0f;
    float camera_zoom_target_ = 1.0f;
    float camera_zoom_speed_ = 0.0f;
    int active_virtual_camera_index_ = -1;
    camera::Mode camera_mode_before_virtual_camera_ = camera::Mode::Side;
    std::vector<RuntimeSceneAnimationState> runtime_animation_states_;
    std::unordered_map<std::string, RuntimeParticleEmitterState> runtime_particle_emitters_;
    bool debug_trace_enabled_ = false;
    mutable std::vector<RenderPassTiming> last_render_pass_timings_;
    mutable float last_render_total_cpu_ms_ = 0.0f;
};

}  // namespace novaiso::entities
