#pragma once

#include "assets/AssetManager.h"
#include "assets/Project.h"
#include "core/Application.h"
#include "entities/Scene.h"
#include "renderer/Framebuffer.h"
#include "renderer/PostProcessStack.h"
#include "renderer/Renderer2D.h"
#include "scripting/PythonScripting.h"
#include "ui/HtmlUiRuntime.h"

#include <filesystem>
#include <vector>

namespace novaiso::runtime {

class RuntimeApp : public core::Application {
public:
    explicit RuntimeApp(std::filesystem::path project_root = {});

protected:
    bool OnInit() override;
    void OnShutdown() override;
    void OnUpdate(float delta_time) override;
    void OnRender() override;
    void OnGui() override;

private:
    void LoadProject(const std::filesystem::path& root);
    void LoadSplashTexture();
    void LoadUserSettings();
    void SaveUserSettings() const;
    void RefreshAvailableResolutions();
    void ApplyUserSettings(bool apply_window_mode);
    void OpenSettingsMenu(bool from_pause);
    void CloseSettingsMenu();
    void StepResolution(int direction);
    void AdjustMasterVolume(float delta);
    void AdjustMusicVolume(float delta);
    void AdjustSoundVolume(float delta);
    void DrawSplashOverlay();
    void DrawMenuOverlay(const assets::MenuDefinition& menu, const char* id, bool pause_context);
    void DrawSettingsOverlay();
    void DrawHudOverlay();
    void HandleMenuAction(const assets::MenuButtonDefinition& button);
    void HandleUiAction(const std::string& action);
    [[nodiscard]] ui::HtmlUiRuntime::DynamicContext BuildHtmlUiContext() const;
    [[nodiscard]] std::string ResolveUiText(const std::string& text) const;
    [[nodiscard]] std::filesystem::path UserSettingsPath() const;
    [[nodiscard]] std::string CurrentResolutionText() const;

    std::filesystem::path project_root_;
    assets::ProjectData project_;
    assets::AssetManager asset_manager_;
    scripting::PythonScripting scripting_;
    entities::Scene scene_;
    renderer::Renderer2D renderer_;
    renderer::PostProcessStack post_stack_;
    renderer::Framebuffer scene_target_;
    renderer::Texture2D splash_texture_;
    ui::HtmlUiRuntime html_ui_;
    float elapsed_time_ = 0.0f;
    float splash_timer_ = 0.0f;
    float splash_duration_ = 2.4f;
    bool show_splash_ = false;
    bool show_main_menu_ = false;
    bool show_pause_menu_ = false;
    bool show_settings_menu_ = false;
    bool settings_from_pause_ = false;
    std::vector<glm::ivec2> available_resolutions_;
    int active_resolution_index_ = 0;
    struct UserSettings {
        glm::ivec2 resolution{1600, 900};
        bool fullscreen = false;
        float master_volume = 1.0f;
        float music_volume = 1.0f;
        float sound_volume = 1.0f;
    } user_settings_;
};

}  // namespace novaiso::runtime
