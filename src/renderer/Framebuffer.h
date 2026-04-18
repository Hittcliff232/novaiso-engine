#pragma once

#include <glad/gl.h>
#include <glm/vec2.hpp>

namespace novaiso::renderer {

class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    void Resize(glm::ivec2 size);
    void Bind();
    static void BindDefault(glm::ivec2 size);
    void Destroy();

    [[nodiscard]] GLuint Texture() const;
    [[nodiscard]] glm::ivec2 Size() const;

private:
    GLuint fbo_ = 0;
    GLuint color_texture_ = 0;
    GLuint depth_rbo_ = 0;
    glm::ivec2 size_{1, 1};
};

}  // namespace novaiso::renderer
