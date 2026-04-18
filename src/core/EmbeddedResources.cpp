#include "core/EmbeddedResources.h"

#include <stb_image.h>

#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

#include <vector>

namespace novaiso::core {

namespace {

constexpr WORD kResourceTypeIcon = 3;
constexpr WORD kResourceTypeGroupIcon = 14;

#pragma pack(push, 2)
struct GrpIconDir {
    WORD reserved = 0;
    WORD type = 1;
    WORD count = 1;
};

struct GrpIconEntry {
    BYTE width = 0;
    BYTE height = 0;
    BYTE color_count = 0;
    BYTE reserved = 0;
    WORD planes = 1;
    WORD bit_count = 32;
    DWORD bytes_in_res = 0;
    WORD resource_id = 1;
};
#pragma pack(pop)

}  // namespace

std::vector<std::uint8_t> LoadEmbeddedBinaryResource(const std::wstring& type, const std::wstring& name) {
    HRSRC resource = FindResourceW(nullptr, name.c_str(), type.c_str());
    if (resource == nullptr) {
        return {};
    }
    HGLOBAL handle = LoadResource(nullptr, resource);
    if (handle == nullptr) {
        return {};
    }
    const DWORD size = SizeofResource(nullptr, resource);
    const void* bytes = LockResource(handle);
    if (bytes == nullptr || size == 0) {
        return {};
    }
    const auto* first = static_cast<const std::uint8_t*>(bytes);
    return std::vector<std::uint8_t>(first, first + size);
}

bool EmbedBinaryResource(
    const std::filesystem::path& executable_path,
    const std::wstring& type,
    const std::wstring& name,
    const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return false;
    }

    HANDLE update = BeginUpdateResourceW(executable_path.wstring().c_str(), FALSE);
    if (update == nullptr) {
        return false;
    }

    const BOOL ok = UpdateResourceW(
        update,
        type.c_str(),
        name.c_str(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
        const_cast<std::uint8_t*>(bytes.data()),
        static_cast<DWORD>(bytes.size()));
    if (ok == FALSE) {
        EndUpdateResourceW(update, TRUE);
        return false;
    }

    return EndUpdateResourceW(update, FALSE) != FALSE;
}

bool EmbedPngIconResource(const std::filesystem::path& executable_path, const std::vector<std::uint8_t>& png_bytes) {
    if (png_bytes.empty()) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info_from_memory(png_bytes.data(), static_cast<int>(png_bytes.size()), &width, &height, &channels) == 0) {
        return false;
    }

    HANDLE update = BeginUpdateResourceW(executable_path.wstring().c_str(), FALSE);
    if (update == nullptr) {
        return false;
    }

    if (UpdateResourceW(
            update,
            MAKEINTRESOURCEW(kResourceTypeIcon),
            MAKEINTRESOURCEW(1),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
            const_cast<std::uint8_t*>(png_bytes.data()),
            static_cast<DWORD>(png_bytes.size())) == FALSE) {
        EndUpdateResourceW(update, TRUE);
        return false;
    }

    const GrpIconDir header{};
    const GrpIconEntry entry{
        .width = static_cast<BYTE>(width >= 256 ? 0 : width),
        .height = static_cast<BYTE>(height >= 256 ? 0 : height),
        .color_count = 0,
        .reserved = 0,
        .planes = 1,
        .bit_count = 32,
        .bytes_in_res = static_cast<DWORD>(png_bytes.size()),
        .resource_id = 1,
    };
    std::vector<std::uint8_t> group(sizeof(GrpIconDir) + sizeof(GrpIconEntry));
    std::memcpy(group.data(), &header, sizeof(header));
    std::memcpy(group.data() + sizeof(header), &entry, sizeof(entry));

    if (UpdateResourceW(
            update,
            MAKEINTRESOURCEW(kResourceTypeGroupIcon),
            MAKEINTRESOURCEW(1),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
            group.data(),
            static_cast<DWORD>(group.size())) == FALSE) {
        EndUpdateResourceW(update, TRUE);
        return false;
    }

    return EndUpdateResourceW(update, FALSE) != FALSE;
}

}  // namespace novaiso::core

#else

namespace novaiso::core {

std::vector<std::uint8_t> LoadEmbeddedBinaryResource(const std::wstring&, const std::wstring&) {
    return {};
}

bool EmbedBinaryResource(const std::filesystem::path&, const std::wstring&, const std::wstring&, const std::vector<std::uint8_t>&) {
    return false;
}

bool EmbedPngIconResource(const std::filesystem::path&, const std::vector<std::uint8_t>&) {
    return false;
}

}  // namespace novaiso::core

#endif
