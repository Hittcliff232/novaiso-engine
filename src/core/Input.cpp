#include "core/Input.h"

#include <unordered_map>

namespace novaiso::core {

void Input::BeginFrame() {
    previous_keys_ = current_keys_;
    previous_mouse_ = current_mouse_;
    previous_mouse_position_ = mouse_position_;
    wheel_delta_ = 0.0f;
}

void Input::ProcessEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_KEYDOWN:
            if (!event.key.repeat) {
                current_keys_[event.key.keysym.scancode] = true;
            }
            break;
        case SDL_KEYUP:
            current_keys_[event.key.keysym.scancode] = false;
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button < current_mouse_.size()) {
                current_mouse_[event.button.button] = true;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (event.button.button < current_mouse_.size()) {
                current_mouse_[event.button.button] = false;
            }
            break;
        case SDL_MOUSEMOTION:
            mouse_position_ = {static_cast<float>(event.motion.x), static_cast<float>(event.motion.y)};
            break;
        case SDL_MOUSEWHEEL:
            wheel_delta_ += static_cast<float>(event.wheel.preciseY);
            break;
        default:
            break;
    }
}

bool Input::IsKeyDown(SDL_Scancode code) const {
    return current_keys_[code];
}

bool Input::WasKeyPressed(SDL_Scancode code) const {
    return current_keys_[code] && !previous_keys_[code];
}

bool Input::WasKeyReleased(SDL_Scancode code) const {
    return !current_keys_[code] && previous_keys_[code];
}

bool Input::IsMouseDown(std::uint8_t button) const {
    return button < current_mouse_.size() ? current_mouse_[button] : false;
}

bool Input::WasMousePressed(std::uint8_t button) const {
    return button < current_mouse_.size() ? current_mouse_[button] && !previous_mouse_[button] : false;
}

glm::vec2 Input::MousePosition() const {
    return mouse_position_;
}

glm::vec2 Input::MouseDelta() const {
    return mouse_position_ - previous_mouse_position_;
}

float Input::WheelDelta() const {
    return wheel_delta_;
}

bool Input::ActionDown(const std::string& action) const {
    static const std::unordered_map<std::string, SDL_Scancode> bindings = {
        {"MoveLeft", SDL_SCANCODE_A},
        {"MoveRight", SDL_SCANCODE_D},
        {"MoveUp", SDL_SCANCODE_W},
        {"MoveDown", SDL_SCANCODE_S},
        {"Jump", SDL_SCANCODE_SPACE},
        {"Run", SDL_SCANCODE_LSHIFT},
        {"ToggleCamera", SDL_SCANCODE_TAB},
        {"Interact", SDL_SCANCODE_E},
    };
    const auto it = bindings.find(action);
    return it != bindings.end() && IsKeyDown(it->second);
}

}  // namespace novaiso::core
