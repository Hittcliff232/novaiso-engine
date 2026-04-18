#include "core/Application.h"

#include "core/FileIO.h"

#include <stb_image.h>

#include <glad/gl.h>
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>
#include <vector>

namespace novaiso::core {

namespace {

std::filesystem::path ResolveExecutableDirectory() {
    if (char* base_path = SDL_GetBasePath(); base_path != nullptr) {
        const std::filesystem::path resolved(base_path);
        SDL_free(base_path);
        return resolved;
    }
    return std::filesystem::current_path();
}

std::vector<std::filesystem::path> CandidateUIFontPaths(const std::filesystem::path& executable_directory) {
    std::vector<std::filesystem::path> candidates{
        executable_directory / "fonts/NotoSans-Regular.ttf",
        executable_directory / "assets/fonts/NotoSans-Regular.ttf",
        executable_directory.parent_path() / "fonts/NotoSans-Regular.ttf",
        executable_directory.parent_path() / "assets/fonts/NotoSans-Regular.ttf"
    };

#if defined(_WIN32)
    std::filesystem::path windows_fonts = "C:/Windows/Fonts";
    if (const char* windir = std::getenv("WINDIR"); windir != nullptr && *windir != '\0') {
        windows_fonts = std::filesystem::path(windir) / "Fonts";
    }
    for (const char* name : {"segoeui.ttf", "arial.ttf", "tahoma.ttf", "verdana.ttf"}) {
        candidates.push_back(windows_fonts / name);
    }
#else
    for (const auto& path : {
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
             "/usr/share/fonts/noto/NotoSans-Regular.ttf"
         }) {
        candidates.emplace_back(path);
    }
#endif

    return candidates;
}

bool ConfigureImGuiFonts(const std::filesystem::path& executable_directory) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = false;

    for (const auto& path : CandidateUIFontPaths(executable_directory)) {
        if (!std::filesystem::exists(path)) {
            continue;
        }
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 18.0f, &config, io.Fonts->GetGlyphRangesCyrillic());
            font != nullptr) {
            io.FontDefault = font;
            return true;
        }
    }

    io.Fonts->AddFontDefault();
    return false;
}

}  // namespace

Application::Application(WindowConfig config) : config_(std::move(config)), executable_directory_(ResolveExecutableDirectory()) {}

Application::~Application() {
    ShutdownImGui();
    if (gl_context_ != nullptr) {
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

int Application::Run() {
    if (!InitializeSDL()) {
        return 1;
    }
    if (GuiEnabled() && !InitializeImGui()) {
        return 1;
    }
    if (!OnInit()) {
        return 1;
    }

    using clock = std::chrono::high_resolution_clock;
    auto previous = clock::now();

    while (running_) {
        const auto now = clock::now();
        delta_time_ = std::clamp(std::chrono::duration<float>(now - previous).count(), 0.0f, 0.05f);
        previous = now;

        input_.BeginFrame();

        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if (GuiEnabled()) {
                ImGui_ImplSDL2_ProcessEvent(&event);
            }

            input_.ProcessEvent(event);

            if (event.type == SDL_QUIT) {
                running_ = false;
            } else if (event.type == SDL_DROPFILE) {
                OnDropFile(event.drop.file != nullptr ? std::filesystem::path(event.drop.file) : std::filesystem::path{});
                SDL_free(event.drop.file);
            } else {
                OnEvent(event);
            }
        }

        OnUpdate(delta_time_);

        if (GuiEnabled()) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        OnRender();

        if (GuiEnabled()) {
            OnGui();
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
                SDL_Window* backup_window = SDL_GL_GetCurrentWindow();
                SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                SDL_GL_MakeCurrent(backup_window, backup_context);
            }
        }

        SDL_GL_SwapWindow(window_);
    }

    OnShutdown();
    return 0;
}

SDL_Window* Application::Window() const {
    return window_;
}

glm::ivec2 Application::WindowSize() const {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    return {width, height};
}

Input& Application::GetInput() {
    return input_;
}

const Input& Application::GetInput() const {
    return input_;
}

const std::filesystem::path& Application::ExecutableDirectory() const {
    return executable_directory_;
}

void Application::RequestQuit() {
    running_ = false;
}

void Application::SetWindowTitle(const std::string& title) {
    if (window_ != nullptr) {
        SDL_SetWindowTitle(window_, title.c_str());
    }
}

void Application::SetWindowIcon(const std::filesystem::path& path) {
    if (window_ == nullptr || path.empty() || !core::FileIO::Exists(path)) {
        return;
    }

    const auto bytes = core::FileIO::ReadBinary(path);
    if (bytes.empty()) {
        return;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        return;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels,
        width,
        height,
        32,
        width * 4,
        SDL_PIXELFORMAT_RGBA32
    );
    if (surface != nullptr) {
        SDL_SetWindowIcon(window_, surface);
        SDL_FreeSurface(surface);
    }
    stbi_image_free(pixels);
}

void Application::SetFullscreen(bool enabled) {
    if (window_ == nullptr) {
        return;
    }
    SDL_SetWindowFullscreen(window_, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void Application::ToggleFullscreen() {
    SetFullscreen(!IsFullscreen());
}

bool Application::IsFullscreen() const {
    if (window_ == nullptr) {
        return false;
    }
    const Uint32 flags = SDL_GetWindowFlags(window_);
    return (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 || (flags & SDL_WINDOW_FULLSCREEN) != 0;
}

void Application::OnShutdown() {}

void Application::OnEvent(const SDL_Event&) {}

void Application::OnGui() {}

void Application::OnDropFile(const std::filesystem::path&) {}

bool Application::GuiEnabled() const {
    return config_.enable_gui;
}

float Application::DeltaTime() const {
    return delta_time_;
}

bool Application::InitializeSDL() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (config_.maximized) {
        flags |= SDL_WINDOW_MAXIMIZED;
    }

    window_ = SDL_CreateWindow(
        config_.title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        config_.width,
        config_.height,
        flags
    );
    if (window_ == nullptr) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
        return false;
    }

    const std::filesystem::path default_icon = executable_directory_ / "logo.png";
    if (std::filesystem::exists(default_icon)) {
        SetWindowIcon(default_icon);
    }

    gl_context_ = SDL_GL_CreateContext(window_);
    if (gl_context_ == nullptr) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << '\n';
        return false;
    }

    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)) == 0) {
        std::cerr << "Failed to load OpenGL functions.\n";
        return false;
    }

    SDL_GL_SetSwapInterval(config_.vsync ? 1 : 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    return true;
}

bool Application::InitializeImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (!ConfigureImGuiFonts(executable_directory_)) {
        std::cerr << "ImGui font fallback loaded without Cyrillic glyphs.\n";
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = {11.0f, 11.0f};
    style.FramePadding = {9.0f, 6.0f};
    style.ItemSpacing = {8.0f, 7.0f};
    style.ItemInnerSpacing = {6.0f, 5.0f};
    style.WindowRounding = 10.0f;
    style.ChildRounding = 9.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 9.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.095f, 0.11f, 0.15f, 0.90f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.14f, 0.19f, 0.76f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.12f, 0.17f, 0.96f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.48f, 0.58f, 0.70f, 0.34f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.24f, 0.31f, 0.86f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.31f, 0.40f, 0.92f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.29f, 0.36f, 0.46f, 0.96f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.13f, 0.18f, 0.92f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.19f, 0.27f, 0.96f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.12f, 0.17f, 0.95f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.24f, 0.46f, 0.78f, 0.88f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.31f, 0.57f, 0.92f, 0.94f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.36f, 0.63f, 0.98f, 0.98f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.28f, 0.38f, 0.88f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.38f, 0.50f, 0.94f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.31f, 0.43f, 0.58f, 0.98f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.19f, 0.27f, 0.90f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.44f, 0.70f, 0.92f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.22f, 0.33f, 0.53f, 0.96f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.12f, 0.15f, 0.21f, 0.90f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.17f, 0.24f, 0.36f, 0.94f);
    style.Colors[ImGuiCol_DockingPreview] = ImVec4(0.30f, 0.66f, 0.98f, 0.44f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.37f, 0.45f, 0.56f, 0.74f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.34f, 0.58f, 0.94f, 0.28f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.38f, 0.66f, 0.99f, 0.78f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.41f, 0.72f, 1.00f, 0.96f);
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        style.WindowRounding = 8.0f;
        style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    }

    if (!ImGui_ImplSDL2_InitForOpenGL(window_, gl_context_)) {
        std::cerr << "ImGui SDL backend init failed.\n";
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 460")) {
        std::cerr << "ImGui OpenGL backend init failed.\n";
        return false;
    }
    return true;
}

void Application::ShutdownImGui() {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }
}

}  // namespace novaiso::core
