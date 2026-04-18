#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace novaiso::core {

struct DecodedImageFrame {
    std::vector<std::uint8_t> rgba;
    int duration_ms = 100;
};

struct DecodedImage {
    int width = 0;
    int height = 0;
    int channels = 4;
    bool animated = false;
    std::vector<DecodedImageFrame> frames;
};

bool DecodeImageMemory(const std::uint8_t* bytes, std::size_t size, DecodedImage& out_image);
bool DecodeImageFile(const std::filesystem::path& path, DecodedImage& out_image);

}  // namespace novaiso::core
