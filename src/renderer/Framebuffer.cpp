#include "renderer/Framebuffer.h"

#include <glm/common.hpp>
#include <stdexcept>

namespace novaiso::renderer {

Framebuffer::~Framebuffer() {
    Destroy();
}

void Framebuffer::Resize(glm::ivec2 size) {
    const glm::ivec2 new_size{glm::max(size.x, 1), glm::max(size.y, 1)};
    if (fbo_ != 0 && new_size == size_) {
        return;
    }

    if (fbo_ == 0) {
        glCreateFramebuffers(1, &fbo_);
    } else {
        if (depth_rbo_ != 0) {
            glDeleteRenderbuffers(1, &depth_rbo_);
            depth_rbo_ = 0;
        }
        if (color_texture_ != 0) {
            glDeleteTextures(1, &color_texture_);
            color_texture_ = 0;
        }
    }
    size_ = new_size;

    glCreateTextures(GL_TEXTURE_2D, 1, &color_texture_);
    glCreateRenderbuffers(1, &depth_rbo_);

    glTextureParameteri(color_texture_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(color_texture_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(color_texture_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(color_texture_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureStorage2D(color_texture_, 1, GL_RGBA8, size_.x, size_.y);

    glNamedRenderbufferStorage(depth_rbo_, GL_DEPTH24_STENCIL8, size_.x, size_.y);
    glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0, color_texture_, 0);
    glNamedFramebufferRenderbuffer(fbo_, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rbo_);

    if (glCheckNamedFramebufferStatus(fbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Framebuffer incomplete.");
    }
}

void Framebuffer::Bind() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, size_.x, size_.y);
}

void Framebuffer::BindDefault(glm::ivec2 size) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, size.x, size.y);
}

void Framebuffer::Destroy() {
    if (depth_rbo_ != 0) {
        glDeleteRenderbuffers(1, &depth_rbo_);
        depth_rbo_ = 0;
    }
    if (color_texture_ != 0) {
        glDeleteTextures(1, &color_texture_);
        color_texture_ = 0;
    }
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    size_ = {1, 1};
}

GLuint Framebuffer::Texture() const {
    return color_texture_;
}

glm::ivec2 Framebuffer::Size() const {
    return size_;
}

}  // namespace novaiso::renderer
