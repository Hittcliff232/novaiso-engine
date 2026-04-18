#pragma once

#include <SDL.h>

#include <array>
#include <cstdint>
#include <glm/vec2.hpp>
#include <string>

namespace novaiso::core {

class Input {
public:
    void BeginFrame();
    void ProcessEvent(const SDL_Event& event);

    [[nodiscard]] bool IsKeyDown(SDL_Scancode code) const;
    [[nodiscard]] bool WasKeyPressed(SDL_Scancode code) const;
    [[nodiscard]] bool WasKeyReleased(SDL_Scancode code) const;
    [[nodiscard]] bool IsMouseDown(std::uint8_t button) const;
    [[nodiscard]] bool WasMousePressed(std::uint8_t button) const;
    [[nodiscard]] glm::vec2 MousePosition() const;
    [[nodiscard]] glm::vec2 MouseDelta() const;
    [[nodiscard]] float WheelDelta() const;
    [[nodiscard]] bool ActionDown(const std::string& action) const;

private:
    std::array<bool, SDL_NUM_SCANCODES> current_keys_{};
    std::array<bool, SDL_NUM_SCANCODES> previous_keys_{};
    std::array<bool, 8> current_mouse_{};
    std::array<bool, 8> previous_mouse_{};
    glm::vec2 mouse_position_{0.0f, 0.0f};
    glm::vec2 previous_mouse_position_{0.0f, 0.0f};
    float wheel_delta_{0.0f};
};

}  // namespace novaiso::core
