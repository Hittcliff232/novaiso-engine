#pragma once

#include "assets/Project.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace novaiso::entities {
class Scene;
}

namespace novaiso::triggers {

class TriggerSystem {
public:
    void Reset();
    void Update(entities::Scene& scene, std::vector<assets::TriggerZone>& triggers);

private:
    std::unordered_map<std::string, bool> occupied_;
};

}  // namespace novaiso::triggers
