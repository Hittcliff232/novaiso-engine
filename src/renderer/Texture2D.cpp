#include "renderer/Texture2D.h"

#include "core/ImageIO.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace novaiso::renderer {

Texture2D::~Texture2D() {
    Destroy();
}

Texture2D::Texture2D(Texture2D&& other) noexcept {
    *this = std::move(other);
}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
    if (this != &other) {
        Destroy();
        id_ = other.id_;
        size_ = other.size_;
        source_path_ = std::move(other.source_path_);
        other.id_ = 0;
        other.size_ = {1, 1};
    }
    return *this;
}

bool Texture2D::LoadFromFile(const std::filesystem::path& path) {
    core::DecodedImage image;
    if (!core::DecodeImageFile(path, image) || image.frames.empty() || image.frames.front().rgba.empty()) {
        return false;
    }

    CreateFromRgba(image.width, image.height, image.frames.front().rgba.data(), false, true);
    source_path_ = path;
    return true;
}

bool Texture2D::LoadFromMemory(const std::uint8_t* bytes, std::size_t size, const std::filesystem::path& virtual_path) {
    core::DecodedImage image;
    if (!core::DecodeImageMemory(bytes, size, image) || image.frames.empty() || image.frames.front().rgba.empty()) {
        return false;
    }

    CreateFromRgba(image.width, image.height, image.frames.front().rgba.data(), false, true);
    source_path_ = virtual_path;
    return true;
}

void Texture2D::CreateFromRgba(std::int32_t width, std::int32_t height, const void* pixels, bool linear_filter, bool repeat) {
    Destroy();
    glCreateTextures(GL_TEXTURE_2D, 1, &id_);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTextureStorage2D(id_, 1, GL_RGBA8, width, height);
    glTextureSubImage2D(id_, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    size_ = {width, height};
    source_path_.clear();
}

void Texture2D::CreateWhitePixel() {
    static constexpr std::uint8_t white[] = {255, 255, 255, 255};
    CreateFromRgba(1, 1, white);
}

void Texture2D::Bind(std::uint32_t slot) const {
    glBindTextureUnit(slot, id_);
}

void Texture2D::Destroy() {
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
}

GLuint Texture2D::Id() const {
    return id_;
}

glm::ivec2 Texture2D::Size() const {
    return size_;
}

const std::filesystem::path& Texture2D::SourcePath() const {
    return source_path_;
}

}  // namespace novaiso::renderer
