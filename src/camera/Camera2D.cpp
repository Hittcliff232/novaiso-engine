#include "camera/Camera2D.h"

#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace novaiso::camera {

void Camera2D::SetViewport(glm::vec2 viewport) {
    viewport_ = viewport;
}

void Camera2D::SetPosition(glm::vec2 position) {
    position_ = position;
}

void Camera2D::SetZoom(float zoom) {
    zoom_ = glm::max(zoom, 0.05f);
}

void Camera2D::SetMode(Mode mode) {
    mode_ = mode;
}

glm::vec2 Camera2D::Viewport() const {
    return viewport_;
}

glm::vec2 Camera2D::Position() const {
    return position_;
}

float Camera2D::Zoom() const {
    return zoom_;
}

Mode Camera2D::GetMode() const {
    return mode_;
}

glm::mat4 Camera2D::ViewProjection() const {
    const float half_width = (viewport_.x * 0.5f) / zoom_;
    const float half_height = (viewport_.y * 0.5f) / zoom_;
    glm::mat4 projection = glm::ortho(-half_width, half_width, half_height, -half_height, -2000.0f, 2000.0f);
    glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(-position_, 0.0f));
    if (mode_ == Mode::Isometric) {
        const glm::mat4 iso =
            glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.5f, 1.0f));
        view = iso * view;
    }
    return projection * view;
}

glm::vec2 Camera2D::ScreenToWorld(glm::vec2 screen) const {
    const glm::vec2 ndc{
        (screen.x / viewport_.x) * 2.0f - 1.0f,
        1.0f - (screen.y / viewport_.y) * 2.0f
    };
    const glm::mat4 inverse = glm::inverse(ViewProjection());
    const glm::vec4 world = inverse * glm::vec4(ndc, 0.0f, 1.0f);
    return {world.x, world.y};
}

glm::vec2 Camera2D::WorldToScreen(glm::vec2 world) const {
    const glm::vec4 clip = ViewProjection() * glm::vec4(world, 0.0f, 1.0f);
    const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
    return {
        (ndc.x * 0.5f + 0.5f) * viewport_.x,
        (0.5f - ndc.y * 0.5f) * viewport_.y
    };
}

}  // namespace novaiso::camera
