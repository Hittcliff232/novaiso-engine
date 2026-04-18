#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace novaiso::core {

std::vector<std::uint8_t> LoadEmbeddedBinaryResource(const std::wstring& type, const std::wstring& name);
bool EmbedBinaryResource(
    const std::filesystem::path& executable_path,
    const std::wstring& type,
    const std::wstring& name,
    const std::vector<std::uint8_t>& bytes);
bool EmbedPngIconResource(const std::filesystem::path& executable_path, const std::vector<std::uint8_t>& png_bytes);

}  // namespace novaiso::core
