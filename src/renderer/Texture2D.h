#pragma once

#include <glad/gl.h>
#include <glm/vec2.hpp>

#include <cstdint>
#include <filesystem>

namespace novaiso::renderer {

class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D();

    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;
    Texture2D(Texture2D&& other) noexcept;
    Texture2D& operator=(Texture2D&& other) noexcept;

    bool LoadFromFile(const std::filesystem::path& path);
    bool LoadFromMemory(const std::uint8_t* bytes, std::size_t size, const std::filesystem::path& virtual_path = {});
    void CreateFromRgba(std::int32_t width, std::int32_t height, const void* pixels, bool linear_filter = false, bool repeat = false);
    void CreateWhitePixel();
    void Bind(std::uint32_t slot = 0) const;
    void Destroy();

    [[nodiscard]] GLuint Id() const;
    [[nodiscard]] glm::ivec2 Size() const;
    [[nodiscard]] const std::filesystem::path& SourcePath() const;

private:
    GLuint id_ = 0;
    glm::ivec2 size_{1, 1};
    std::filesystem::path source_path_;
};

}  // namespace novaiso::renderer
