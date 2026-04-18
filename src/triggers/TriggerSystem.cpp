#include "triggers/TriggerSystem.h"

#include "entities/Scene.h"
#include "scripting/PythonScripting.h"

namespace novaiso::triggers {

namespace {

bool Intersects(glm::vec2 a_pos, glm::vec2 a_size, glm::vec2 b_pos, glm::vec2 b_size) {
    return a_pos.x < b_pos.x + b_size.x &&
           a_pos.x + a_size.x > b_pos.x &&
           a_pos.y < b_pos.y + b_size.y &&
           a_pos.y + a_size.y > b_pos.y;
}

}  // namespace

void TriggerSystem::Reset() {
    occupied_.clear();
}

void TriggerSystem::Update(entities::Scene& scene, std::vector<assets::TriggerZone>& triggers) {
    entities::Entity* player = scene.Player();
    if (player == nullptr) {
        return;
    }

    for (auto& trigger : triggers) {
        if (!trigger.enabled) {
            occupied_[trigger.name] = false;
            continue;
        }

        const bool inside = Intersects(player->position, player->size, trigger.position, trigger.size);
        const bool was_inside = occupied_[trigger.name];
        if (inside && !was_inside && !(trigger.once && trigger.fired)) {
            if (scene.DebugTraceEnabled()) {
                scene.Trace("Player entered trigger " + trigger.name + "");
            }
            bool conditions_ok = true;
            for (const auto& condition : trigger.conditions) {
                conditions_ok = conditions_ok && scene.Scripts().EvaluateCondition(condition, scene, trigger.name);
            }

            if (conditions_ok) {
                for (const auto& action : trigger.actions) {
                    if (scene.DebugTraceEnabled()) {
                        scene.Trace("Trigger " + trigger.name + " action " + action.function + "");
                    }
                    scene.Scripts().ExecuteAction(action, scene, trigger.name);
                }
                scene.MarkTriggerFired(trigger.name);

                for (auto& entity : scene.RuntimeEntities()) {
                    if (entity.active &&
                        Intersects(entity.position, entity.size, trigger.position, trigger.size) &&
                        !entity.script.empty()) {
                        if (scene.DebugTraceEnabled()) {
                            scene.Trace("Entity trigger hook " + entity.id + " from " + trigger.name + "");
                        }
                        scene.Scripts().CallEntityTrigger(entity, scene, trigger.name);
                    }
                }

                if (trigger.once) {
                    trigger.fired = true;
                }
            } else if (scene.DebugTraceEnabled()) {
                scene.Trace("Trigger " + trigger.name + " blocked by conditions");
            }
        }

        occupied_[trigger.name] = inside;
    }
}

}  // namespace novaiso::triggers
