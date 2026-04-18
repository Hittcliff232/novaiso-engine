#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <optional>
#include <string>
#include <vector>

namespace novaiso::core {

class FileIO {
public:
    struct PackEntry {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    static std::string ReadText(const std::filesystem::path& path);
    static void WriteText(const std::filesystem::path& path, std::string_view content);
    static std::vector<std::uint8_t> ReadBinary(const std::filesystem::path& path);
    static bool Exists(const std::filesystem::path& path);
    static void EnsureDirectory(const std::filesystem::path& path);
    static void CopyFileInto(const std::filesystem::path& source, const std::filesystem::path& destination_directory);
    static std::optional<std::filesystem::file_time_type> LastWriteTime(const std::filesystem::path& path);

    static bool CreatePack(
        const std::filesystem::path& pack_path,
        const std::filesystem::path& source_root,
        const std::vector<std::filesystem::path>& relative_paths,
        std::string_view password = {});
    static bool MountPack(
        const std::filesystem::path& pack_path,
        const std::filesystem::path& virtual_root = {},
        std::string_view password = {});
    static void UnmountPack();
    [[nodiscard]] static bool PackMounted();
    [[nodiscard]] static std::vector<std::filesystem::path> MountedPackFiles();
};

}  // namespace novaiso::core
