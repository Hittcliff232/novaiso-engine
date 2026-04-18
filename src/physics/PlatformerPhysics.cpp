#include "physics/PlatformerPhysics.h"

#include <algorithm>
#include <cmath>

namespace novaiso::physics {

namespace {

constexpr float kGravity = 1800.0f;
constexpr float kMaxFallSpeed = 1400.0f;

int FloorToTile(float value, int tile_size) {
    return static_cast<int>(std::floor(value / static_cast<float>(tile_size)));
}

}  // namespace

void PlatformerPhysics::Step(entities::Entity& entity, const assets::LevelData& level, float delta_time) const {
    if (!entity.dynamic || !entity.active) {
        return;
    }

    const glm::vec2 previous_position = entity.position;
    entity.grounded = false;
    entity.velocity.y = std::min(entity.velocity.y + kGravity * delta_time, kMaxFallSpeed);

    entity.position.x += entity.velocity.x * delta_time;
    ResolveHorizontal(entity, level, previous_position);

    entity.position.y += entity.velocity.y * delta_time;
    ResolveVertical(entity, level, previous_position);
    ResolveSlope(entity, level, previous_position);
}

assets::CollisionType PlatformerPhysics::CollisionAt(const assets::LevelData& level, int tile_x, int tile_y) const {
    if (tile_x < 0 || tile_y < 0) {
        return assets::CollisionType::Empty;
    }

    for (const auto& layer : level.tile_layers) {
        if (!layer.collidable) {
            continue;
        }
        if (tile_x >= layer.width || tile_y >= layer.height) {
            continue;
        }
        const int index = tile_y * layer.width + tile_x;
        if (index < 0 || index >= static_cast<int>(layer.tiles.size())) {
            continue;
        }
        const int tile_id = layer.tiles[index];
        if (tile_id == 0) {
            continue;
        }
        if (const auto it = level.tileset.tile_info.find(tile_id); it != level.tileset.tile_info.end()) {
            return it->second.collision;
        }
        return assets::CollisionType::Solid;
    }
    return assets::CollisionType::Empty;
}

float PlatformerPhysics::SlopeSurfaceY(const assets::LevelData& level, int tile_x, int tile_y, assets::CollisionType type, float world_x) const {
    const float tile_origin_x = static_cast<float>(tile_x * level.tile_width);
    const float tile_origin_y = static_cast<float>(tile_y * level.tile_height);
    const float local_x = std::clamp(world_x - tile_origin_x, 0.0f, static_cast<float>(level.tile_width));
    const float ratio = local_x / static_cast<float>(level.tile_width);
    if (type == assets::CollisionType::SlopeLeft) {
        return tile_origin_y + (1.0f - ratio) * static_cast<float>(level.tile_height);
    }
    return tile_origin_y + ratio * static_cast<float>(level.tile_height);
}

void PlatformerPhysics::ResolveHorizontal(entities::Entity& entity, const assets::LevelData& level, glm::vec2) const {
    const int left = FloorToTile(entity.position.x, level.tile_width);
    const int right = FloorToTile(entity.position.x + entity.size.x - 1.0f, level.tile_width);
    const int top = FloorToTile(entity.position.y, level.tile_height);
    const int bottom = FloorToTile(entity.position.y + entity.size.y - 1.0f, level.tile_height);

    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const assets::CollisionType type = CollisionAt(level, x, y);
            if (type != assets::CollisionType::Solid) {
                continue;
            }

            const float tile_left = static_cast<float>(x * level.tile_width);
            const float tile_right = tile_left + static_cast<float>(level.tile_width);
            if (entity.velocity.x > 0.0f) {
                entity.position.x = tile_left - entity.size.x;
            } else if (entity.velocity.x < 0.0f) {
                entity.position.x = tile_right;
            }
            entity.velocity.x = 0.0f;
        }
    }
}

void PlatformerPhysics::ResolveVertical(entities::Entity& entity, const assets::LevelData& level, glm::vec2 previous_position) const {
    const int left = FloorToTile(entity.position.x + 2.0f, level.tile_width);
    const int right = FloorToTile(entity.position.x + entity.size.x - 2.0f, level.tile_width);
    const int top = FloorToTile(entity.position.y, level.tile_height);
    const int bottom = FloorToTile(entity.position.y + entity.size.y - 1.0f, level.tile_height);

    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const assets::CollisionType type = CollisionAt(level, x, y);
            if (type == assets::CollisionType::Empty || type == assets::CollisionType::SlopeLeft || type == assets::CollisionType::SlopeRight) {
                continue;
            }

            const float tile_top = static_cast<float>(y * level.tile_height);
            const float tile_bottom = tile_top + static_cast<float>(level.tile_height);
            if (type == assets::CollisionType::OneWay) {
                const float previous_bottom = previous_position.y + entity.size.y;
                if (entity.velocity.y < 0.0f || previous_bottom > tile_top + 6.0f) {
                    continue;
                }
            }

            if (entity.velocity.y > 0.0f) {
                entity.position.y = tile_top - entity.size.y;
                entity.grounded = true;
            } else if (entity.velocity.y < 0.0f) {
                entity.position.y = tile_bottom;
            }
            entity.velocity.y = 0.0f;
        }
    }
}

void PlatformerPhysics::ResolveSlope(entities::Entity& entity, const assets::LevelData& level, glm::vec2 previous_position) const {
    const float foot_x = entity.position.x + entity.size.x * 0.5f;
    const int tile_x = FloorToTile(foot_x, level.tile_width);
    const int tile_y = FloorToTile(entity.position.y + entity.size.y, level.tile_height);
    const assets::CollisionType type = CollisionAt(level, tile_x, tile_y);
    if (type != assets::CollisionType::SlopeLeft && type != assets::CollisionType::SlopeRight) {
        return;
    }

    const float surface_y = SlopeSurfaceY(level, tile_x, tile_y, type, foot_x);
    const float previous_bottom = previous_position.y + entity.size.y;
    const float current_bottom = entity.position.y + entity.size.y;
    if (current_bottom >= surface_y && previous_bottom <= surface_y + 18.0f) {
        entity.position.y = surface_y - entity.size.y;
        entity.velocity.y = 0.0f;
        entity.grounded = true;
    }
}

}  // namespace novaiso::physics
