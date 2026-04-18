#include "runtime/RuntimeApp.h"

#include <imgui.h>
#include <SDL.h>
#include <algorithm>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/EmbeddedResources.h"
#include "core/FileIO.h"
#include "core/ThreadPool.h"

namespace novaiso::runtime {

namespace {

glm::ivec2 FitSizeInside(glm::ivec2 outer, glm::ivec2 reference) {
    outer.x = std::max(outer.x, 1);
    outer.y = std::max(outer.y, 1);
    reference.x = std::max(reference.x, 1);
    reference.y = std::max(reference.y, 1);
    const float scale = std::min(
        static_cast<float>(outer.x) / static_cast<float>(reference.x),
        static_cast<float>(outer.y) / static_cast<float>(reference.y));
    return {
        std::max(static_cast<int>(std::round(reference.x * scale)), 1),
        std::max(static_cast<int>(std::round(reference.y * scale)), 1)
    };
}

std::string ResolutionText(glm::ivec2 resolution) {
    return std::to_string(std::max(resolution.x, 1)) + " x " + std::to_string(std::max(resolution.y, 1));
}

int VolumePercent(float value) {
    return static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * 100.0f));
}

std::size_t ResolveProjectWorkerCount(const assets::ProjectData& project) {
    if (!project.multithreading_enabled) {
        return 0;
    }
    if (project.worker_threads > 0) {
        return static_cast<std::size_t>(project.worker_threads);
    }
    return core::ThreadPool::AutoWorkerCount();
}

}  // namespace

RuntimeApp::RuntimeApp(std::filesystem::path project_root)
    : Application({
          .title = "NovaIso Runtime",
          .width = 1600,
          .height = 900,
          .enable_gui = true,
          .maximized = false,
          .vsync = true,
      }),
      project_root_(std::move(project_root)) {}

bool RuntimeApp::OnInit() {
    const auto pack_password = [&]() {
        const auto bytes = core::LoadEmbeddedBinaryResource(L"NOVAISO_BIN", L"NOVAISO_PACK_PASSWORD");
        if (bytes.empty()) {
            return std::string{};
        }
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }();
    const auto root = [&]() {
        core::FileIO::UnmountPack();
        if (!project_root_.empty()) {
            if (std::filesystem::is_regular_file(project_root_) && project_root_.extension() == ".npak") {
                if (core::FileIO::MountPack(project_root_, ExecutableDirectory(), pack_password)) {
                    return ExecutableDirectory();
                }
            }
            if (std::filesystem::exists(project_root_ / "content.npak")) {
                core::FileIO::MountPack(project_root_ / "content.npak", project_root_, pack_password);
            }
            return project_root_;
        }
        if (std::filesystem::exists(ExecutableDirectory() / "content.npak")) {
            if (core::FileIO::MountPack(ExecutableDirectory() / "content.npak", ExecutableDirectory(), pack_password)) {
                return ExecutableDirectory();
            }
        }
        if (std::filesystem::exists(ExecutableDirectory() / "project.json")) {
            return ExecutableDirectory();
        }
        return ExecutableDirectory() / "demo_project";
    }();
    LoadProject(root);
    return true;
}

void RuntimeApp::OnShutdown() {
    post_stack_.Shutdown();
    renderer_.Shutdown();
    asset_manager_.Shutdown();
    scripting_.Shutdown();
    html_ui_.Clear();
    core::FileIO::UnmountPack();
}

void RuntimeApp::OnUpdate(float delta_time) {
    elapsed_time_ += delta_time;

    if (GetInput().WasKeyPressed(SDL_SCANCODE_F11)) {
        user_settings_.fullscreen = !user_settings_.fullscreen;
        ApplyUserSettings(true);
        SaveUserSettings();
    }
    if (show_splash_) {
        splash_timer_ += delta_time;
        if (splash_timer_ >= splash_duration_) {
            show_splash_ = false;
        }
        return;
    }
    if (show_settings_menu_) {
        if (GetInput().WasKeyPressed(SDL_SCANCODE_ESCAPE)) {
            CloseSettingsMenu();
        }
        return;
    }
    if (show_main_menu_) {
        if (GetInput().WasKeyPressed(SDL_SCANCODE_ESCAPE)) {
            RequestQuit();
        }
        return;
    }

    if (GetInput().WasKeyPressed(SDL_SCANCODE_ESCAPE)) {
        if (project_.pause_menu.enabled) {
            show_pause_menu_ = !show_pause_menu_;
        } else {
            RequestQuit();
        }
    }
    if (show_pause_menu_) {
        return;
    }
    if (GetInput().WasKeyPressed(SDL_SCANCODE_TAB)) {
        scene_.ToggleCameraMode();
    }

    scene_.Update(delta_time, GetInput());
}

void RuntimeApp::OnRender() {
    const glm::ivec2 size = WindowSize();
    const glm::ivec2 render_size = FitSizeInside(size, project_.game_viewport_size);
    scene_.Camera().SetViewport(glm::vec2(project_.game_viewport_size));
    scene_target_.Resize(render_size);

    scene_target_.Bind();
    const auto clear = scene_.Level().clear_color;
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);

    renderer_.Begin(scene_.Camera());
    scene_.Render(renderer_, false);
    renderer_.Flush();

    const GLuint final_texture = post_stack_.Process(scene_target_.Texture(), scene_target_.Size(), scene_.Level().post_effects, elapsed_time_);
    renderer::Framebuffer::BindDefault(size);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    post_stack_.BlitToDefaultFitted(final_texture, scene_target_.Size(), size, elapsed_time_);
}

void RuntimeApp::OnGui() {
    if (show_splash_) {
        DrawSplashOverlay();
        return;
    }
    const auto html_context = BuildHtmlUiContext();
    if (show_settings_menu_) {
        if (html_ui_.HasScreen("settings-menu")) {
            html_ui_.RenderScreen("settings-menu", html_context, [&](const std::string& text) { return ResolveUiText(text); },
                [&](const std::string& action) { HandleUiAction(action); });
        } else {
            DrawSettingsOverlay();
        }
    } else if (show_main_menu_) {
        if (html_ui_.HasScreen("main-menu")) {
            html_ui_.RenderScreen("main-menu", html_context, [&](const std::string& text) { return ResolveUiText(text); },
                [&](const std::string& action) { HandleUiAction(action); });
        } else {
            DrawMenuOverlay(project_.main_menu, "MainMenuOverlay", false);
        }
    } else if (show_pause_menu_) {
        if (html_ui_.HasScreen("pause-menu")) {
            html_ui_.RenderScreen("pause-menu", html_context, [&](const std::string& text) { return ResolveUiText(text); },
                [&](const std::string& action) { HandleUiAction(action); });
        } else {
            DrawMenuOverlay(project_.pause_menu, "PauseMenuOverlay", true);
        }
    }

    if (!show_main_menu_) {
        if (html_ui_.HasScreen("hud")) {
            html_ui_.RenderScreen("hud", html_context, [&](const std::string& text) { return ResolveUiText(text); },
                [&](const std::string& action) { HandleUiAction(action); });
        } else {
            DrawHudOverlay();
        }
    }
}

void RuntimeApp::LoadProject(const std::filesystem::path& root) {
    project_root_ = root;
    project_ = assets::LoadProject(project_root_ / "project.json");
    project_.worker_threads = std::max(project_.worker_threads, 0);
    project_.game_viewport_size.x = std::max(project_.game_viewport_size.x, 320);
    project_.game_viewport_size.y = std::max(project_.game_viewport_size.y, 180);
    SetWindowTitle(project_.name + " v" + project_.version);
    if (!project_.icon.empty()) {
        SetWindowIcon(project_root_ / project_.icon);
    }
    RefreshAvailableResolutions();
    LoadUserSettings();
    ApplyUserSettings(true);
    asset_manager_.Initialize(project_root_);
    core::ThreadPool::Shared().Configure(ResolveProjectWorkerCount(project_));
    asset_manager_.SetMasterVolume(user_settings_.master_volume);
    asset_manager_.SetMusicVolume(user_settings_.music_volume);
    asset_manager_.SetSoundVolume(user_settings_.sound_volume);
    scripting_.Initialize(project_root_);
    const std::filesystem::path builtin_shader_root = ExecutableDirectory() / "shaders";
    const std::filesystem::path project_shader_root = project_root_ / "shaders";
    if (std::filesystem::exists(builtin_shader_root)) {
        core::FileIO::EnsureDirectory(project_shader_root);
        const std::filesystem::path obsolete_reflection = project_shader_root / "wet_reflection.frag";
        if (std::filesystem::exists(obsolete_reflection)) {
            std::filesystem::remove(obsolete_reflection);
        }
        for (const auto& entry : std::filesystem::directory_iterator(builtin_shader_root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::filesystem::path destination = project_shader_root / entry.path().filename();
            if (!std::filesystem::exists(destination)) {
                std::filesystem::copy_file(entry.path(), destination, std::filesystem::copy_options::overwrite_existing);
            }
        }
    }
    const std::filesystem::path shader_root = core::FileIO::Exists(project_root_ / "shaders" / "post.vert")
        ? (project_root_ / "shaders")
        : (ExecutableDirectory() / "shaders");
    renderer_.Initialize(shader_root);
    post_stack_.Initialize(shader_root);
    html_ui_.Load(project_root_, project_.html_ui);

    const std::string level_path = project_.startup_level.empty() && !project_.levels.empty() ? project_.levels.front() : project_.startup_level;
    scene_.Load(project_root_, project_, level_path, asset_manager_, scripting_);
    LoadSplashTexture();
    show_main_menu_ = project_.main_menu.enabled;
    show_pause_menu_ = false;
    show_settings_menu_ = false;
    if (show_main_menu_ && !project_.main_menu.background_music.empty()) {
        asset_manager_.PlayMusic(project_.main_menu.background_music, project_.main_menu.music_loop);
    }
}

std::filesystem::path RuntimeApp::UserSettingsPath() const {
    return project_root_ / "user_settings.json";
}

void RuntimeApp::RefreshAvailableResolutions() {
    std::set<std::pair<int, int>> unique_modes;
    const int display_index = Window() != nullptr ? SDL_GetWindowDisplayIndex(Window()) : 0;
    const int mode_count = SDL_GetNumDisplayModes(display_index);
    for (int i = 0; i < mode_count; ++i) {
        SDL_DisplayMode mode{};
        if (SDL_GetDisplayMode(display_index, i, &mode) == 0 && mode.w >= 640 && mode.h >= 360) {
            unique_modes.emplace(mode.w, mode.h);
        }
    }

    static constexpr std::array<glm::ivec2, 10> kFallbackModes{{
        {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {1920, 1200},
        {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 1600}, {3840, 2160}
    }};
    for (const auto& mode : kFallbackModes) {
        unique_modes.emplace(mode.x, mode.y);
    }
    unique_modes.emplace(project_.game_viewport_size.x, project_.game_viewport_size.y);

    available_resolutions_.clear();
    for (const auto& [width, height] : unique_modes) {
        available_resolutions_.push_back({width, height});
    }
    if (available_resolutions_.empty()) {
        available_resolutions_.push_back(project_.game_viewport_size);
    }
}

void RuntimeApp::LoadUserSettings() {
    user_settings_.resolution = project_.game_viewport_size;
    user_settings_.fullscreen = false;
    user_settings_.master_volume = 1.0f;
    user_settings_.music_volume = 1.0f;
    user_settings_.sound_volume = 1.0f;

    const std::filesystem::path path = UserSettingsPath();
    if (std::filesystem::exists(path)) {
        const auto parsed = nlohmann::json::parse(core::FileIO::ReadText(path), nullptr, false);
        if (parsed.is_object()) {
            user_settings_.resolution = {
                std::max(parsed.value("resolution_width", user_settings_.resolution.x), 640),
                std::max(parsed.value("resolution_height", user_settings_.resolution.y), 360)
            };
            user_settings_.fullscreen = parsed.value("fullscreen", false);
            user_settings_.master_volume = std::clamp(parsed.value("master_volume", 1.0f), 0.0f, 1.0f);
            user_settings_.music_volume = std::clamp(parsed.value("music_volume", 1.0f), 0.0f, 1.0f);
            user_settings_.sound_volume = std::clamp(parsed.value("sound_volume", 1.0f), 0.0f, 1.0f);
        }
    }

    auto it = std::find(available_resolutions_.begin(), available_resolutions_.end(), user_settings_.resolution);
    if (it == available_resolutions_.end()) {
        available_resolutions_.push_back(user_settings_.resolution);
        std::sort(available_resolutions_.begin(), available_resolutions_.end(), [](glm::ivec2 lhs, glm::ivec2 rhs) {
            if (lhs.x != rhs.x) {
                return lhs.x < rhs.x;
            }
            return lhs.y < rhs.y;
        });
        it = std::find(available_resolutions_.begin(), available_resolutions_.end(), user_settings_.resolution);
    }
    active_resolution_index_ = it != available_resolutions_.end()
        ? static_cast<int>(std::distance(available_resolutions_.begin(), it))
        : 0;
}

void RuntimeApp::SaveUserSettings() const {
    nlohmann::json root = {
        {"resolution_width", user_settings_.resolution.x},
        {"resolution_height", user_settings_.resolution.y},
        {"fullscreen", user_settings_.fullscreen},
        {"master_volume", user_settings_.master_volume},
        {"music_volume", user_settings_.music_volume},
        {"sound_volume", user_settings_.sound_volume}
    };
    core::FileIO::WriteText(UserSettingsPath(), root.dump(2));
}

void RuntimeApp::ApplyUserSettings(bool apply_window_mode) {
    user_settings_.resolution.x = std::max(user_settings_.resolution.x, 640);
    user_settings_.resolution.y = std::max(user_settings_.resolution.y, 360);
    user_settings_.master_volume = std::clamp(user_settings_.master_volume, 0.0f, 1.0f);
    user_settings_.music_volume = std::clamp(user_settings_.music_volume, 0.0f, 1.0f);
    user_settings_.sound_volume = std::clamp(user_settings_.sound_volume, 0.0f, 1.0f);

    if (apply_window_mode && Window() != nullptr) {
        SDL_DisplayMode mode{};
        mode.w = user_settings_.resolution.x;
        mode.h = user_settings_.resolution.y;
        mode.format = SDL_PIXELFORMAT_UNKNOWN;
        mode.refresh_rate = 0;
        mode.driverdata = nullptr;

        if (user_settings_.fullscreen) {
            SDL_SetWindowDisplayMode(Window(), &mode);
            if (SDL_SetWindowFullscreen(Window(), SDL_WINDOW_FULLSCREEN) != 0) {
                SDL_SetWindowFullscreen(Window(), SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        } else {
            SDL_SetWindowFullscreen(Window(), 0);
            SDL_SetWindowSize(Window(), user_settings_.resolution.x, user_settings_.resolution.y);
            SDL_SetWindowPosition(Window(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }

    if (asset_manager_.AudioEnabled()) {
        asset_manager_.SetMasterVolume(user_settings_.master_volume);
        asset_manager_.SetMusicVolume(user_settings_.music_volume);
        asset_manager_.SetSoundVolume(user_settings_.sound_volume);
    }
}

void RuntimeApp::OpenSettingsMenu(bool from_pause) {
    settings_from_pause_ = from_pause;
    show_settings_menu_ = true;
}

void RuntimeApp::CloseSettingsMenu() {
    show_settings_menu_ = false;
    if (!settings_from_pause_) {
        show_pause_menu_ = false;
    }
}

void RuntimeApp::StepResolution(int direction) {
    if (available_resolutions_.empty()) {
        return;
    }
    active_resolution_index_ = std::clamp(active_resolution_index_ + direction, 0, static_cast<int>(available_resolutions_.size()) - 1);
    user_settings_.resolution = available_resolutions_[static_cast<std::size_t>(active_resolution_index_)];
    ApplyUserSettings(true);
    SaveUserSettings();
}

void RuntimeApp::AdjustMasterVolume(float delta) {
    user_settings_.master_volume = std::clamp(user_settings_.master_volume + delta, 0.0f, 1.0f);
    ApplyUserSettings(false);
    SaveUserSettings();
}

void RuntimeApp::AdjustMusicVolume(float delta) {
    user_settings_.music_volume = std::clamp(user_settings_.music_volume + delta, 0.0f, 1.0f);
    ApplyUserSettings(false);
    SaveUserSettings();
}

void RuntimeApp::AdjustSoundVolume(float delta) {
    user_settings_.sound_volume = std::clamp(user_settings_.sound_volume + delta, 0.0f, 1.0f);
    ApplyUserSettings(false);
    SaveUserSettings();
}

void RuntimeApp::LoadSplashTexture() {
    splash_texture_.Destroy();
    show_splash_ = false;
    splash_timer_ = 0.0f;
    if (!project_.splash_enabled) {
        return;
    }

    const auto embedded = core::LoadEmbeddedBinaryResource(L"NOVAISO_BIN", L"NOVAISO_SPLASH");
    if (!embedded.empty() && splash_texture_.LoadFromMemory(embedded.data(), embedded.size(), "embedded://splash")) {
        show_splash_ = true;
        return;
    }

    std::filesystem::path splash_source;
    if (!project_.splash_image.empty()) {
        splash_source = std::filesystem::path(project_.splash_image);
        if (!splash_source.is_absolute()) {
            splash_source = project_root_ / splash_source;
        }
    } else if (core::FileIO::Exists(project_root_ / "logo.png")) {
        splash_source = project_root_ / "logo.png";
    } else if (core::FileIO::Exists(ExecutableDirectory() / "logo.png")) {
        splash_source = ExecutableDirectory() / "logo.png";
    }

    if (!splash_source.empty() && core::FileIO::Exists(splash_source)) {
        const auto bytes = core::FileIO::ReadBinary(splash_source);
        if (!bytes.empty() && splash_texture_.LoadFromMemory(bytes.data(), bytes.size(), splash_source)) {
            show_splash_ = true;
        }
    }
}

void RuntimeApp::DrawSplashOverlay() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("NovaIsoSplash", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs);

    const float fade_in = std::clamp(splash_timer_ / 0.55f, 0.0f, 1.0f);
    const float fade_out = std::clamp((splash_duration_ - splash_timer_) / 0.55f, 0.0f, 1.0f);
    const float alpha = std::min(fade_in, fade_out);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        viewport->Pos,
        {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y},
        IM_COL32(6, 8, 14, 255));

    const ImVec2 center{
        viewport->Pos.x + viewport->Size.x * 0.5f,
        viewport->Pos.y + viewport->Size.y * 0.5f
    };

    if (splash_texture_.Id() != 0) {
        const glm::ivec2 texture_size = splash_texture_.Size();
        const float max_width = viewport->Size.x * 0.38f;
        const float max_height = viewport->Size.y * 0.34f;
        const float scale = std::min(max_width / std::max(texture_size.x, 1), max_height / std::max(texture_size.y, 1));
        const ImVec2 size{
            texture_size.x * std::max(scale, 0.1f),
            texture_size.y * std::max(scale, 0.1f)
        };
        ImGui::SetCursorScreenPos({center.x - size.x * 0.5f, center.y - size.y * 0.78f});
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::Image(
            reinterpret_cast<void*>(static_cast<intptr_t>(splash_texture_.Id())),
            size,
            ImVec2(0, 1),
            ImVec2(1, 0));
        ImGui::PopStyleVar();
    }

    draw_list->AddText(
        nullptr,
        42.0f,
        {center.x - 155.0f, center.y + 70.0f},
        IM_COL32(240, 244, 255, static_cast<int>(alpha * 255.0f)),
        "NovaIso Engine");
    draw_list->AddText(
        nullptr,
        22.0f,
        {center.x - 118.0f, center.y + 116.0f},
        IM_COL32(150, 198, 255, static_cast<int>(alpha * 220.0f)),
        "Modern 2D platform runtime");

    ImGui::End();
}

void RuntimeApp::DrawMenuOverlay(const assets::MenuDefinition& menu, const char* id, bool pause_context) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin(id, nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y}, IM_COL32(8, 11, 18, pause_context ? 168 : 210));

    const ImVec2 panel_size{
        menu.panel_size.x > 8.0f ? menu.panel_size.x : 520.0f,
        menu.panel_size.y > 8.0f ? menu.panel_size.y : 0.0f
    };
    const ImVec2 center{
        viewport->Pos.x + viewport->Size.x * menu.position.x,
        viewport->Pos.y + viewport->Size.y * menu.position.y
    };

    ImGui::SetCursorScreenPos({center.x - panel_size.x * 0.5f, center.y - 180.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(menu.background.r, menu.background.g, menu.background.b, menu.background.a));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, menu.roundness);
    ImGui::BeginChild("MenuPanel", {panel_size.x, panel_size.y}, true, ImGuiWindowFlags_AlwaysAutoResize);
    draw_list->AddRectFilled(
        {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y},
        {ImGui::GetItemRectMin().x + 5.0f, ImGui::GetItemRectMax().y},
        IM_COL32(
            static_cast<int>(menu.accent.r * 255.0f),
            static_cast<int>(menu.accent.g * 255.0f),
            static_cast<int>(menu.accent.b * 255.0f),
            static_cast<int>(menu.accent.a * 255.0f))
    );

    ImGui::Dummy({8.0f, 4.0f});
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(menu.title_color.r, menu.title_color.g, menu.title_color.b, menu.title_color.a));
    ImGui::SetWindowFontScale(1.45f);
    ImGui::TextWrapped("%s", menu.title.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(menu.subtitle_color.r, menu.subtitle_color.g, menu.subtitle_color.b, menu.subtitle_color.a));
    ImGui::TextWrapped("%s", menu.subtitle.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    for (const auto& button : menu.buttons) {
        if (!button.enabled) {
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(button.color.r, button.color.g, button.color.b, button.color.a));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(button.hover_color.r, button.hover_color.g, button.hover_color.b, button.hover_color.a));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(button.hover_color.r, button.hover_color.g, button.hover_color.b, button.hover_color.a));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(button.text_color.r, button.text_color.g, button.text_color.b, button.text_color.a));
        if (ImGui::Button(button.label.c_str(), {button.size.x, button.size.y})) {
            HandleMenuAction(button);
        }
        ImGui::PopStyleColor(4);
        ImGui::Dummy({2.0f, menu.spacing});
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::End();
}

void RuntimeApp::DrawSettingsOverlay() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("RuntimeSettingsOverlay", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y}, IM_COL32(8, 11, 18, 222));

    const ImVec2 panel_size{620.0f, 0.0f};
    const ImVec2 center{viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f};
    ImGui::SetCursorScreenPos({center.x - panel_size.x * 0.5f, center.y - 240.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.13f, 0.20f, 0.96f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 26.0f);
    ImGui::BeginChild("RuntimeSettingsPanel", {panel_size.x, panel_size.y}, true, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextDisabled("SETTINGS");
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextWrapped("Display And Audio");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextWrapped("The game keeps the scene aspect safe. Ultrawide and 4K resolutions are displayed without stretching the world.");
    ImGui::Spacing();

    bool fullscreen = user_settings_.fullscreen;
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
        user_settings_.fullscreen = fullscreen;
        ApplyUserSettings(true);
        SaveUserSettings();
    }

    if (ImGui::BeginCombo("Resolution", CurrentResolutionText().c_str())) {
        for (int i = 0; i < static_cast<int>(available_resolutions_.size()); ++i) {
            const bool selected = i == active_resolution_index_;
            const std::string label = ResolutionText(available_resolutions_[static_cast<std::size_t>(i)]);
            if (ImGui::Selectable(label.c_str(), selected)) {
                active_resolution_index_ = i;
                user_settings_.resolution = available_resolutions_[static_cast<std::size_t>(i)];
                ApplyUserSettings(true);
                SaveUserSettings();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    float master = user_settings_.master_volume;
    float music = user_settings_.music_volume;
    float sound = user_settings_.sound_volume;
    if (ImGui::SliderFloat("Master Volume", &master, 0.0f, 1.0f, "%.2f")) {
        user_settings_.master_volume = master;
        ApplyUserSettings(false);
        SaveUserSettings();
    }
    if (ImGui::SliderFloat("Music Volume", &music, 0.0f, 1.0f, "%.2f")) {
        user_settings_.music_volume = music;
        ApplyUserSettings(false);
        SaveUserSettings();
    }
    if (ImGui::SliderFloat("Effects Volume", &sound, 0.0f, 1.0f, "%.2f")) {
        user_settings_.sound_volume = sound;
        ApplyUserSettings(false);
        SaveUserSettings();
    }

    ImGui::Spacing();
    if (ImGui::Button("Back", {180.0f, 48.0f})) {
        CloseSettingsMenu();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::End();
}

void RuntimeApp::DrawHudOverlay() {
    if (project_.ui_elements.empty()) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw_list = ImGui::GetForegroundDrawList(viewport);
    for (const auto& widget : project_.ui_elements) {
        if (!widget.enabled) {
            continue;
        }

        const ImVec2 min{
            viewport->Pos.x + viewport->Size.x * widget.anchor.x,
            viewport->Pos.y + viewport->Size.y * widget.anchor.y
        };
        const ImVec2 max{min.x + widget.size.x, min.y + widget.size.y};
        draw_list->AddRectFilled(min, max, IM_COL32(
            static_cast<int>(widget.background.r * 255.0f),
            static_cast<int>(widget.background.g * 255.0f),
            static_cast<int>(widget.background.b * 255.0f),
            static_cast<int>(widget.background.a * 255.0f)), 12.0f);
        draw_list->AddRectFilled({min.x, min.y}, {min.x + 4.0f, max.y}, IM_COL32(
            static_cast<int>(widget.accent.r * 255.0f),
            static_cast<int>(widget.accent.g * 255.0f),
            static_cast<int>(widget.accent.b * 255.0f),
            static_cast<int>(widget.accent.a * 255.0f)), 10.0f);

        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::SetNextWindowPos(min);
        ImGui::SetNextWindowSize({widget.size.x, widget.size.y});
        const std::string window_id = "HUD_" + widget.name;
        ImGui::Begin(window_id.c_str(), nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs);
        ImGui::SetCursorPos({widget.padding.x, widget.padding.y});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(widget.text_color.r, widget.text_color.g, widget.text_color.b, widget.text_color.a));
        ImGui::SetWindowFontScale(widget.scale);
        ImGui::TextWrapped("%s", ResolveUiText(widget.text).c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::End();
    }
}

void RuntimeApp::HandleMenuAction(const assets::MenuButtonDefinition& button) {
    HandleUiAction(button.action);
}

void RuntimeApp::HandleUiAction(const std::string& action) {
    if (action == "open_settings") {
        OpenSettingsMenu(show_pause_menu_ && !show_main_menu_);
        return;
    }
    if (action == "close_settings") {
        CloseSettingsMenu();
        return;
    }
    if (action == "toggle_fullscreen") {
        user_settings_.fullscreen = !user_settings_.fullscreen;
        ApplyUserSettings(true);
        SaveUserSettings();
        return;
    }
    if (action == "resolution_prev") {
        StepResolution(-1);
        return;
    }
    if (action == "resolution_next") {
        StepResolution(1);
        return;
    }
    if (action == "master_volume_down") {
        AdjustMasterVolume(-0.05f);
        return;
    }
    if (action == "master_volume_up") {
        AdjustMasterVolume(0.05f);
        return;
    }
    if (action == "music_volume_down") {
        AdjustMusicVolume(-0.05f);
        return;
    }
    if (action == "music_volume_up") {
        AdjustMusicVolume(0.05f);
        return;
    }
    if (action == "sound_volume_down") {
        AdjustSoundVolume(-0.05f);
        return;
    }
    if (action == "sound_volume_up") {
        AdjustSoundVolume(0.05f);
        return;
    }
    if (action == "start_game") {
        show_main_menu_ = false;
        show_pause_menu_ = false;
        show_settings_menu_ = false;
        scene_.ResetSimulation(true);
        return;
    }
    if (action == "resume_game") {
        show_pause_menu_ = false;
        show_settings_menu_ = false;
        return;
    }
    if (action == "restart_level") {
        show_main_menu_ = false;
        show_pause_menu_ = false;
        show_settings_menu_ = false;
        scene_.ResetSimulation(true);
        return;
    }
    if (action == "open_main_menu") {
        show_main_menu_ = true;
        show_pause_menu_ = false;
        show_settings_menu_ = false;
        if (!project_.main_menu.background_music.empty()) {
            asset_manager_.PlayMusic(project_.main_menu.background_music, project_.main_menu.music_loop);
        }
        return;
    }
    if (action == "toggle_camera_mode") {
        scene_.ToggleCameraMode();
        return;
    }
    if (action == "quit_game") {
        RequestQuit();
    }
}

ui::HtmlUiRuntime::DynamicContext RuntimeApp::BuildHtmlUiContext() const {
    ui::HtmlUiRuntime::DynamicContext context;
    context.values["engine.project"] = project_.name;
    context.values["engine.version"] = project_.version;
    context.values["engine.level"] = scene_.Level().name;
    context.values["engine.camera_mode"] = scene_.CameraModeName();
    context.values["engine.paused"] = show_pause_menu_ ? "true" : "false";
    context.values["engine.fullscreen"] = IsFullscreen() ? "true" : "false";
    context.values["engine.time"] = std::to_string(elapsed_time_);
    context.values["engine.window_width"] = std::to_string(WindowSize().x);
    context.values["engine.window_height"] = std::to_string(WindowSize().y);
    context.values["engine.resolution"] = CurrentResolutionText();
    context.values["engine.master_volume"] = std::to_string(VolumePercent(user_settings_.master_volume));
    context.values["engine.music_volume"] = std::to_string(VolumePercent(user_settings_.music_volume));
    context.values["engine.sound_volume"] = std::to_string(VolumePercent(user_settings_.sound_volume));
    context.values["ui.current_screen"] = show_settings_menu_ ? "settings-menu" : (show_main_menu_ ? "main-menu" : (show_pause_menu_ ? "pause-menu" : "hud"));
    return context;
}

std::string RuntimeApp::ResolveUiText(const std::string& text) const {
    std::string resolved = text;
    auto replace_all = [&](const std::string& needle, const std::string& value) {
        std::size_t position = 0;
        while ((position = resolved.find(needle, position)) != std::string::npos) {
            resolved.replace(position, needle.size(), value);
            position += value.size();
        }
    };

    replace_all("{project}", project_.name);
    replace_all("{version}", project_.version);
    replace_all("{level}", scene_.Level().name);
    replace_all("{camera_mode}", scene_.CameraModeName());
    replace_all("{last_message}", scene_.Messages().empty() ? std::string{} : scene_.Messages().back());
    replace_all("{setting_resolution}", CurrentResolutionText());
    replace_all("{setting_fullscreen}", user_settings_.fullscreen ? "On" : "Off");
    replace_all("{setting_master_volume}", std::to_string(VolumePercent(user_settings_.master_volume)) + "%");
    replace_all("{setting_music_volume}", std::to_string(VolumePercent(user_settings_.music_volume)) + "%");
    replace_all("{setting_sound_volume}", std::to_string(VolumePercent(user_settings_.sound_volume)) + "%");

    std::size_t cursor = 0;
    while ((cursor = resolved.find("{entity:", cursor)) != std::string::npos) {
        const std::size_t end = resolved.find('}', cursor);
        if (end == std::string::npos) {
            break;
        }
        const std::string token = resolved.substr(cursor + 8, end - (cursor + 8));
        const std::size_t separator = token.find(':');
        std::string replacement;
        if (separator != std::string::npos) {
            const std::string entity_id = token.substr(0, separator);
            const std::string property = token.substr(separator + 1);
            if (const auto* entity = scene_.FindEntity(entity_id); entity != nullptr && entity->properties.contains(property)) {
                const auto& value = entity->properties[property];
                if (value.is_string()) {
                    replacement = value.get<std::string>();
                } else if (value.is_boolean()) {
                    replacement = value.get<bool>() ? "true" : "false";
                } else if (value.is_number_integer()) {
                    replacement = std::to_string(value.get<int>());
                } else if (value.is_number_float()) {
                    std::ostringstream stream;
                    stream << value.get<float>();
                    replacement = stream.str();
                } else {
                    replacement = value.dump();
                }
            }
        }
        resolved.replace(cursor, end - cursor + 1, replacement);
        cursor += replacement.size();
    }

    return resolved;
}

std::string RuntimeApp::CurrentResolutionText() const {
    return ResolutionText(user_settings_.resolution);
}

}  // namespace novaiso::runtime
