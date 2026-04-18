#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace novaiso::entities {

struct Entity {
    std::string id;
    std::string archetype;
    std::string object_asset;
    std::string sprite_asset;
    std::string animation = "idle";
    glm::vec2 position{0.0f, 0.0f};
    glm::vec2 previous_position{0.0f, 0.0f};
    glm::vec2 size{32.0f, 32.0f};
    glm::vec2 velocity{0.0f, 0.0f};
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    std::string texture;
    std::string sound;
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
    float rotation = 0.0f;
    glm::vec2 skew{0.0f, 0.0f};
    int layer = 0;
    bool dynamic = false;
    bool collidable = false;
    bool active = true;
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
    bool grounded = false;
    bool was_grounded = false;
    bool facing_right = true;
    glm::vec2 collider_offset{0.0f, 0.0f};
    glm::vec2 collider_size{32.0f, 32.0f};
    float pseudo_3d_height = 16.0f;
    float animation_speed = 1.0f;
    float animation_time = 0.0f;
    int animation_frame = 0;
    std::string script;
    std::string on_trigger;
    std::string attached_to;
    glm::vec2 attach_offset{0.0f, 0.0f};
    nlohmann::json properties = nlohmann::json::object();
};

}  // namespace novaiso::entities
