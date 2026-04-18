#include "assets/Project.h"

#include "core/FileIO.h"

#include <nlohmann/json.hpp>

namespace novaiso::assets {

namespace {

using json = nlohmann::json;

glm::vec2 ParseVec2(const json& value, glm::vec2 fallback = {0.0f, 0.0f}) {
    if (!value.is_array() || value.size() < 2) {
        return fallback;
    }
    return {value[0].get<float>(), value[1].get<float>()};
}

float ParseReflectionValue(const json& value, float fallback = 0.0f) {
    if (value.is_boolean()) {
        return value.get<bool>() ? 0.7f : 0.0f;
    }
    if (value.is_number_float() || value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<float>();
    }
    return fallback;
}

glm::ivec2 ParseIVec2(const json& value, glm::ivec2 fallback = {0, 0}) {
    if (!value.is_array() || value.size() < 2) {
        return fallback;
    }
    return {value[0].get<int>(), value[1].get<int>()};
}

glm::vec4 ParseVec4(const json& value, glm::vec4 fallback = {1.0f, 1.0f, 1.0f, 1.0f}) {
    if (!value.is_array() || value.size() < 4) {
        return fallback;
    }
    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>()};
}

json ToJson(const glm::vec2& value) {
    return json::array({value.x, value.y});
}

json ToJson(const glm::ivec2& value) {
    return json::array({value.x, value.y});
}

json ToJson(const glm::vec4& value) {
    return json::array({value.x, value.y, value.z, value.w});
}

TriggerCondition ParseCondition(const json& value) {
    return TriggerCondition{
        .script = value.value("script", ""),
        .function = value.value("function", ""),
        .args_json = value.value("args", "{}"),
        .editor_position = ParseVec2(value.value("editor_position", json::array({260.0f, 140.0f})), {260.0f, 140.0f}),
    };
}

TriggerAction ParseAction(const json& value) {
    return TriggerAction{
        .script = value.value("script", ""),
        .function = value.value("function", ""),
        .args_json = value.value("args", "{}"),
        .editor_position = ParseVec2(value.value("editor_position", json::array({620.0f, 140.0f})), {620.0f, 140.0f}),
    };
}

json ToJson(const TriggerCondition& condition) {
    return {
        {"script", condition.script},
        {"function", condition.function},
        {"args", condition.args_json},
        {"editor_position", ToJson(condition.editor_position)}
    };
}

json ToJson(const TriggerAction& action) {
    return {
        {"script", action.script},
        {"function", action.function},
        {"args", action.args_json},
        {"editor_position", ToJson(action.editor_position)}
    };
}

MenuButtonDefinition ParseMenuButton(const json& value) {
    MenuButtonDefinition button;
    button.label = value.value("label", "Button");
    button.action = value.value("action", "start_game");
    button.target = value.value("target", "");
    button.size = ParseVec2(value.value("size", json::array({280.0f, 52.0f})), {280.0f, 52.0f});
    button.color = ParseVec4(value.value("color", json::array({0.22f, 0.26f, 0.36f, 1.0f})));
    button.hover_color = ParseVec4(value.value("hover_color", json::array({0.30f, 0.36f, 0.50f, 1.0f})));
    button.text_color = ParseVec4(value.value("text_color", json::array({1.0f, 1.0f, 1.0f, 1.0f})));
    button.enabled = value.value("enabled", true);
    return button;
}

json ToJson(const MenuButtonDefinition& button) {
    return {
        {"label", button.label},
        {"action", button.action},
        {"target", button.target},
        {"size", ToJson(button.size)},
        {"color", ToJson(button.color)},
        {"hover_color", ToJson(button.hover_color)},
        {"text_color", ToJson(button.text_color)},
        {"enabled", button.enabled}
    };
}

MenuDefinition ParseMenuDefinition(const json& value, const MenuDefinition& fallback) {
    MenuDefinition menu = fallback;
    menu.enabled = value.value("enabled", fallback.enabled);
    menu.title = value.value("title", fallback.title);
    menu.subtitle = value.value("subtitle", fallback.subtitle);
    menu.position = ParseVec2(value.value("position", ToJson(fallback.position)), fallback.position);
    menu.panel_size = ParseVec2(value.value("panel_size", ToJson(fallback.panel_size)), fallback.panel_size);
    menu.background = ParseVec4(value.value("background", ToJson(fallback.background)), fallback.background);
    menu.accent = ParseVec4(value.value("accent", ToJson(fallback.accent)), fallback.accent);
    menu.title_color = ParseVec4(value.value("title_color", ToJson(fallback.title_color)), fallback.title_color);
    menu.subtitle_color = ParseVec4(value.value("subtitle_color", ToJson(fallback.subtitle_color)), fallback.subtitle_color);
    menu.roundness = value.value("roundness", fallback.roundness);
    menu.spacing = value.value("spacing", fallback.spacing);
    menu.background_music = value.value("background_music", fallback.background_music);
    menu.music_loop = value.value("music_loop", fallback.music_loop);
    menu.buttons.clear();
    for (const auto& button_json : value.value("buttons", json::array())) {
        menu.buttons.push_back(ParseMenuButton(button_json));
    }
    if (menu.buttons.empty()) {
        menu.buttons = fallback.buttons;
    }
    return menu;
}

json ToJson(const MenuDefinition& menu) {
    json root{
        {"enabled", menu.enabled},
        {"title", menu.title},
        {"subtitle", menu.subtitle},
        {"position", ToJson(menu.position)},
        {"panel_size", ToJson(menu.panel_size)},
        {"background", ToJson(menu.background)},
        {"accent", ToJson(menu.accent)},
        {"title_color", ToJson(menu.title_color)},
        {"subtitle_color", ToJson(menu.subtitle_color)},
        {"roundness", menu.roundness},
        {"spacing", menu.spacing},
        {"background_music", menu.background_music},
        {"music_loop", menu.music_loop},
        {"buttons", json::array()}
    };
    for (const auto& button : menu.buttons) {
        root["buttons"].push_back(ToJson(button));
    }
    return root;
}

LightingSettings ParseLightingSettings(const json& value, const LightingSettings& fallback) {
    LightingSettings lighting = fallback;
    lighting.enabled = value.value("enabled", fallback.enabled);
    lighting.rt_enabled = value.value("rt_enabled", fallback.rt_enabled);
    lighting.ambient_color = ParseVec4(value.value("ambient_color", ToJson(fallback.ambient_color)), fallback.ambient_color);
    lighting.ambient_intensity = value.value("ambient_intensity", fallback.ambient_intensity);
    lighting.shadow_strength = value.value("shadow_strength", fallback.shadow_strength);
    lighting.shadow_softness = value.value("shadow_softness", fallback.shadow_softness);
    lighting.shadow_diffusion = value.value("shadow_diffusion", fallback.shadow_diffusion);
    lighting.shadow_samples = value.value("shadow_samples", fallback.shadow_samples);
    return lighting;
}

json ToJson(const LightingSettings& lighting) {
    return {
        {"enabled", lighting.enabled},
        {"rt_enabled", lighting.rt_enabled},
        {"ambient_color", ToJson(lighting.ambient_color)},
        {"ambient_intensity", lighting.ambient_intensity},
        {"shadow_strength", lighting.shadow_strength},
        {"shadow_softness", lighting.shadow_softness},
        {"shadow_diffusion", lighting.shadow_diffusion},
        {"shadow_samples", lighting.shadow_samples}
    };
}

LightDefinition ParseLightDefinition(const json& value) {
    LightDefinition light;
    light.id = value.value("id", "light");
    light.name = value.value("name", "Lamp");
    light.type = value.value("type", "point");
    light.position = ParseVec2(value.value("position", json::array({0.0f, 0.0f})));
    light.radius = value.value("radius", 280.0f);
    light.length = value.value(
        "length",
        light.type == "flashlight" || light.type == "spot" || light.type == "spotlight" || light.type == "directional"
            ? std::max(light.radius * 1.85f, 520.0f)
            : light.radius);
    light.source_radius = value.value("source_radius", 26.0f);
    light.scatter = value.value("scatter", 1.0f);
    light.direction_degrees = value.value("direction_degrees", -35.0f);
    light.cone_angle = value.value("cone_angle", 42.0f);
    light.cone_softness = value.value("cone_softness", 0.28f);
    light.color = ParseVec4(value.value("color", json::array({1.0f, 0.85f, 0.55f, 0.9f})));
    light.intensity = value.value("intensity", 1.0f);
    light.enabled = value.value("enabled", true);
    light.attached_to = value.value("attached_to", "");
    light.attach_offset = ParseVec2(value.value("attach_offset", json::array({0.0f, 0.0f})));
    light.editor_group = value.value("editor_group", "");
    return light;
}

json ToJson(const LightDefinition& light) {
    return {
        {"id", light.id},
        {"name", light.name},
        {"type", light.type},
        {"position", ToJson(light.position)},
        {"radius", light.radius},
        {"length", light.length},
        {"source_radius", light.source_radius},
        {"scatter", light.scatter},
        {"direction_degrees", light.direction_degrees},
        {"cone_angle", light.cone_angle},
        {"cone_softness", light.cone_softness},
        {"color", ToJson(light.color)},
        {"intensity", light.intensity},
        {"enabled", light.enabled},
        {"attached_to", light.attached_to},
        {"attach_offset", ToJson(light.attach_offset)},
        {"editor_group", light.editor_group}
    };
}

AudioSourceDefinition ParseAudioSourceDefinition(const json& value) {
    AudioSourceDefinition source;
    source.id = value.value("id", "audio_source");
    source.name = value.value("name", "Audio Source");
    source.audio = value.value("audio", "");
    source.position = ParseVec2(value.value("position", json::array({0.0f, 0.0f})));
    source.enabled = value.value("enabled", true);
    source.always = value.value("always", false);
    source.loop = value.value("loop", false);
    source.radius = value.value("radius", 180.0f);
    source.stop_on_exit = value.value("stop_on_exit", true);
    source.distance = value.value("distance", 620.0f);
    source.volume = value.value("volume", 1.0f);
    source.editor_group = value.value("editor_group", "");
    return source;
}

json ToJson(const AudioSourceDefinition& source) {
    return {
        {"id", source.id},
        {"name", source.name},
        {"audio", source.audio},
        {"position", ToJson(source.position)},
        {"enabled", source.enabled},
        {"always", source.always},
        {"loop", source.loop},
        {"radius", source.radius},
        {"stop_on_exit", source.stop_on_exit},
        {"distance", source.distance},
        {"volume", source.volume},
        {"editor_group", source.editor_group}
    };
}

AudioPakDefinition ParseAudioPakDefinition(const json& value) {
    AudioPakDefinition pak;
    pak.id = value.value("id", "audio_pak");
    pak.name = value.value("name", "Audio Pak");
    pak.position = ParseVec2(value.value("position", json::array({0.0f, 0.0f})));
    pak.enabled = value.value("enabled", true);
    pak.always = value.value("always", true);
    pak.loop = value.value("loop", false);
    pak.radius = value.value("radius", 420.0f);
    pak.stop_on_exit = value.value("stop_on_exit", false);
    pak.distance = value.value("distance", 1200.0f);
    pak.volume = value.value("volume", 1.0f);
    pak.shuffle = value.value("shuffle", false);
    pak.repeat_playlist = value.value("repeat_playlist", true);
    pak.tracks = value.value("tracks", std::vector<std::string>{});
    pak.editor_group = value.value("editor_group", "");
    return pak;
}

json ToJson(const AudioPakDefinition& pak) {
    return {
        {"id", pak.id},
        {"name", pak.name},
        {"position", ToJson(pak.position)},
        {"enabled", pak.enabled},
        {"always", pak.always},
        {"loop", pak.loop},
        {"radius", pak.radius},
        {"stop_on_exit", pak.stop_on_exit},
        {"distance", pak.distance},
        {"volume", pak.volume},
        {"shuffle", pak.shuffle},
        {"repeat_playlist", pak.repeat_playlist},
        {"tracks", pak.tracks},
        {"editor_group", pak.editor_group}
    };
}

VirtualCameraDefinition ParseVirtualCameraDefinition(const json& value) {
    VirtualCameraDefinition camera;
    camera.id = value.value("id", "vcam");
    camera.name = value.value("name", "Virtual Camera");
    camera.position = ParseVec2(value.value("position", json::array({0.0f, 0.0f})));
    camera.size = ParseVec2(value.value("size", json::array({960.0f, 540.0f})), {960.0f, 540.0f});
    camera.follow_target = value.value("follow_target", "player");
    camera.follow_offset = ParseVec2(value.value("follow_offset", json::array({0.0f, 0.0f})));
    camera.dead_zone = ParseVec2(value.value("dead_zone", json::array({160.0f, 96.0f})), {160.0f, 96.0f});
    camera.zoom = value.value("zoom", 1.0f);
    camera.follow_lag = value.value("follow_lag", 6.0f);
    camera.zoom_lag = value.value("zoom_lag", 6.0f);
    camera.enabled = value.value("enabled", true);
    camera.auto_activate = value.value("auto_activate", true);
    camera.release_on_exit = value.value("release_on_exit", true);
    camera.override_mode = value.value("override_mode", false);
    camera.camera_mode = value.value("camera_mode", "side");
    camera.pseudo_3d_enabled = value.value("pseudo_3d_enabled", false);
    camera.pseudo_3d_offset = ParseVec2(value.value("pseudo_3d_offset", json::array({-18.0f, -14.0f})), {-18.0f, -14.0f});
    camera.pseudo_3d_top_tint = value.value("pseudo_3d_top_tint", 1.08f);
    camera.pseudo_3d_side_tint = value.value("pseudo_3d_side_tint", 0.58f);
    camera.editor_group = value.value("editor_group", "");
    return camera;
}

json ToJson(const VirtualCameraDefinition& camera) {
    return {
        {"id", camera.id},
        {"name", camera.name},
        {"position", ToJson(camera.position)},
        {"size", ToJson(camera.size)},
        {"follow_target", camera.follow_target},
        {"follow_offset", ToJson(camera.follow_offset)},
        {"dead_zone", ToJson(camera.dead_zone)},
        {"zoom", camera.zoom},
        {"follow_lag", camera.follow_lag},
        {"zoom_lag", camera.zoom_lag},
        {"enabled", camera.enabled},
        {"auto_activate", camera.auto_activate},
        {"release_on_exit", camera.release_on_exit},
        {"override_mode", camera.override_mode},
        {"camera_mode", camera.camera_mode},
        {"pseudo_3d_enabled", camera.pseudo_3d_enabled},
        {"pseudo_3d_offset", ToJson(camera.pseudo_3d_offset)},
        {"pseudo_3d_top_tint", camera.pseudo_3d_top_tint},
        {"pseudo_3d_side_tint", camera.pseudo_3d_side_tint},
        {"editor_group", camera.editor_group}
    };
}

PlayerCameraDefinition ParsePlayerCameraDefinition(const json& value) {
    PlayerCameraDefinition camera;
    camera.enabled = value.value("enabled", false);
    camera.follow_target = value.value("follow_target", "player");
    camera.follow_offset = ParseVec2(value.value("follow_offset", json::array({0.0f, 0.0f})));
    camera.dead_zone = ParseVec2(value.value("dead_zone", json::array({160.0f, 96.0f})), {160.0f, 96.0f});
    camera.zoom = value.value("zoom", 1.0f);
    camera.follow_lag = value.value("follow_lag", 6.0f);
    camera.zoom_lag = value.value("zoom_lag", 6.0f);
    camera.clamp_to_world = value.value("clamp_to_world", true);
    camera.override_mode = value.value("override_mode", false);
    camera.camera_mode = value.value("camera_mode", "side");
    camera.pseudo_3d_enabled = value.value("pseudo_3d_enabled", false);
    camera.pseudo_3d_offset = ParseVec2(value.value("pseudo_3d_offset", json::array({-18.0f, -14.0f})), {-18.0f, -14.0f});
    camera.pseudo_3d_top_tint = value.value("pseudo_3d_top_tint", 1.08f);
    camera.pseudo_3d_side_tint = value.value("pseudo_3d_side_tint", 0.58f);
    return camera;
}

json ToJson(const PlayerCameraDefinition& camera) {
    return {
        {"enabled", camera.enabled},
        {"follow_target", camera.follow_target},
        {"follow_offset", ToJson(camera.follow_offset)},
        {"dead_zone", ToJson(camera.dead_zone)},
        {"zoom", camera.zoom},
        {"follow_lag", camera.follow_lag},
        {"zoom_lag", camera.zoom_lag},
        {"clamp_to_world", camera.clamp_to_world},
        {"override_mode", camera.override_mode},
        {"camera_mode", camera.camera_mode},
        {"pseudo_3d_enabled", camera.pseudo_3d_enabled},
        {"pseudo_3d_offset", ToJson(camera.pseudo_3d_offset)},
        {"pseudo_3d_top_tint", camera.pseudo_3d_top_tint},
        {"pseudo_3d_side_tint", camera.pseudo_3d_side_tint}
    };
}

AnimationFloatKey ParseAnimationFloatKey(const json& value, AnimationFloatKey fallback = {}) {
    AnimationFloatKey key = fallback;
    key.time = value.value("time", fallback.time);
    key.value = value.value("value", fallback.value);
    return key;
}

AnimationVec2Key ParseAnimationVec2Key(const json& value, AnimationVec2Key fallback = {}) {
    AnimationVec2Key key = fallback;
    key.time = value.value("time", fallback.time);
    key.value = ParseVec2(value.value("value", ToJson(fallback.value)), fallback.value);
    return key;
}

json ToJson(const AnimationFloatKey& key) {
    return {
        {"time", key.time},
        {"value", key.value}
    };
}

json ToJson(const AnimationVec2Key& key) {
    return {
        {"time", key.time},
        {"value", ToJson(key.value)}
    };
}

ObjectAnimationAsset ParseObjectAnimationAsset(const json& value) {
    ObjectAnimationAsset animation;
    animation.name = value.value("name", "animation");
    animation.duration = value.value("duration", 1.0f);
    animation.preview_texture = value.value("preview_texture", "");
    animation.affect_position = value.value("affect_position", true);
    animation.affect_opacity = value.value("affect_opacity", false);
    animation.affect_scale = value.value("affect_scale", false);
    animation.affect_rotation = value.value("affect_rotation", false);
    animation.affect_skew = value.value("affect_skew", false);
    animation.path_points.clear();
    for (const auto& point_json : value.value("path_points", json::array())) {
        animation.path_points.push_back(ParseVec2(point_json));
    }
    if (animation.path_points.empty()) {
        animation.path_points = {{0.0f, 0.0f}, {96.0f, 0.0f}};
    }
    animation.opacity_keys.clear();
    for (const auto& key_json : value.value("opacity_keys", json::array())) {
        animation.opacity_keys.push_back(ParseAnimationFloatKey(key_json, {.time = 0.0f, .value = 1.0f}));
    }
    if (animation.opacity_keys.empty()) {
        animation.opacity_keys = {
            {.time = 0.0f, .value = 1.0f},
            {.time = 1.0f, .value = 1.0f},
        };
    }
    animation.scale_keys.clear();
    for (const auto& key_json : value.value("scale_keys", json::array())) {
        animation.scale_keys.push_back(ParseAnimationVec2Key(key_json, {.time = 0.0f, .value = {1.0f, 1.0f}}));
    }
    if (animation.scale_keys.empty()) {
        animation.scale_keys = {
            {.time = 0.0f, .value = {1.0f, 1.0f}},
            {.time = 1.0f, .value = {1.0f, 1.0f}},
        };
    }
    animation.rotation_keys.clear();
    for (const auto& key_json : value.value("rotation_keys", json::array())) {
        animation.rotation_keys.push_back(ParseAnimationFloatKey(key_json, {.time = 0.0f, .value = 0.0f}));
    }
    if (animation.rotation_keys.empty()) {
        animation.rotation_keys = {
            {.time = 0.0f, .value = 0.0f},
            {.time = 1.0f, .value = 0.0f},
        };
    }
    animation.skew_keys.clear();
    for (const auto& key_json : value.value("skew_keys", json::array())) {
        animation.skew_keys.push_back(ParseAnimationVec2Key(key_json, {.time = 0.0f, .value = {0.0f, 0.0f}}));
    }
    if (animation.skew_keys.empty()) {
        animation.skew_keys = {
            {.time = 0.0f, .value = {0.0f, 0.0f}},
            {.time = 1.0f, .value = {0.0f, 0.0f}},
        };
    }
    return animation;
}

json ToJson(const ObjectAnimationAsset& animation) {
    json root{
        {"name", animation.name},
        {"duration", animation.duration},
        {"preview_texture", animation.preview_texture},
        {"affect_position", animation.affect_position},
        {"affect_opacity", animation.affect_opacity},
        {"affect_scale", animation.affect_scale},
        {"affect_rotation", animation.affect_rotation},
        {"affect_skew", animation.affect_skew},
        {"path_points", json::array()},
        {"opacity_keys", json::array()},
        {"scale_keys", json::array()},
        {"rotation_keys", json::array()},
        {"skew_keys", json::array()}
    };
    for (const auto& point : animation.path_points) {
        root["path_points"].push_back(ToJson(point));
    }
    for (const auto& key : animation.opacity_keys) {
        root["opacity_keys"].push_back(ToJson(key));
    }
    for (const auto& key : animation.scale_keys) {
        root["scale_keys"].push_back(ToJson(key));
    }
    for (const auto& key : animation.rotation_keys) {
        root["rotation_keys"].push_back(ToJson(key));
    }
    for (const auto& key : animation.skew_keys) {
        root["skew_keys"].push_back(ToJson(key));
    }
    return root;
}

ParticleEffectAsset ParseParticleEffectAsset(const json& value) {
    ParticleEffectAsset particle;
    particle.name = value.value("name", "particle");
    particle.preset = value.value("preset", "custom");
    particle.texture = value.value("texture", "");
    particle.sound = value.value("sound", "");
    particle.start_color = ParseVec4(value.value("start_color", ToJson(particle.start_color)), particle.start_color);
    particle.end_color = ParseVec4(value.value("end_color", ToJson(particle.end_color)), particle.end_color);
    particle.acceleration = ParseVec2(value.value("acceleration", ToJson(particle.acceleration)), particle.acceleration);
    particle.start_size = ParseVec2(value.value("start_size", ToJson(particle.start_size)), particle.start_size);
    particle.end_size = ParseVec2(value.value("end_size", ToJson(particle.end_size)), particle.end_size);
    particle.velocity_angle_min = value.value("velocity_angle_min", particle.velocity_angle_min);
    particle.velocity_angle_max = value.value("velocity_angle_max", particle.velocity_angle_max);
    particle.speed_min = value.value("speed_min", particle.speed_min);
    particle.speed_max = value.value("speed_max", particle.speed_max);
    particle.lifetime_min = value.value("lifetime_min", particle.lifetime_min);
    particle.lifetime_max = value.value("lifetime_max", particle.lifetime_max);
    particle.spawn_rate = value.value("spawn_rate", particle.spawn_rate);
    particle.burst_count = value.value("burst_count", particle.burst_count);
    particle.emission_radius = value.value("emission_radius", particle.emission_radius);
    particle.drag = value.value("drag", particle.drag);
    particle.angular_velocity_min = value.value("angular_velocity_min", particle.angular_velocity_min);
    particle.angular_velocity_max = value.value("angular_velocity_max", particle.angular_velocity_max);
    particle.loop = value.value("loop", particle.loop);
    particle.additive = value.value("additive", particle.additive);
    particle.align_to_velocity = value.value("align_to_velocity", particle.align_to_velocity);
    return particle;
}

json ToJson(const ParticleEffectAsset& particle) {
    return {
        {"name", particle.name},
        {"preset", particle.preset},
        {"texture", particle.texture},
        {"sound", particle.sound},
        {"start_color", ToJson(particle.start_color)},
        {"end_color", ToJson(particle.end_color)},
        {"acceleration", ToJson(particle.acceleration)},
        {"start_size", ToJson(particle.start_size)},
        {"end_size", ToJson(particle.end_size)},
        {"velocity_angle_min", particle.velocity_angle_min},
        {"velocity_angle_max", particle.velocity_angle_max},
        {"speed_min", particle.speed_min},
        {"speed_max", particle.speed_max},
        {"lifetime_min", particle.lifetime_min},
        {"lifetime_max", particle.lifetime_max},
        {"spawn_rate", particle.spawn_rate},
        {"burst_count", particle.burst_count},
        {"emission_radius", particle.emission_radius},
        {"drag", particle.drag},
        {"angular_velocity_min", particle.angular_velocity_min},
        {"angular_velocity_max", particle.angular_velocity_max},
        {"loop", particle.loop},
        {"additive", particle.additive},
        {"align_to_velocity", particle.align_to_velocity}
    };
}

SceneAnimationDefinition ParseSceneAnimationDefinition(const json& value) {
    SceneAnimationDefinition animation;
    animation.id = value.value("id", "scene_animation");
    animation.name = value.value("name", "Scene Animation");
    animation.asset = value.value("asset", "");
    animation.target_entity = value.value("target_entity", "player");
    animation.enabled = value.value("enabled", true);
    animation.play_on_start = value.value("play_on_start", true);
    animation.loop = value.value("loop", false);
    animation.speed = value.value("speed", 1.0f);
    animation.editor_group = value.value("editor_group", "");
    return animation;
}

json ToJson(const SceneAnimationDefinition& animation) {
    return {
        {"id", animation.id},
        {"name", animation.name},
        {"asset", animation.asset},
        {"target_entity", animation.target_entity},
        {"enabled", animation.enabled},
        {"play_on_start", animation.play_on_start},
        {"loop", animation.loop},
        {"speed", animation.speed},
        {"editor_group", animation.editor_group}
    };
}

UiElementDefinition ParseUiElement(const json& value) {
    UiElementDefinition widget;
    widget.name = value.value("name", "Widget");
    widget.text = value.value("text", "Label");
    widget.anchor = ParseVec2(value.value("anchor", json::array({0.02f, 0.02f})), {0.02f, 0.02f});
    widget.size = ParseVec2(value.value("size", json::array({260.0f, 54.0f})), {260.0f, 54.0f});
    widget.padding = ParseVec2(value.value("padding", json::array({14.0f, 10.0f})), {14.0f, 10.0f});
    widget.background = ParseVec4(value.value("background", json::array({0.05f, 0.08f, 0.14f, 0.72f})));
    widget.accent = ParseVec4(value.value("accent", json::array({0.19f, 0.58f, 0.95f, 1.0f})));
    widget.text_color = ParseVec4(value.value("text_color", json::array({1.0f, 1.0f, 1.0f, 1.0f})));
    widget.scale = value.value("scale", 1.0f);
    widget.enabled = value.value("enabled", true);
    return widget;
}

json ToJson(const UiElementDefinition& widget) {
    return {
        {"name", widget.name},
        {"text", widget.text},
        {"anchor", ToJson(widget.anchor)},
        {"size", ToJson(widget.size)},
        {"padding", ToJson(widget.padding)},
        {"background", ToJson(widget.background)},
        {"accent", ToJson(widget.accent)},
        {"text_color", ToJson(widget.text_color)},
        {"scale", widget.scale},
        {"enabled", widget.enabled}
    };
}

HtmlUiSettings ParseHtmlUiSettings(const json& value, const HtmlUiSettings& fallback) {
    HtmlUiSettings settings = fallback;
    settings.enabled = value.value("enabled", fallback.enabled);
    settings.html_path = value.value("html_path", fallback.html_path);
    settings.css_path = value.value("css_path", fallback.css_path);
    settings.script_path = value.value("script_path", fallback.script_path);
    return settings;
}

json ToJson(const HtmlUiSettings& settings) {
    return {
        {"enabled", settings.enabled},
        {"html_path", settings.html_path},
        {"css_path", settings.css_path},
        {"script_path", settings.script_path}
    };
}

}  // namespace

std::string ToString(CollisionType type) {
    switch (type) {
        case CollisionType::Solid:
            return "solid";
        case CollisionType::OneWay:
            return "one_way";
        case CollisionType::SlopeLeft:
            return "slope_left";
        case CollisionType::SlopeRight:
            return "slope_right";
        case CollisionType::Empty:
        default:
            return "empty";
    }
}

CollisionType CollisionTypeFromString(std::string_view value) {
    if (value == "solid") {
        return CollisionType::Solid;
    }
    if (value == "one_way") {
        return CollisionType::OneWay;
    }
    if (value == "slope_left") {
        return CollisionType::SlopeLeft;
    }
    if (value == "slope_right") {
        return CollisionType::SlopeRight;
    }
    return CollisionType::Empty;
}

ProjectData LoadProject(const std::filesystem::path& path) {
    const json root = json::parse(core::FileIO::ReadText(path));
    ProjectData project;
    project.name = root.value("name", "NovaIso Project");
    project.developer = root.value("developer", "Unknown");
    project.version = root.value("version", "0.1.0");
    project.icon = root.value("icon", "");
    project.preview_image = root.value("preview_image", "");
    project.splash_enabled = root.value("splash_enabled", true);
    project.splash_image = root.value("splash_image", "");
    project.startup_level = root.value("startup_level", "");
    project.camera_mode = root.value("camera_mode", "side");
    project.levels = root.value("levels", std::vector<std::string>{});
    project.assets = root.value("assets", std::vector<std::string>{});
    project.scripts = root.value("scripts", std::vector<std::string>{});
    project.sprite_assets = root.value("sprite_assets", std::vector<std::string>{});
    project.object_assets = root.value("object_assets", std::vector<std::string>{});
    project.trigger_assets = root.value("trigger_assets", std::vector<std::string>{});
    project.animation_assets = root.value("animation_assets", std::vector<std::string>{});
    project.particle_assets = root.value("particle_assets", std::vector<std::string>{});
    project.default_language = root.value("default_language", "en");
    project.editor_language = root.value("editor_language", "en");
    project.renderer_backend = root.value("renderer_backend", "opengl");
    project.multithreading_enabled = root.value("multithreading_enabled", true);
    project.worker_threads = root.value("worker_threads", 0);
    project.supported_languages = root.value("supported_languages", std::vector<std::string>{"en", "ru"});
    project.game_viewport_size = ParseIVec2(root.value("game_viewport_size", root.value("editor_render_size", json::array({1600, 900}))), {1600, 900});
    project.editor_render_size = ParseIVec2(root.value("editor_render_size", json::array({1600, 900})), {1600, 900});
    project.export_directory = root.value("export_directory", "Builds");
    project.encrypt_archive = root.value("encrypt_archive", false);
    project.archive_password = root.value("archive_password", "");
    project.main_menu = ParseMenuDefinition(root.value("main_menu", json::object()), project.main_menu);
    project.pause_menu = ParseMenuDefinition(root.value("pause_menu", json::object()), project.pause_menu);
    project.ui_elements.clear();
    for (const auto& widget_json : root.value("ui_elements", json::array())) {
        project.ui_elements.push_back(ParseUiElement(widget_json));
    }
    if (project.ui_elements.empty()) {
        project.ui_elements = ProjectData{}.ui_elements;
    }
    project.html_ui = ParseHtmlUiSettings(root.value("html_ui", json::object()), project.html_ui);
    return project;
}

LevelData LoadLevelFromText(std::string_view text, const std::filesystem::path& path_hint) {
    const json root = json::parse(text);
    LevelData level;
    level.name = root.value("name", path_hint.empty() ? std::string("level") : path_hint.stem().string());
    level.tile_width = root.value("tile_width", 32);
    level.tile_height = root.value("tile_height", 32);
    level.player_spawn = ParseVec2(root.value("player_spawn", json::array({64.0f, 64.0f})), {64.0f, 64.0f});
    level.clear_color = ParseVec4(root.value("clear_color", json::array({0.08f, 0.09f, 0.12f, 1.0f})));
    level.post_effects = root.value("post_effects", std::vector<std::string>{});
    level.lighting = ParseLightingSettings(root.value("lighting", json::object()), level.lighting);

    const json tileset_json = root.value("tileset", json::object());
    level.tileset.texture = tileset_json.value("texture", "");
    level.tileset.tile_width = tileset_json.value("tile_width", level.tile_width);
    level.tileset.tile_height = tileset_json.value("tile_height", level.tile_height);
    level.tileset.columns = tileset_json.value("columns", 1);
    level.tileset.rows = tileset_json.value("rows", 1);
    if (tileset_json.contains("collision")) {
        for (const auto& entry : tileset_json["collision"]) {
            const int id = entry.value("id", 0);
            level.tileset.tile_info[id] = TileInfo{id, CollisionTypeFromString(entry.value("type", "empty"))};
        }
    }

    for (const auto& layer_json : root.value("tile_layers", json::array())) {
        TileLayer layer;
        layer.name = layer_json.value("name", "Layer");
        layer.width = layer_json.value("width", 0);
        layer.height = layer_json.value("height", 0);
        layer.depth = layer_json.value("depth", 0.0f);
        layer.visible = layer_json.value("visible", true);
        layer.collidable = layer_json.value("collidable", false);
        layer.tiles = layer_json.value("tiles", std::vector<int>{});
        level.tile_layers.push_back(std::move(layer));
    }

    for (const auto& layer_json : root.value("parallax_layers", json::array())) {
        ParallaxLayer layer;
        layer.id = layer_json.value("id", "parallax_" + std::to_string(level.parallax_layers.size()));
        layer.name = layer_json.value("name", "Parallax");
        layer.texture = layer_json.value("texture", "");
        layer.speed = ParseVec2(layer_json.value("speed", json::array({0.25f, 0.0f})), {0.25f, 0.0f});
        layer.scale = ParseVec2(layer_json.value("scale", json::array({1.0f, 1.0f})), {1.0f, 1.0f});
        layer.offset = ParseVec2(layer_json.value("offset", json::array({0.0f, 0.0f})));
        layer.tint = ParseVec4(layer_json.value("tint", json::array({1.0f, 1.0f, 1.0f, 1.0f})));
        layer.depth = layer_json.value("depth", -200.0f);
        layer.zoom_factor = layer_json.value("zoom_factor", 1.0f);
        layer.receives_lighting = layer_json.value("receives_lighting", true);
        layer.lighting_response = layer_json.value("lighting_response", 1.0f);
        layer.artificial_light = layer_json.value("artificial_light", false);
        layer.artificial_light_color = ParseVec4(
            layer_json.value("artificial_light_color", json::array({1.0f, 0.92f, 0.78f, 1.0f})),
            {1.0f, 0.92f, 0.78f, 1.0f});
        layer.artificial_light_strength = layer_json.value("artificial_light_strength", 0.0f);
        layer.repeat = layer_json.value("repeat", true);
        layer.visible = layer_json.value("visible", true);
        layer.editor_group = layer_json.value("editor_group", "");
        level.parallax_layers.push_back(std::move(layer));
    }

    for (const auto& entity_json : root.value("entities", json::array())) {
        EntityDefinition entity;
        entity.id = entity_json.value("id", "");
        entity.archetype = entity_json.value("archetype", "generic");
        entity.object_asset = entity_json.value("object_asset", "");
        entity.sprite_asset = entity_json.value("sprite_asset", "");
        entity.animation = entity_json.value("animation", "idle");
        entity.position = ParseVec2(entity_json.value("position", json::array({0.0f, 0.0f})));
        entity.size = ParseVec2(entity_json.value("size", json::array({32.0f, 32.0f})), {32.0f, 32.0f});
        entity.velocity = ParseVec2(entity_json.value("velocity", json::array({0.0f, 0.0f})));
        entity.tint = ParseVec4(entity_json.value("tint", json::array({1.0f, 1.0f, 1.0f, 1.0f})));
        entity.texture = entity_json.value("texture", "");
        entity.sound = entity_json.value("sound", "");
        entity.uv = ParseVec4(entity_json.value("uv", json::array({0.0f, 0.0f, 1.0f, 1.0f})));
        entity.layer = entity_json.value("layer", 0);
        entity.dynamic = entity_json.value("dynamic", false);
        entity.collidable = entity_json.value("collidable", false);
        entity.visible = entity_json.value("visible", true);
        entity.reflection = entity_json.contains("reflection") ? ParseReflectionValue(entity_json["reflection"], 0.0f) : 0.0f;
        entity.normal_map = entity_json.value("normal_map", "");
        entity.height_map = entity_json.value("height_map", "");
        entity.displacement_map = entity_json.value("displacement_map", "");
        entity.relief_enabled = entity_json.value("relief_enabled", false);
        entity.bump_strength = entity_json.value("bump_strength", 1.0f);
        entity.relief_depth = entity_json.value("relief_depth", 0.035f);
        entity.parallax_depth = entity_json.value("parallax_depth", 0.018f);
        entity.relief_contrast = entity_json.value("relief_contrast", 1.35f);
        entity.pseudo_3d = entity_json.value("pseudo_3d", false);
        entity.collider_offset = ParseVec2(entity_json.value("collider_offset", json::array({0.0f, 0.0f})));
        entity.collider_size = ParseVec2(entity_json.value("collider_size", json::array({entity.size.x, entity.size.y})), entity.size);
        entity.pseudo_3d_height = entity_json.value("pseudo_3d_height", 16.0f);
        entity.animation_speed = entity_json.value("animation_speed", 1.0f);
        entity.script = entity_json.value("script", "");
        entity.on_trigger = entity_json.value("on_trigger", "");
        entity.attached_to = entity_json.value("attached_to", "");
        entity.attach_offset = ParseVec2(entity_json.value("attach_offset", json::array({0.0f, 0.0f})));
        entity.properties = entity_json.value("properties", json::object());
        entity.editor_group = entity_json.value("editor_group", "");
        level.entities.push_back(std::move(entity));
    }

    for (const auto& trigger_json : root.value("triggers", json::array())) {
        TriggerZone trigger;
        trigger.id = trigger_json.value("id", "trigger_" + std::to_string(level.triggers.size()));
        trigger.name = trigger_json.value("name", "Trigger");
        trigger.asset = trigger_json.value("asset", "");
        trigger.position = ParseVec2(trigger_json.value("position", json::array({0.0f, 0.0f})));
        trigger.size = ParseVec2(trigger_json.value("size", json::array({64.0f, 64.0f})), {64.0f, 64.0f});
        trigger.color = ParseVec4(trigger_json.value("color", json::array({1.0f, 0.7f, 0.1f, 1.0f})));
        trigger.once = trigger_json.value("once", false);
        trigger.enabled = trigger_json.value("enabled", true);
        trigger.editor_group = trigger_json.value("editor_group", "");
        for (const auto& condition_json : trigger_json.value("conditions", json::array())) {
            trigger.conditions.push_back(ParseCondition(condition_json));
        }
        for (const auto& action_json : trigger_json.value("actions", json::array())) {
            trigger.actions.push_back(ParseAction(action_json));
        }
        level.triggers.push_back(std::move(trigger));
    }

    for (const auto& light_json : root.value("lights", json::array())) {
        level.lights.push_back(ParseLightDefinition(light_json));
    }

    for (const auto& source_json : root.value("audio_sources", json::array())) {
        level.audio_sources.push_back(ParseAudioSourceDefinition(source_json));
    }

    for (const auto& pak_json : root.value("audio_paks", json::array())) {
        level.audio_paks.push_back(ParseAudioPakDefinition(pak_json));
    }

    level.player_camera = ParsePlayerCameraDefinition(root.value("player_camera", json::object()));

    for (const auto& camera_json : root.value("virtual_cameras", json::array())) {
        level.virtual_cameras.push_back(ParseVirtualCameraDefinition(camera_json));
    }

    for (const auto& animation_json : root.value("animations", json::array())) {
        level.animations.push_back(ParseSceneAnimationDefinition(animation_json));
    }

    return level;
}

LevelData LoadLevel(const std::filesystem::path& path) {
    return LoadLevelFromText(core::FileIO::ReadText(path), path);
}

SpriteAsset LoadSpriteAsset(const std::filesystem::path& path) {
    const json root = json::parse(core::FileIO::ReadText(path));
    SpriteAsset sprite;
    sprite.name = root.value("name", path.stem().string());
    sprite.canvas_size = ParseIVec2(root.value("canvas_size", json::array({16, 16})), {16, 16});
    sprite.default_animation = root.value("default_animation", "idle");
    sprite.pivot = ParseVec2(root.value("pivot", json::array({0.0f, 0.0f})));
    sprite.tint = ParseVec4(root.value("tint", json::array({1.0f, 1.0f, 1.0f, 1.0f})));

    for (const auto& animation_json : root.value("animations", json::array())) {
        SpriteAnimation animation;
        animation.name = animation_json.value("name", "idle");
        animation.loop = animation_json.value("loop", true);
        for (const auto& frame_json : animation_json.value("frames", json::array())) {
            animation.frames.push_back(SpriteFrame{
                .texture = frame_json.value("texture", ""),
                .duration = frame_json.value("duration", 0.1f),
            });
        }
        sprite.animations.push_back(std::move(animation));
    }

    if (sprite.animations.empty() && root.contains("frames")) {
        SpriteAnimation animation;
        animation.name = sprite.default_animation;
        animation.loop = true;
        for (const auto& frame_json : root["frames"]) {
            animation.frames.push_back(SpriteFrame{
                .texture = frame_json.value("texture", ""),
                .duration = frame_json.value("duration", 0.1f),
            });
        }
        sprite.animations.push_back(std::move(animation));
    }

    return sprite;
}

ObjectAsset LoadObjectAsset(const std::filesystem::path& path) {
    const json root = json::parse(core::FileIO::ReadText(path));
    ObjectAsset object;
    object.name = root.value("name", path.stem().string());
    object.preset = root.value("preset", "none");
    object.sprite = root.value("sprite", "");
    object.default_animation = root.value("default_animation", "idle");
    object.size = ParseVec2(root.value("size", json::array({32.0f, 32.0f})), {32.0f, 32.0f});
    object.tint = ParseVec4(root.value("tint", json::array({1.0f, 1.0f, 1.0f, 1.0f})));
    object.layer = root.value("layer", 0);
    object.dynamic = root.value("dynamic", false);
    object.collidable = root.value("collidable", false);
    object.reflection = root.contains("reflection") ? ParseReflectionValue(root["reflection"], 0.0f) : 0.0f;
    object.normal_map = root.value("normal_map", "");
    object.height_map = root.value("height_map", "");
    object.displacement_map = root.value("displacement_map", "");
    object.relief_enabled = root.value("relief_enabled", false);
    object.bump_strength = root.value("bump_strength", 1.0f);
    object.relief_depth = root.value("relief_depth", 0.035f);
    object.parallax_depth = root.value("parallax_depth", 0.018f);
    object.relief_contrast = root.value("relief_contrast", 1.35f);
    object.pseudo_3d = root.value("pseudo_3d", false);
    object.collider_offset = ParseVec2(root.value("collider_offset", json::array({0.0f, 0.0f})));
    object.collider_size = ParseVec2(root.value("collider_size", json::array({object.size.x, object.size.y})), object.size);
    object.pseudo_3d_height = root.value("pseudo_3d_height", 16.0f);
    object.sound = root.value("sound", "");
    object.script = root.value("script", "");
    object.properties = root.value("properties", json::object());
    return object;
}

TriggerAsset LoadTriggerAsset(const std::filesystem::path& path) {
    const json root = json::parse(core::FileIO::ReadText(path));
    TriggerAsset trigger;
    trigger.name = root.value("name", path.stem().string());
    trigger.default_size = ParseVec2(root.value("default_size", json::array({96.0f, 96.0f})), {96.0f, 96.0f});
    trigger.color = ParseVec4(root.value("color", json::array({1.0f, 0.7f, 0.1f, 1.0f})));
    trigger.once = root.value("once", false);
    trigger.enabled = root.value("enabled", true);
    for (const auto& condition_json : root.value("conditions", json::array())) {
        trigger.conditions.push_back(ParseCondition(condition_json));
    }
    for (const auto& action_json : root.value("actions", json::array())) {
        trigger.actions.push_back(ParseAction(action_json));
    }
    return trigger;
}

ObjectAnimationAsset LoadObjectAnimationAsset(const std::filesystem::path& path) {
    const json root = json::parse(core::FileIO::ReadText(path));
    ObjectAnimationAsset animation = ParseObjectAnimationAsset(root);
    if (animation.name.empty()) {
        animation.name = path.stem().string();
    }
    return animation;
}

ParticleEffectAsset LoadParticleEffectAsset(const std::filesystem::path& path) {
    const json root = json::parse(core::FileIO::ReadText(path));
    ParticleEffectAsset particle = ParseParticleEffectAsset(root);
    if (particle.name.empty()) {
        particle.name = path.stem().string();
    }
    return particle;
}

void SaveProject(const std::filesystem::path& path, const ProjectData& project) {
    json root;
    root["name"] = project.name;
    root["developer"] = project.developer;
    root["version"] = project.version;
    root["icon"] = project.icon;
    root["preview_image"] = project.preview_image;
    root["splash_enabled"] = project.splash_enabled;
    root["splash_image"] = project.splash_image;
    root["startup_level"] = project.startup_level;
    root["levels"] = project.levels;
    root["assets"] = project.assets;
    root["scripts"] = project.scripts;
    root["sprite_assets"] = project.sprite_assets;
    root["object_assets"] = project.object_assets;
    root["trigger_assets"] = project.trigger_assets;
    root["animation_assets"] = project.animation_assets;
    root["particle_assets"] = project.particle_assets;
    root["camera_mode"] = project.camera_mode;
    root["renderer_backend"] = project.renderer_backend;
    root["multithreading_enabled"] = project.multithreading_enabled;
    root["worker_threads"] = project.worker_threads;
    root["default_language"] = project.default_language;
    root["editor_language"] = project.editor_language;
    root["supported_languages"] = project.supported_languages;
    root["game_viewport_size"] = ToJson(project.game_viewport_size);
    root["editor_render_size"] = ToJson(project.editor_render_size);
    root["export_directory"] = project.export_directory;
    root["encrypt_archive"] = project.encrypt_archive;
    root["archive_password"] = project.archive_password;
    root["main_menu"] = ToJson(project.main_menu);
    root["pause_menu"] = ToJson(project.pause_menu);
    root["ui_elements"] = json::array();
    for (const auto& widget : project.ui_elements) {
        root["ui_elements"].push_back(ToJson(widget));
    }
    root["html_ui"] = ToJson(project.html_ui);
    core::FileIO::WriteText(path, root.dump(2));
}

std::string SaveLevelToText(const LevelData& level) {
    json root;
    root["name"] = level.name;
    root["tile_width"] = level.tile_width;
    root["tile_height"] = level.tile_height;
    root["player_spawn"] = ToJson(level.player_spawn);
    root["clear_color"] = ToJson(level.clear_color);
    root["post_effects"] = level.post_effects;
    root["lighting"] = ToJson(level.lighting);

    json tileset;
    tileset["texture"] = level.tileset.texture;
    tileset["tile_width"] = level.tileset.tile_width;
    tileset["tile_height"] = level.tileset.tile_height;
    tileset["columns"] = level.tileset.columns;
    tileset["rows"] = level.tileset.rows;
    tileset["collision"] = json::array();
    for (const auto& [id, info] : level.tileset.tile_info) {
        tileset["collision"].push_back({{"id", id}, {"type", ToString(info.collision)}});
    }
    root["tileset"] = std::move(tileset);

    root["tile_layers"] = json::array();
    for (const auto& layer : level.tile_layers) {
        root["tile_layers"].push_back({
            {"name", layer.name},
            {"width", layer.width},
            {"height", layer.height},
            {"depth", layer.depth},
            {"visible", layer.visible},
            {"collidable", layer.collidable},
            {"tiles", layer.tiles}
        });
    }

    root["parallax_layers"] = json::array();
    for (const auto& layer : level.parallax_layers) {
        root["parallax_layers"].push_back({
            {"id", layer.id},
            {"name", layer.name},
            {"texture", layer.texture},
            {"speed", ToJson(layer.speed)},
            {"scale", ToJson(layer.scale)},
            {"offset", ToJson(layer.offset)},
            {"tint", ToJson(layer.tint)},
            {"depth", layer.depth},
            {"zoom_factor", layer.zoom_factor},
            {"receives_lighting", layer.receives_lighting},
            {"lighting_response", layer.lighting_response},
            {"artificial_light", layer.artificial_light},
            {"artificial_light_color", ToJson(layer.artificial_light_color)},
            {"artificial_light_strength", layer.artificial_light_strength},
            {"repeat", layer.repeat},
            {"visible", layer.visible},
            {"editor_group", layer.editor_group}
        });
    }

    root["entities"] = json::array();
    for (const auto& entity : level.entities) {
        root["entities"].push_back({
            {"id", entity.id},
            {"archetype", entity.archetype},
            {"object_asset", entity.object_asset},
            {"sprite_asset", entity.sprite_asset},
            {"animation", entity.animation},
            {"position", ToJson(entity.position)},
            {"size", ToJson(entity.size)},
            {"velocity", ToJson(entity.velocity)},
            {"tint", ToJson(entity.tint)},
            {"texture", entity.texture},
            {"sound", entity.sound},
            {"uv", ToJson(entity.uv)},
            {"layer", entity.layer},
            {"dynamic", entity.dynamic},
            {"collidable", entity.collidable},
            {"visible", entity.visible},
            {"reflection", entity.reflection},
            {"normal_map", entity.normal_map},
            {"height_map", entity.height_map},
            {"displacement_map", entity.displacement_map},
            {"relief_enabled", entity.relief_enabled},
            {"bump_strength", entity.bump_strength},
            {"relief_depth", entity.relief_depth},
            {"parallax_depth", entity.parallax_depth},
            {"relief_contrast", entity.relief_contrast},
            {"pseudo_3d", entity.pseudo_3d},
            {"collider_offset", ToJson(entity.collider_offset)},
            {"collider_size", ToJson(entity.collider_size)},
            {"pseudo_3d_height", entity.pseudo_3d_height},
            {"animation_speed", entity.animation_speed},
            {"script", entity.script},
            {"on_trigger", entity.on_trigger},
            {"attached_to", entity.attached_to},
            {"attach_offset", ToJson(entity.attach_offset)},
            {"properties", entity.properties},
            {"editor_group", entity.editor_group}
        });
    }

    root["triggers"] = json::array();
    for (const auto& trigger : level.triggers) {
        json trigger_json{
            {"id", trigger.id},
            {"name", trigger.name},
            {"asset", trigger.asset},
            {"position", ToJson(trigger.position)},
            {"size", ToJson(trigger.size)},
            {"color", ToJson(trigger.color)},
            {"once", trigger.once},
            {"enabled", trigger.enabled},
            {"editor_group", trigger.editor_group},
            {"conditions", json::array()},
            {"actions", json::array()}
        };
        for (const auto& condition : trigger.conditions) {
            trigger_json["conditions"].push_back(ToJson(condition));
        }
        for (const auto& action : trigger.actions) {
            trigger_json["actions"].push_back(ToJson(action));
        }
        root["triggers"].push_back(std::move(trigger_json));
    }

    root["lights"] = json::array();
    for (const auto& light : level.lights) {
        root["lights"].push_back(ToJson(light));
    }

    root["audio_sources"] = json::array();
    for (const auto& source : level.audio_sources) {
        root["audio_sources"].push_back(ToJson(source));
    }

    root["audio_paks"] = json::array();
    for (const auto& pak : level.audio_paks) {
        root["audio_paks"].push_back(ToJson(pak));
    }

    root["player_camera"] = ToJson(level.player_camera);

    root["virtual_cameras"] = json::array();
    for (const auto& camera : level.virtual_cameras) {
        root["virtual_cameras"].push_back(ToJson(camera));
    }

    root["animations"] = json::array();
    for (const auto& animation : level.animations) {
        root["animations"].push_back(ToJson(animation));
    }

    return root.dump(2);
}

void SaveLevel(const std::filesystem::path& path, const LevelData& level) {
    core::FileIO::WriteText(path, SaveLevelToText(level));
}

void SaveSpriteAsset(const std::filesystem::path& path, const SpriteAsset& sprite) {
    json root;
    root["name"] = sprite.name;
    root["canvas_size"] = ToJson(sprite.canvas_size);
    root["default_animation"] = sprite.default_animation;
    root["pivot"] = ToJson(sprite.pivot);
    root["tint"] = ToJson(sprite.tint);
    root["animations"] = json::array();
    for (const auto& animation : sprite.animations) {
        json animation_json{
            {"name", animation.name},
            {"loop", animation.loop},
            {"frames", json::array()}
        };
        for (const auto& frame : animation.frames) {
            animation_json["frames"].push_back({
                {"texture", frame.texture},
                {"duration", frame.duration}
            });
        }
        root["animations"].push_back(std::move(animation_json));
    }
    core::FileIO::WriteText(path, root.dump(2));
}

void SaveObjectAsset(const std::filesystem::path& path, const ObjectAsset& object) {
    json root;
    root["name"] = object.name;
    root["preset"] = object.preset;
    root["sprite"] = object.sprite;
    root["default_animation"] = object.default_animation;
    root["size"] = ToJson(object.size);
    root["tint"] = ToJson(object.tint);
    root["layer"] = object.layer;
    root["dynamic"] = object.dynamic;
    root["collidable"] = object.collidable;
    root["reflection"] = object.reflection;
    root["normal_map"] = object.normal_map;
    root["height_map"] = object.height_map;
    root["displacement_map"] = object.displacement_map;
    root["relief_enabled"] = object.relief_enabled;
    root["bump_strength"] = object.bump_strength;
    root["relief_depth"] = object.relief_depth;
    root["parallax_depth"] = object.parallax_depth;
    root["relief_contrast"] = object.relief_contrast;
    root["pseudo_3d"] = object.pseudo_3d;
    root["collider_offset"] = ToJson(object.collider_offset);
    root["collider_size"] = ToJson(object.collider_size);
    root["pseudo_3d_height"] = object.pseudo_3d_height;
    root["sound"] = object.sound;
    root["script"] = object.script;
    root["properties"] = object.properties;
    core::FileIO::WriteText(path, root.dump(2));
}

void SaveTriggerAsset(const std::filesystem::path& path, const TriggerAsset& trigger) {
    json root;
    root["name"] = trigger.name;
    root["default_size"] = ToJson(trigger.default_size);
    root["color"] = ToJson(trigger.color);
    root["once"] = trigger.once;
    root["enabled"] = trigger.enabled;
    root["conditions"] = json::array();
    root["actions"] = json::array();
    for (const auto& condition : trigger.conditions) {
        root["conditions"].push_back(ToJson(condition));
    }
    for (const auto& action : trigger.actions) {
        root["actions"].push_back(ToJson(action));
    }
    core::FileIO::WriteText(path, root.dump(2));
}

void SaveObjectAnimationAsset(const std::filesystem::path& path, const ObjectAnimationAsset& animation) {
    core::FileIO::WriteText(path, ToJson(animation).dump(2));
}

void SaveParticleEffectAsset(const std::filesystem::path& path, const ParticleEffectAsset& particle) {
    core::FileIO::WriteText(path, ToJson(particle).dump(2));
}

}  // namespace novaiso::assets
