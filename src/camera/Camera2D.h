#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace novaiso::camera {

enum class Mode {
    Side,
    Isometric
};

class Camera2D {
public:
    void SetViewport(glm::vec2 viewport);
    void SetPosition(glm::vec2 position);
    void SetZoom(float zoom);
    void SetMode(Mode mode);

    [[nodiscard]] glm::vec2 Viewport() const;
    [[nodiscard]] glm::vec2 Position() const;
    [[nodiscard]] float Zoom() const;
    [[nodiscard]] Mode GetMode() const;
    [[nodiscard]] glm::mat4 ViewProjection() const;
    [[nodiscard]] glm::vec2 ScreenToWorld(glm::vec2 screen) const;
    [[nodiscard]] glm::vec2 WorldToScreen(glm::vec2 world) const;

private:
    glm::vec2 viewport_{1280.0f, 720.0f};
    glm::vec2 position_{0.0f, 0.0f};
    float zoom_ = 1.0f;
    Mode mode_ = Mode::Side;
};

}  // namespace novaiso::camera
