#pragma once

#include "renderer/Texture2D.h"

#include <SDL_mixer.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace novaiso::assets {

class AssetManager {
public:
    AssetManager() = default;
    ~AssetManager();

    bool Initialize(const std::filesystem::path& project_root);
    void Shutdown();

    void SetProjectRoot(const std::filesystem::path& project_root);
    [[nodiscard]] const std::filesystem::path& ProjectRoot() const;
    [[nodiscard]] std::filesystem::path Resolve(const std::filesystem::path& path) const;

    void SetAudioEnabled(bool enabled);
    [[nodiscard]] bool AudioEnabled() const;
    void SetMasterVolume(float volume);
    void SetMusicVolume(float volume);
    void SetSoundVolume(float volume);
    [[nodiscard]] float MasterVolume() const;
    [[nodiscard]] float MusicVolume() const;
    [[nodiscard]] float SoundVolume() const;
    void StopAllAudio() const;

    renderer::Texture2D& LoadTexture(const std::string& path);
    renderer::Texture2D& FallbackTexture();

    Mix_Chunk* LoadSound(const std::string& path);
    Mix_Music* LoadMusic(const std::string& path);
    void PlaySound(const std::string& path);
    void PlayMusic(const std::string& path, bool loop);
    int PlaySoundChannel(const std::string& path, int loops = 0, float volume = 1.0f);
    void SetChannelVolume(int channel, float volume) const;
    void SetChannelDistance(int channel, float normalized_distance) const;
    void HaltChannel(int channel) const;
    [[nodiscard]] bool IsChannelPlaying(int channel) const;

    [[nodiscard]] std::filesystem::path ImportFile(const std::filesystem::path& source);
    [[nodiscard]] std::vector<std::filesystem::path> EnumerateAssets() const;

private:
    struct TextureRecord {
        renderer::Texture2D texture;
        std::filesystem::file_time_type last_write{};
        bool has_last_write = false;
    };

    struct SoundRecord {
        Mix_Chunk* chunk = nullptr;
        std::vector<std::uint8_t> bytes;
    };

    struct MusicRecord {
        Mix_Music* track = nullptr;
        std::vector<std::uint8_t> bytes;
        std::filesystem::path temp_file;
    };

    static std::string KeyFor(const std::filesystem::path& path);
    static bool IsImagePath(const std::filesystem::path& path);
    static bool IsAudioPath(const std::filesystem::path& path);
    static bool IsShaderPath(const std::filesystem::path& path);
    [[nodiscard]] std::filesystem::path ImportTargetFor(const std::filesystem::path& source) const;

    std::filesystem::path project_root_;
    renderer::Texture2D fallback_texture_;
    std::unordered_map<std::string, TextureRecord> textures_;
    std::unordered_map<std::string, SoundRecord> sounds_;
    std::unordered_map<std::string, MusicRecord> music_;
    bool audio_enabled_ = true;
    float master_volume_ = 1.0f;
    float music_volume_ = 1.0f;
    float sound_volume_ = 1.0f;
};

}  // namespace novaiso::assets
