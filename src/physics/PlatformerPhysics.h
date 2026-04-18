#pragma once

#include "assets/Project.h"
#include "entities/Entity.h"

#include <glm/vec2.hpp>

namespace novaiso::physics {

class PlatformerPhysics {
public:
    void Step(entities::Entity& entity, const assets::LevelData& level, float delta_time) const;

private:
    [[nodiscard]] assets::CollisionType CollisionAt(const assets::LevelData& level, int tile_x, int tile_y) const;
    [[nodiscard]] float SlopeSurfaceY(const assets::LevelData& level, int tile_x, int tile_y, assets::CollisionType type, float world_x) const;
    void ResolveHorizontal(entities::Entity& entity, const assets::LevelData& level, glm::vec2 previous_position) const;
    void ResolveVertical(entities::Entity& entity, const assets::LevelData& level, glm::vec2 previous_position) const;
    void ResolveSlope(entities::Entity& entity, const assets::LevelData& level, glm::vec2 previous_position) const;
};

}  // namespace novaiso::physics
