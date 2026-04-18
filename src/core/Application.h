#pragma once

#include "core/Input.h"

#include <SDL.h>
#include <glm/vec2.hpp>

#include <filesystem>
#include <string>

namespace novaiso::core {

struct WindowConfig {
    std::string title = "NovaIso Engine";
    int width = 1600;
    int height = 900;
    bool enable_gui = false;
    bool maximized = false;
    bool vsync = true;
};

class Application {
public:
    explicit Application(WindowConfig config);
    virtual ~Application();

    int Run();

    [[nodiscard]] SDL_Window* Window() const;
    [[nodiscard]] glm::ivec2 WindowSize() const;
    [[nodiscard]] Input& GetInput();
    [[nodiscard]] const Input& GetInput() const;
    [[nodiscard]] const std::filesystem::path& ExecutableDirectory() const;
    void RequestQuit();
    void SetWindowTitle(const std::string& title);
    void SetWindowIcon(const std::filesystem::path& path);
    void SetFullscreen(bool enabled);
    void ToggleFullscreen();
    [[nodiscard]] bool IsFullscreen() const;

protected:
    virtual bool OnInit() = 0;
    virtual void OnShutdown();
    virtual void OnEvent(const SDL_Event& event);
    virtual void OnUpdate(float delta_time) = 0;
    virtual void OnRender() = 0;
    virtual void OnGui();
    virtual void OnDropFile(const std::filesystem::path& path);

    [[nodiscard]] bool GuiEnabled() const;
    [[nodiscard]] float DeltaTime() const;

private:
    bool InitializeSDL();
    bool InitializeImGui();
    void ShutdownImGui();

    WindowConfig config_;
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;
    Input input_;
    bool running_ = true;
    float delta_time_ = 0.0f;
    std::filesystem::path executable_directory_;
};

}  // namespace novaiso::core
