#include "core/FileIO.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>

namespace novaiso::core {

namespace {

using json = nlohmann::json;

struct MountedPackState {
    std::filesystem::path pack_path;
    std::filesystem::path virtual_root;
    std::map<std::string, FileIO::PackEntry> entries;
    bool encrypted = false;
    std::uint64_t key = 0;
    std::uint64_t salt = 0;
};

MountedPackState& MountedPack() {
    static MountedPackState state;
    return state;
}

constexpr char kPackMagicV1[8] = {'N', 'P', 'A', 'K', '0', '0', '1', '\0'};
constexpr char kPackMagicV2[8] = {'N', 'P', 'A', 'K', '0', '0', '2', '\0'};
constexpr std::uint64_t kPackFlagEncrypted = 1ull << 0;

struct PackHeaderV1 {
    char magic[8]{};
    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
};

struct PackHeaderV2 {
    char magic[8]{};
    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
    std::uint64_t flags = 0;
    std::uint64_t salt = 0;
    std::uint64_t password_hash = 0;
};

std::uint64_t Fnv1a64(std::string_view text, std::uint64_t seed = 1469598103934665603ull) {
    std::uint64_t hash = seed;
    for (unsigned char value : text) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t HashPassword(std::string_view password, std::uint64_t salt) {
    std::uint64_t hash = Fnv1a64(password);
    for (int shift = 0; shift < 8; ++shift) {
        hash ^= (salt >> (shift * 8)) & 0xFFull;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t DeriveEncryptionKey(std::string_view password, std::uint64_t salt) {
    return HashPassword(password, salt ^ 0x9E3779B97F4A7C15ull) ^ 0xA5A5A5A5D3C4B2F1ull;
}

std::uint64_t RandomSalt() {
    std::random_device device;
    std::uint64_t salt = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
    if (salt == 0) {
        salt = 0xC0DEC0DE5EED1234ull;
    }
    return salt;
}

void CryptBuffer(std::vector<std::uint8_t>& data, std::uint64_t key, std::uint64_t salt, std::uint64_t stream_offset) {
    if (data.empty()) {
        return;
    }
    std::uint64_t state = key ^ salt ^ ((stream_offset + 1ull) * 0x9E3779B97F4A7C15ull);
    if (state == 0) {
        state = 0x6A09E667F3BCC909ull;
    }
    for (std::size_t index = 0; index < data.size(); ++index) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const std::uint64_t random = state * 2685821657736338717ull;
        data[index] ^= static_cast<std::uint8_t>((random >> ((index & 7u) * 8u)) & 0xFFu);
    }
}

std::string NormalizeKey(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

std::optional<std::string> PackLookupKey(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }

    const auto& mounted = MountedPack();
    if (mounted.pack_path.empty()) {
        return std::nullopt;
    }

    if (!path.is_absolute()) {
        const std::string relative_key = NormalizeKey(path);
        if (mounted.entries.contains(relative_key)) {
            return relative_key;
        }
        return std::nullopt;
    }

    std::error_code error;
    const auto absolute = std::filesystem::weakly_canonical(path, error);
    const auto root = std::filesystem::weakly_canonical(mounted.virtual_root, error);
    if (error || absolute.empty() || root.empty()) {
        return std::nullopt;
    }

    const auto relative = absolute.lexically_relative(root);
    const std::string relative_key = NormalizeKey(relative);
    if (relative.empty() || relative_key.rfind("..", 0) == 0) {
        return std::nullopt;
    }

    if (mounted.entries.contains(relative_key)) {
        return relative_key;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> ReadPackBytes(const std::filesystem::path& path) {
    const auto key = PackLookupKey(path);
    if (!key.has_value()) {
        return {};
    }

    const auto& mounted = MountedPack();
    const auto it = mounted.entries.find(*key);
    if (it == mounted.entries.end()) {
        return {};
    }

    std::ifstream stream(mounted.pack_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Failed to open pack file: " + mounted.pack_path.string());
    }
    stream.seekg(static_cast<std::streamoff>(it->second.offset), std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(it->second.size));
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!stream) {
        throw std::runtime_error("Failed to read pack entry: " + *key);
    }
    if (mounted.encrypted) {
        CryptBuffer(data, mounted.key, mounted.salt, it->second.offset);
    }
    return data;
}

}  // namespace

std::string FileIO::ReadText(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        const auto data = ReadPackBytes(path);
        if (!data.empty()) {
            return std::string(reinterpret_cast<const char*>(data.data()), data.size());
        }
    }

    std::ifstream stream(path, std::ios::in);
    if (!stream) {
        throw std::runtime_error("Failed to open text file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void FileIO::WriteText(const std::filesystem::path& path, std::string_view content) {
    EnsureDirectory(path.parent_path());
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("Failed to write text file: " + path.string());
    }
    stream << content;
}

std::vector<std::uint8_t> FileIO::ReadBinary(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        const auto packed = ReadPackBytes(path);
        if (!packed.empty()) {
            return packed;
        }
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Failed to open binary file: " + path.string());
    }
    stream.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(size);
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

bool FileIO::Exists(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        return true;
    }
    return PackLookupKey(path).has_value();
}

void FileIO::EnsureDirectory(const std::filesystem::path& path) {
    if (!path.empty()) {
        std::filesystem::create_directories(path);
    }
}

void FileIO::CopyFileInto(const std::filesystem::path& source, const std::filesystem::path& destination_directory) {
    EnsureDirectory(destination_directory);
    std::filesystem::copy_file(source, destination_directory / source.filename(), std::filesystem::copy_options::overwrite_existing);
}

std::optional<std::filesystem::file_time_type> FileIO::LastWriteTime(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        if (PackLookupKey(path).has_value()) {
            return std::filesystem::file_time_type{};
        }
        return std::nullopt;
    }
    return std::filesystem::last_write_time(path);
}

bool FileIO::CreatePack(
    const std::filesystem::path& pack_path,
    const std::filesystem::path& source_root,
    const std::vector<std::filesystem::path>& relative_paths,
    std::string_view password) {
    EnsureDirectory(pack_path.parent_path());

    std::ofstream stream(pack_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }

    PackHeaderV2 header{};
    std::memcpy(header.magic, kPackMagicV2, sizeof(kPackMagicV2));
    const bool encrypted = !password.empty();
    header.flags = encrypted ? kPackFlagEncrypted : 0ull;
    header.salt = encrypted ? RandomSalt() : 0ull;
    header.password_hash = encrypted ? HashPassword(password, header.salt) : 0ull;
    const std::uint64_t key = encrypted ? DeriveEncryptionKey(password, header.salt) : 0ull;
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));

    json index = json::array();
    std::uint64_t offset = sizeof(header);
    for (const auto& relative_path : relative_paths) {
        const auto full_path = source_root / relative_path;
        if (!std::filesystem::is_regular_file(full_path)) {
            continue;
        }
        auto data = ReadBinary(full_path);
        if (encrypted) {
            CryptBuffer(data, key, header.salt, offset);
        }
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        index.push_back({
            {"path", NormalizeKey(relative_path)},
            {"offset", offset},
            {"size", static_cast<std::uint64_t>(data.size())}
        });
        offset += static_cast<std::uint64_t>(data.size());
    }

    std::string index_text = index.dump();
    header.index_offset = offset;
    header.index_size = static_cast<std::uint64_t>(index_text.size());
    if (encrypted) {
        std::vector<std::uint8_t> index_bytes(index_text.begin(), index_text.end());
        CryptBuffer(index_bytes, key, header.salt, header.index_offset);
        stream.write(reinterpret_cast<const char*>(index_bytes.data()), static_cast<std::streamsize>(index_bytes.size()));
    } else {
        stream.write(index_text.data(), static_cast<std::streamsize>(index_text.size()));
    }
    stream.seekp(0, std::ios::beg);
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    return static_cast<bool>(stream);
}

bool FileIO::MountPack(
    const std::filesystem::path& pack_path,
    const std::filesystem::path& virtual_root,
    std::string_view password) {
    std::ifstream stream(pack_path, std::ios::binary);
    if (!stream) {
        return false;
    }

    char magic[8]{};
    stream.read(magic, sizeof(magic));
    if (!stream) {
        return false;
    }
    stream.seekg(0, std::ios::beg);

    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
    bool encrypted = false;
    std::uint64_t salt = 0;
    std::uint64_t key = 0;
    if (std::memcmp(magic, kPackMagicV2, sizeof(kPackMagicV2)) == 0) {
        PackHeaderV2 header{};
        stream.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!stream) {
            return false;
        }
        encrypted = (header.flags & kPackFlagEncrypted) != 0;
        if (encrypted) {
            if (password.empty()) {
                return false;
            }
            if (HashPassword(password, header.salt) != header.password_hash) {
                return false;
            }
            salt = header.salt;
            key = DeriveEncryptionKey(password, salt);
        }
        index_offset = header.index_offset;
        index_size = header.index_size;
    } else if (std::memcmp(magic, kPackMagicV1, sizeof(kPackMagicV1)) == 0) {
        PackHeaderV1 header{};
        stream.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!stream) {
            return false;
        }
        index_offset = header.index_offset;
        index_size = header.index_size;
    } else {
        return false;
    }

    stream.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
    std::vector<std::uint8_t> index_bytes(static_cast<std::size_t>(index_size));
    stream.read(reinterpret_cast<char*>(index_bytes.data()), static_cast<std::streamsize>(index_bytes.size()));
    if (!stream) {
        return false;
    }
    if (encrypted) {
        CryptBuffer(index_bytes, key, salt, index_offset);
    }

    const json root = json::parse(index_bytes.begin(), index_bytes.end(), nullptr, false);
    if (!root.is_array()) {
        return false;
    }

    MountedPackState mounted;
    mounted.pack_path = pack_path;
    mounted.virtual_root = virtual_root.empty() ? pack_path.parent_path() : virtual_root;
    mounted.encrypted = encrypted;
    mounted.key = key;
    mounted.salt = salt;
    for (const auto& entry : root) {
        const std::string key = entry.value("path", "");
        if (key.empty()) {
            continue;
        }
        mounted.entries[key] = PackEntry{
            .offset = entry.value("offset", std::uint64_t{0}),
            .size = entry.value("size", std::uint64_t{0}),
        };
    }

    MountedPack() = std::move(mounted);
    return true;
}

void FileIO::UnmountPack() {
    MountedPack() = MountedPackState{};
}

bool FileIO::PackMounted() {
    return !MountedPack().pack_path.empty();
}

std::vector<std::filesystem::path> FileIO::MountedPackFiles() {
    std::vector<std::filesystem::path> files;
    files.reserve(MountedPack().entries.size());
    for (const auto& [path, _] : MountedPack().entries) {
        files.emplace_back(path);
    }
    return files;
}

}  // namespace novaiso::core
