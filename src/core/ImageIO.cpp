#include "core/ImageIO.h"

#include "core/FileIO.h"

#include <stb_image.h>

#include <algorithm>

namespace novaiso::core {

namespace {

bool HasGifSignature(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || size < 6) {
        return false;
    }
    return (bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == '8' &&
            ((bytes[4] == '7' && bytes[5] == 'a') || (bytes[4] == '9' && bytes[5] == 'a')));
}

}  // namespace

bool DecodeImageMemory(const std::uint8_t* bytes, std::size_t size, DecodedImage& out_image) {
    out_image = {};
    if (bytes == nullptr || size == 0) {
        return false;
    }

    if (HasGifSignature(bytes, size)) {
        int* delays = nullptr;
        int width = 0;
        int height = 0;
        int frame_count = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_gif_from_memory(bytes, static_cast<int>(size), &delays, &width, &height, &frame_count, &channels, STBI_rgb_alpha);
        if (pixels == nullptr || width <= 0 || height <= 0 || frame_count <= 0) {
            if (pixels != nullptr) {
                stbi_image_free(pixels);
            }
            if (delays != nullptr) {
                stbi_image_free(delays);
            }
            return false;
        }

        const std::size_t frame_stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4ull;
        out_image.width = width;
        out_image.height = height;
        out_image.channels = 4;
        out_image.animated = frame_count > 1;
        out_image.frames.reserve(static_cast<std::size_t>(frame_count));
        for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
            DecodedImageFrame frame;
            frame.duration_ms = delays != nullptr && delays[frame_index] > 0 ? delays[frame_index] : 100;
            const std::uint8_t* frame_begin = pixels + frame_stride * static_cast<std::size_t>(frame_index);
            frame.rgba.assign(frame_begin, frame_begin + frame_stride);
            out_image.frames.push_back(std::move(frame));
        }

        stbi_image_free(pixels);
        if (delays != nullptr) {
            stbi_image_free(delays);
        }
        return !out_image.frames.empty();
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return false;
    }

    DecodedImageFrame frame;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4ull;
    frame.rgba.assign(pixels, pixels + pixel_count);
    stbi_image_free(pixels);

    out_image.width = width;
    out_image.height = height;
    out_image.channels = 4;
    out_image.animated = false;
    out_image.frames.push_back(std::move(frame));
    return true;
}

bool DecodeImageFile(const std::filesystem::path& path, DecodedImage& out_image) {
    try {
        const auto bytes = FileIO::ReadBinary(path);
        if (bytes.empty()) {
            out_image = {};
            return false;
        }
        return DecodeImageMemory(bytes.data(), bytes.size(), out_image);
    } catch (...) {
        out_image = {};
        return false;
    }
}

}  // namespace novaiso::core
