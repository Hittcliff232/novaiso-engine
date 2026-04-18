#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <glm/ext/vector_int2.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace novaiso::assets {

enum class CollisionType {
    Empty,
    Solid,
    OneWay,
    SlopeLeft,
    SlopeRight
};

struct TileInfo {
    int id = 0;
    CollisionType collision = CollisionType::Empty;
};

struct Tileset {
    std::string texture;
    int tile_width = 32;
    int tile_height = 32;
    int columns = 1;
    int rows = 1;
    std::unordered_map<int, TileInfo> tile_info;
};

struct TileLayer {
    std::string name;
    int width = 0;
    int height = 0;
    float depth = 0.0f;
    bool visible = true;
    bool collidable = false;
    std::vector<int> tiles;
};

struct ParallaxLayer {
    std::string id;
    std::string name;
    std::string texture;
    glm::vec2 speed{0.25f, 0.0f};
    glm::vec2 scale{1.0f, 1.0f};
    glm::vec2 offset{0.0f, 0.0f};
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    float depth = -200.0f;
    float zoom_factor = 1.0f;
    bool receives_lighting = true;
    float lighting_response = 1.0f;
    bool artificial_light = false;
    glm::vec4 artificial_light_color{1.0f, 0.92f, 0.78f, 1.0f};
    float artificial_light_strength = 0.0f;
    bool repeat = true;
    bool visible = true;
    std::string editor_group;
};

struct SpriteFrame {
    std::string texture;
    float duration = 0.1f;
};

struct SpriteAnimation {
    std::string name = "idle";
    bool loop = true;
    std::vector<SpriteFrame> frames;
};

struct SpriteAsset {
    std::string name;
    glm::ivec2 canvas_size{16, 16};
    std::string default_animation = "idle";
    glm::vec2 pivot{0.0f, 0.0f};
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<SpriteAnimation> animations;
};

struct ObjectAsset {
    std::string name;
    std::string preset = "none";
    std::string sprite;
    std::string default_animation = "idle";
    glm::vec2 size{32.0f, 32.0f};
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    int layer = 0;
    bool dynamic = false;
    bool collidable = false;
    float reflection = 0.0f;
    std::string normal_map;
    std::string height_map;
    std::string displacement_map;
    bool relief_enabled = false;
    float bump_strength = 1.0f;
    float relief_depth = 0.035f;
    float parallax_depth = 0.018f;
    float relief_contrast = 1.35f;
    bool pseudo_3d = false;
    glm::vec2 collider_offset{0.0f, 0.0f};
    glm::vec2 collider_size{32.0f, 32.0f};
    float pseudo_3d_height = 16.0f;
    std::string sound;
    std::string script;
    nlohmann::json properties = nlohmann::json::object();
};

struct EntityDefinition {
    std::string id;
    std::string archetype;
    std::string object_asset;
    std::string sprite_asset;
    std::string animation = "idle";
    glm::vec2 position{0.0f, 0.0f};
    glm::vec2 size{32.0f, 32.0f};
    glm::vec2 velocity{0.0f, 0.0f};
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    std::string texture;
    std::string sound;
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
    int layer = 0;
    bool dynamic = false;
    bool collidable = false;
    bool visible = true;
    float reflection = 0.0f;
    std::string normal_map;
    std::string height_map;
    std::string displacement_map;
    bool relief_enabled = false;
    float bump_strength = 1.0f;
    float relief_depth = 0.035f;
    float parallax_depth = 0.018f;
    float relief_contrast = 1.35f;
    bool pseudo_3d = false;
    glm::vec2 collider_offset{0.0f, 0.0f};
    glm::vec2 collider_size{32.0f, 32.0f};
    float pseudo_3d_height = 16.0f;
    float animation_speed = 1.0f;
    std::string script;
    std::string on_trigger;
    std::string attached_to;
    glm::vec2 attach_offset{0.0f, 0.0f};
    nlohmann::json properties = nlohmann::json::object();
    std::string editor_group;
};

struct TriggerCondition {
    std::string script;
    std::string function;
    std::string args_json = "{}";
    glm::vec2 editor_position{260.0f, 140.0f};
};

struct TriggerAction {
    std::string script;
    std::string function;
    std::string args_json = "{}";
    glm::vec2 editor_position{620.0f, 140.0f};
};

struct TriggerAsset {
    std::string name;
    glm::vec2 default_size{96.0f, 96.0f};
    glm::vec4 color{1.0f, 0.7f, 0.1f, 1.0f};
    bool once = false;
    bool enabled = true;
    std::vector<TriggerCondition> conditions;
    std::vector<TriggerAction> actions;
};

struct TriggerZone {
    std::string id;
    std::string name;
    std::string asset;
    glm::vec2 position{0.0f, 0.0f};
    glm::vec2 size{64.0f, 64.0f};
    glm::vec4 color{1.0f, 0.7f, 0.1f, 1.0f};
    bool once = false;
    bool enabled = true;
    bool fired = false;
    std::vector<TriggerCondition> conditions;
    std::vector<TriggerAction> actions;
    std::string editor_group;
};

struct ParticleEffectAsset {
    std::string name;
    std::string preset = "custom";
    std::string texture;
    std::string sound;
    glm::vec4 start_color{1.0f, 0.72f, 0.34f, 0.92f};
    glm::vec4 end_color{0.28f, 0.04f, 0.02f, 0.0f};
    glm::vec2 acceleration{0.0f, 58.0f};
    glm::vec2 start_size{20.0f, 20.0f};
    glm::vec2 end_size{8.0f, 8.0f};
    float velocity_angle_min = -95.0f;
    float velocity_angle_max = -85.0f;
    float speed_min = 40.0f;
    float speed_max = 120.0f;
    float lifetime_min = 0.35f;
    float lifetime_max = 0.90f;
    float spawn_rate = 28.0f;
    int burst_count = 18;
    float emission_radius = 6.0f;
    float drag = 0.0f;
    float angular_velocity_min = -40.0f;
    float angular_velocity_max = 40.0f;
    bool loop = true;
    bool additive = true;
    bool align_to_velocity = false;
};

struct LightingSettings {
    bool enabled = false;
    bool rt_enabled = false;
    glm::vec4 ambient_color{0.08f, 0.09f, 0.14f, 1.0f};
    float ambient_intensity = 0.35f;
    float shadow_strength = 1.0f;
    float shadow_softness = 1.0f;
    float shadow_diffusion = 1.0f;
    int shadow_samples = 4;
};

struct LightDefinition {
    std::string id = "light";
    std::string name = "Lamp";
    std::string type = "point";
    glm::vec2 position{0.0f, 0.0f};
    float radius = 280.0f;
    float length = 280.0f;
    float source_radius = 26.0f;
    float scatter = 1.0f;
    float direction_degrees = -35.0f;
    float cone_angle = 42.0f;
    float cone_softness = 0.28f;
    glm::vec4 color{1.0f, 0.85f, 0.55f, 0.9f};
    float intensity = 1.0f;
    bool enabled = true;
    std::string attached_to;
    glm::vec2 attach_offset{0.0f, 0.0f};
    std::string editor_group;
};

struct AudioSourceDefinition {
    std::string id = "audio_source";
    std::string name = "Audio Source";
    std::string audio;
    glm::vec2 position{0.0f, 0.0f};
    bool enabled = true;
    bool always = false;
    bool loop = false;
    float radius = 180.0f;
    bool stop_on_exit = true;
    float distance = 620.0f;
    float volume = 1.0f;
    std::string editor_group;
};

struct AudioPakDefinition {
    std::string id = "audio_pak";
    std::string name = "Audio Pak";
    glm::vec2 position{0.0f, 0.0f};
    bool enabled = true;
    bool always = true;
    bool loop = false;
    float radius = 420.0f;
    bool stop_on_exit = false;
    float distance = 1200.0f;
    float volume = 1.0f;
    bool shuffle = false;
    bool repeat_playlist = true;
    std::vector<std::string> tracks;
    std::string editor_group;
};

struct VirtualCameraDefinition {
    std::string id = "vcam";
    std::string name = "Virtual Camera";
    glm::vec2 position{0.0f, 0.0f};
    glm::vec2 size{960.0f, 540.0f};
    std::string follow_target = "player";
    glm::vec2 follow_offset{0.0f, 0.0f};
    glm::vec2 dead_zone{160.0f, 96.0f};
    float zoom = 1.0f;
    float follow_lag = 6.0f;
    float zoom_lag = 6.0f;
    bool enabled = true;
    bool auto_activate = true;
    bool release_on_exit = true;
    bool override_mode = false;
    std::string camera_mode = "side";
    bool pseudo_3d_enabled = false;
    glm::vec2 pseudo_3d_offset{-18.0f, -14.0f};
    float pseudo_3d_top_tint = 1.08f;
    float pseudo_3d_side_tint = 0.58f;
    std::string editor_group;
};

struct PlayerCameraDefinition {
    bool enabled = false;
    std::string follow_target = "player";
    glm::vec2 follow_offset{0.0f, 0.0f};
    glm::vec2 dead_zone{160.0f, 96.0f};
    float zoom = 1.0f;
    float follow_lag = 6.0f;
    float zoom_lag = 6.0f;
    bool clamp_to_world = true;
    bool override_mode = false;
    std::string camera_mode = "side";
    bool pseudo_3d_enabled = false;
    glm::vec2 pseudo_3d_offset{-18.0f, -14.0f};
    float pseudo_3d_top_tint = 1.08f;
    float pseudo_3d_side_tint = 0.58f;
};

struct AnimationFloatKey {
    float time = 0.0f;
    float value = 1.0f;
};

struct AnimationVec2Key {
    float time = 0.0f;
    glm::vec2 value{0.0f, 0.0f};
};

struct ObjectAnimationAsset {
    std::string name;
    float duration = 1.0f;
    std::string preview_texture;
    bool affect_position = true;
    bool affect_opacity = false;
    bool affect_scale = false;
    bool affect_rotation = false;
    bool affect_skew = false;
    std::vector<glm::vec2> path_points{{0.0f, 0.0f}, {96.0f, 0.0f}};
    std::vector<AnimationFloatKey> opacity_keys{
        {.time = 0.0f, .value = 1.0f},
        {.time = 1.0f, .value = 1.0f},
    };
    std::vector<AnimationVec2Key> scale_keys{
        {.time = 0.0f, .value = {1.0f, 1.0f}},
        {.time = 1.0f, .value = {1.0f, 1.0f}},
    };
    std::vector<AnimationFloatKey> rotation_keys{
        {.time = 0.0f, .value = 0.0f},
        {.time = 1.0f, .value = 0.0f},
    };
    std::vector<AnimationVec2Key> skew_keys{
        {.time = 0.0f, .value = {0.0f, 0.0f}},
        {.time = 1.0f, .value = {0.0f, 0.0f}},
    };
};

struct SceneAnimationDefinition {
    std::string id = "scene_animation";
    std::string name = "Scene Animation";
    std::string asset;
    std::string target_entity = "player";
    bool enabled = true;
    bool play_on_start = true;
    bool loop = false;
    float speed = 1.0f;
    std::string editor_group;
};

struct MenuButtonDefinition {
    std::string label = "Button";
    std::string action = "start_game";
    std::string target;
    glm::vec2 size{280.0f, 52.0f};
    glm::vec4 color{0.22f, 0.26f, 0.36f, 1.0f};
    glm::vec4 hover_color{0.30f, 0.36f, 0.50f, 1.0f};
    glm::vec4 text_color{1.0f, 1.0f, 1.0f, 1.0f};
    bool enabled = true;
};

struct MenuDefinition {
    bool enabled = false;
    std::string title;
    std::string subtitle;
    glm::vec2 position{0.5f, 0.5f};
    glm::vec2 panel_size{520.0f, 0.0f};
    glm::vec4 background{0.08f, 0.09f, 0.13f, 0.95f};
    glm::vec4 accent{0.20f, 0.55f, 0.95f, 1.0f};
    glm::vec4 title_color{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 subtitle_color{0.82f, 0.86f, 0.92f, 1.0f};
    float roundness = 18.0f;
    float spacing = 10.0f;
    std::string background_music;
    bool music_loop = true;
    std::vector<MenuButtonDefinition> buttons;
};

struct UiElementDefinition {
    std::string name = "Widget";
    std::string text = "Label";
    glm::vec2 anchor{0.02f, 0.02f};
    glm::vec2 size{260.0f, 54.0f};
    glm::vec2 padding{14.0f, 10.0f};
    glm::vec4 background{0.05f, 0.08f, 0.14f, 0.72f};
    glm::vec4 accent{0.19f, 0.58f, 0.95f, 1.0f};
    glm::vec4 text_color{1.0f, 1.0f, 1.0f, 1.0f};
    float scale = 1.0f;
    bool enabled = true;
};

struct HtmlUiSettings {
    bool enabled = false;
    std::string html_path = "ui/index.html";
    std::string css_path = "ui/styles.css";
    std::string script_path = "ui/app.js";
};

struct LevelData {
    std::string name;
    int tile_width = 32;
    int tile_height = 32;
    glm::vec2 player_spawn{64.0f, 64.0f};
    glm::vec4 clear_color{0.08f, 0.09f, 0.12f, 1.0f};
    Tileset tileset;
    std::vector<TileLayer> tile_layers;
    std::vector<ParallaxLayer> parallax_layers;
    std::vector<EntityDefinition> entities;
    std::vector<TriggerZone> triggers;
    LightingSettings lighting;
    std::vector<LightDefinition> lights;
    std::vector<AudioSourceDefinition> audio_sources;
    std::vector<AudioPakDefinition> audio_paks;
    PlayerCameraDefinition player_camera;
    std::vector<VirtualCameraDefinition> virtual_cameras;
    std::vector<SceneAnimationDefinition> animations;
    std::vector<std::string> post_effects;
};

struct ProjectData {
    std::string name;
    std::string developer = "Unknown";
    std::string version = "0.1.0";
    std::string icon;
    std::string preview_image;
    bool splash_enabled = true;
    std::string splash_image;
    std::string startup_level;
    std::vector<std::string> levels;
    std::vector<std::string> assets;
    std::vector<std::string> scripts;
    std::vector<std::string> sprite_assets;
    std::vector<std::string> object_assets;
    std::vector<std::string> trigger_assets;
    std::vector<std::string> animation_assets;
    std::vector<std::string> particle_assets;
    std::string camera_mode = "side";
    std::string renderer_backend = "opengl";
    bool multithreading_enabled = true;
    int worker_threads = 0;
    std::string default_language = "en";
    std::string editor_language = "en";
    std::vector<std::string> supported_languages{"en", "ru"};
    glm::ivec2 game_viewport_size{1600, 900};
    glm::ivec2 editor_render_size{1600, 900};
    std::string export_directory = "Builds";
    bool encrypt_archive = false;
    std::string archive_password;
    MenuDefinition main_menu{
        .enabled = true,
        .title = "NovaIso Engine",
        .subtitle = "Python-driven platformer framework",
        .buttons = {
            MenuButtonDefinition{.label = "Start Game", .action = "start_game"},
            MenuButtonDefinition{.label = "Settings", .action = "open_settings"},
            MenuButtonDefinition{.label = "Toggle Camera", .action = "toggle_camera_mode"},
            MenuButtonDefinition{.label = "Quit", .action = "quit_game"}
        }
    };
    MenuDefinition pause_menu{
        .enabled = true,
        .title = "Paused",
        .subtitle = "Game is paused",
        .buttons = {
            MenuButtonDefinition{.label = "Resume", .action = "resume_game"},
            MenuButtonDefinition{.label = "Settings", .action = "open_settings"},
            MenuButtonDefinition{.label = "Restart Level", .action = "restart_level"},
            MenuButtonDefinition{.label = "Main Menu", .action = "open_main_menu"},
            MenuButtonDefinition{.label = "Quit", .action = "quit_game"}
        }
    };
    std::vector<UiElementDefinition> ui_elements{
        UiElementDefinition{
            .name = "TopLeftStatus",
            .text = "{project}",
            .anchor = {0.02f, 0.02f},
        },
        UiElementDefinition{
            .name = "TopRightCamera",
            .text = "Camera: {camera_mode}",
            .anchor = {0.72f, 0.02f},
        }
    };
    HtmlUiSettings html_ui;
};

ProjectData LoadProject(const std::filesystem::path& path);
LevelData LoadLevel(const std::filesystem::path& path);
LevelData LoadLevelFromText(std::string_view text, const std::filesystem::path& path_hint = {});
SpriteAsset LoadSpriteAsset(const std::filesystem::path& path);
ObjectAsset LoadObjectAsset(const std::filesystem::path& path);
TriggerAsset LoadTriggerAsset(const std::filesystem::path& path);
ObjectAnimationAsset LoadObjectAnimationAsset(const std::filesystem::path& path);
ParticleEffectAsset LoadParticleEffectAsset(const std::filesystem::path& path);

void SaveProject(const std::filesystem::path& path, const ProjectData& project);
void SaveLevel(const std::filesystem::path& path, const LevelData& level);
std::string SaveLevelToText(const LevelData& level);
void SaveSpriteAsset(const std::filesystem::path& path, const SpriteAsset& sprite);
void SaveObjectAsset(const std::filesystem::path& path, const ObjectAsset& object);
void SaveTriggerAsset(const std::filesystem::path& path, const TriggerAsset& trigger);
void SaveObjectAnimationAsset(const std::filesystem::path& path, const ObjectAnimationAsset& animation);
void SaveParticleEffectAsset(const std::filesystem::path& path, const ParticleEffectAsset& particle);

std::string ToString(CollisionType type);
CollisionType CollisionTypeFromString(std::string_view value);

}  // namespace novaiso::assets
