#include "assets/AssetManager.h"

#include "core/FileIO.h"

#include <SDL_mixer.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <system_error>

namespace novaiso::assets {

namespace {

bool MixerReady() {
    return Mix_QuerySpec(nullptr, nullptr, nullptr) != 0;
}

std::filesystem::path MusicCachePathFor(const std::filesystem::path& source_path) {
    std::error_code error;
    std::filesystem::path root = std::filesystem::temp_directory_path(error);
    if (error || root.empty()) {
        root = std::filesystem::current_path(error);
    }
    if (root.empty()) {
        root = ".";
    }

    const std::filesystem::path cache_dir = root / "NovaIso-Engine" / "music_cache";
    std::filesystem::create_directories(cache_dir, error);

    const std::string normalized = source_path.lexically_normal().generic_string();
    const auto hash = std::hash<std::string>{}(normalized);
    std::filesystem::path extension = source_path.extension();
    if (extension.empty()) {
        extension = ".bin";
    }
    return cache_dir / ("music_" + std::to_string(hash) + extension.string());
}

bool WritePackedMusicToTempFile(const std::filesystem::path& source_path,
                                const std::vector<std::uint8_t>& bytes,
                                std::filesystem::path& output_path) {
    if (bytes.empty()) {
        return false;
    }

    output_path = MusicCachePathFor(source_path);
    std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        output_path.clear();
        return false;
    }

    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const bool ok = static_cast<bool>(stream);
    stream.close();

    if (!ok) {
        std::error_code error;
        std::filesystem::remove(output_path, error);
        output_path.clear();
    }
    return ok;
}

void RemoveIfExists(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

AssetManager::~AssetManager() {
    Shutdown();
}

bool AssetManager::Initialize(const std::filesystem::path& project_root) {
    SetProjectRoot(project_root);

    Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG);
    audio_enabled_ = Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 2048) == 0;

    fallback_texture_.CreateWhitePixel();
    return true;
}

void AssetManager::Shutdown() {
    StopAllAudio();

    for (auto& [_, sound] : sounds_) {
        if (sound.chunk != nullptr) {
            Mix_FreeChunk(sound.chunk);
        }
    }
    sounds_.clear();

    for (auto& [_, music] : music_) {
        if (music.track != nullptr) {
            Mix_FreeMusic(music.track);
        }
        RemoveIfExists(music.temp_file);
    }
    music_.clear();

    textures_.clear();
    fallback_texture_.Destroy();

    if (MixerReady()) {
        Mix_CloseAudio();
    }
    Mix_Quit();
    audio_enabled_ = false;
}

void AssetManager::SetProjectRoot(const std::filesystem::path& project_root) {
    project_root_ = project_root;
}

const std::filesystem::path& AssetManager::ProjectRoot() const {
    return project_root_;
}

std::filesystem::path AssetManager::Resolve(const std::filesystem::path& path) const {
    return path.is_absolute() ? path : (project_root_ / path);
}

void AssetManager::SetAudioEnabled(bool enabled) {
    audio_enabled_ = enabled;
    if (!audio_enabled_) {
        StopAllAudio();
    }
}

bool AssetManager::AudioEnabled() const {
    return audio_enabled_;
}

void AssetManager::SetMasterVolume(float volume) {
    master_volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (!MixerReady()) {
        return;
    }
    Mix_Volume(-1, static_cast<int>(MIX_MAX_VOLUME * master_volume_ * sound_volume_));
    Mix_VolumeMusic(static_cast<int>(MIX_MAX_VOLUME * master_volume_ * music_volume_));
}

void AssetManager::SetMusicVolume(float volume) {
    music_volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (!MixerReady()) {
        return;
    }
    Mix_VolumeMusic(static_cast<int>(MIX_MAX_VOLUME * master_volume_ * music_volume_));
}

void AssetManager::SetSoundVolume(float volume) {
    sound_volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (!MixerReady()) {
        return;
    }
    Mix_Volume(-1, static_cast<int>(MIX_MAX_VOLUME * master_volume_ * sound_volume_));
}

float AssetManager::MasterVolume() const {
    return master_volume_;
}

float AssetManager::MusicVolume() const {
    return music_volume_;
}

float AssetManager::SoundVolume() const {
    return sound_volume_;
}

void AssetManager::StopAllAudio() const {
    if (!MixerReady()) {
        return;
    }
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
}

renderer::Texture2D& AssetManager::LoadTexture(const std::string& path) {
    if (path.empty()) {
        return fallback_texture_;
    }

    const auto key = KeyFor(path);
    const auto full_path = Resolve(path);
    const auto timestamp = core::FileIO::LastWriteTime(full_path);

    if (auto it = textures_.find(key); it != textures_.end()) {
        const bool unchanged =
            (!timestamp.has_value() && !it->second.has_last_write) ||
            (timestamp.has_value() && it->second.has_last_write && it->second.last_write == *timestamp);
        if (unchanged) {
            return it->second.texture;
        }
        bool loaded = false;
        if (core::FileIO::Exists(full_path)) {
            if (std::filesystem::exists(full_path)) {
                loaded = it->second.texture.LoadFromFile(full_path);
            } else {
                const auto bytes = core::FileIO::ReadBinary(full_path);
                loaded = it->second.texture.LoadFromMemory(bytes.data(), bytes.size(), full_path);
            }
        }
        if (loaded) {
            it->second.has_last_write = timestamp.has_value();
            if (timestamp.has_value()) {
                it->second.last_write = *timestamp;
            }
            return it->second.texture;
        }
        return fallback_texture_;
    }

    TextureRecord record;
    bool loaded = false;
    if (std::filesystem::exists(full_path)) {
        loaded = record.texture.LoadFromFile(full_path);
    } else if (core::FileIO::Exists(full_path)) {
        const auto bytes = core::FileIO::ReadBinary(full_path);
        loaded = record.texture.LoadFromMemory(bytes.data(), bytes.size(), full_path);
    }
    if (!loaded) {
        return fallback_texture_;
    }
    record.has_last_write = timestamp.has_value();
    if (timestamp.has_value()) {
        record.last_write = *timestamp;
    }
    auto [inserted_it, _] = textures_.emplace(key, std::move(record));
    return inserted_it->second.texture;
}

renderer::Texture2D& AssetManager::FallbackTexture() {
    return fallback_texture_;
}

Mix_Chunk* AssetManager::LoadSound(const std::string& path) {
    if (path.empty()) {
        return nullptr;
    }

    const auto key = KeyFor(path);
    if (const auto it = sounds_.find(key); it != sounds_.end()) {
        return it->second.chunk;
    }

    const auto full_path = Resolve(path);
    if (!core::FileIO::Exists(full_path)) {
        return nullptr;
    }

    SoundRecord record;
    if (std::filesystem::exists(full_path)) {
        record.chunk = Mix_LoadWAV(full_path.string().c_str());
    } else {
        record.bytes = core::FileIO::ReadBinary(full_path);
        SDL_RWops* rw = SDL_RWFromConstMem(record.bytes.data(), static_cast<int>(record.bytes.size()));
        if (rw != nullptr) {
            record.chunk = Mix_LoadWAV_RW(rw, 1);
        }
    }
    if (record.chunk != nullptr) {
        sounds_[key] = std::move(record);
    }
    const auto it = sounds_.find(key);
    return it != sounds_.end() ? it->second.chunk : nullptr;
}

Mix_Music* AssetManager::LoadMusic(const std::string& path) {
    if (path.empty() || !MixerReady()) {
        return nullptr;
    }

    const auto key = KeyFor(path);
    if (const auto it = music_.find(key); it != music_.end()) {
        return it->second.track;
    }

    const auto full_path = Resolve(path);
    if (!core::FileIO::Exists(full_path)) {
        return nullptr;
    }

    MusicRecord record;
    if (std::filesystem::exists(full_path)) {
        record.track = Mix_LoadMUS(full_path.string().c_str());
    } else {
        record.bytes = core::FileIO::ReadBinary(full_path);
        if (WritePackedMusicToTempFile(full_path, record.bytes, record.temp_file)) {
            record.track = Mix_LoadMUS(record.temp_file.string().c_str());
        }
        if (record.track == nullptr) {
            RemoveIfExists(record.temp_file);
            record.temp_file.clear();
        }
    }
    if (record.track != nullptr) {
        music_[key] = std::move(record);
    }
    const auto it = music_.find(key);
    return it != music_.end() ? it->second.track : nullptr;
}

void AssetManager::PlaySound(const std::string& path) {
    if (!audio_enabled_) {
        return;
    }
    PlaySoundChannel(path, 0, 1.0f);
}

void AssetManager::PlayMusic(const std::string& path, bool loop) {
    if (!audio_enabled_ || !MixerReady()) {
        return;
    }
    if (Mix_Music* track = LoadMusic(path); track != nullptr) {
        if (Mix_PlayMusic(track, loop ? -1 : 0) == 0) {
            Mix_VolumeMusic(static_cast<int>(MIX_MAX_VOLUME * master_volume_ * music_volume_));
        }
    }
}

int AssetManager::PlaySoundChannel(const std::string& path, int loops, float volume) {
    if (!audio_enabled_ || !MixerReady()) {
        return -1;
    }
    if (Mix_Chunk* chunk = LoadSound(path); chunk != nullptr) {
        const int channel = Mix_PlayChannel(-1, chunk, loops);
        if (channel >= 0) {
            SetChannelVolume(channel, volume);
        }
        return channel;
    }
    return -1;
}

void AssetManager::SetChannelVolume(int channel, float volume) const {
    if (channel < 0 || !MixerReady()) {
        return;
    }
    const float clamped = std::clamp(volume, 0.0f, 1.0f);
    Mix_Volume(channel, static_cast<int>(MIX_MAX_VOLUME * master_volume_ * sound_volume_ * clamped));
}

void AssetManager::SetChannelDistance(int channel, float normalized_distance) const {
    if (channel < 0 || !MixerReady()) {
        return;
    }
    const float clamped = std::clamp(normalized_distance, 0.0f, 1.0f);
    Mix_SetPosition(channel, 0, static_cast<Uint8>(clamped * 255.0f));
}

void AssetManager::HaltChannel(int channel) const {
    if (channel < 0 || !MixerReady()) {
        return;
    }
    Mix_HaltChannel(channel);
}

bool AssetManager::IsChannelPlaying(int channel) const {
    if (channel < 0 || !MixerReady()) {
        return false;
    }
    return Mix_Playing(channel) != 0;
}

std::filesystem::path AssetManager::ImportFile(const std::filesystem::path& source) {
    const auto relative_target = ImportTargetFor(source);
    core::FileIO::EnsureDirectory((project_root_ / relative_target).parent_path());
    std::filesystem::copy_file(source, project_root_ / relative_target, std::filesystem::copy_options::overwrite_existing);
    return relative_target;
}

std::vector<std::filesystem::path> AssetManager::EnumerateAssets() const {
    std::vector<std::filesystem::path> files;
    const auto root = project_root_ / "assets";
    if (!std::filesystem::exists(root)) {
        return files;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files.push_back(std::filesystem::relative(entry.path(), project_root_));
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string AssetManager::KeyFor(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

bool AssetManager::IsImagePath(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
           extension == ".tga" || extension == ".ppm" || extension == ".gif";
}

bool AssetManager::IsAudioPath(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3";
}

bool AssetManager::IsShaderPath(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".frag" || extension == ".vert" || extension == ".glsl" || extension == ".shader";
}

std::filesystem::path AssetManager::ImportTargetFor(const std::filesystem::path& source) const {
    if (IsImagePath(source)) {
        return std::filesystem::path("assets/images") / source.filename();
    }
    if (IsAudioPath(source)) {
        return std::filesystem::path("assets/audio") / source.filename();
    }
    if (IsShaderPath(source)) {
        return std::filesystem::path("shaders") / source.filename();
    }
    if (source.extension() == ".py") {
        return std::filesystem::path("scripts") / source.filename();
    }
    if (source.extension() == ".niso") {
        return std::filesystem::path("levels") / source.filename();
    }
    return std::filesystem::path("assets/imported") / source.filename();
}

}  // namespace novaiso::assets

