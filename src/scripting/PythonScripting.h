#pragma once

#include "assets/Project.h"

#include <pybind11/embed.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace novaiso::entities {
class Scene;
struct Entity;
}  // namespace novaiso::entities

namespace novaiso::scripting {

class ScriptEntityHandle {
public:
    explicit ScriptEntityHandle(entities::Entity* entity = nullptr);

    [[nodiscard]] bool Valid() const;
    [[nodiscard]] std::string Id() const;
    [[nodiscard]] std::pair<float, float> Position() const;
    [[nodiscard]] std::pair<float, float> Velocity() const;
    [[nodiscard]] float Rotation() const;
    [[nodiscard]] bool Grounded() const;

    void SetPosition(float x, float y) const;
    void Move(float dx, float dy) const;
    void SetVelocity(float x, float y) const;
    void SetRotation(float angle) const;
    void SetTint(float r, float g, float b, float a) const;
    void SetActive(bool value) const;
    void SetVisible(bool value) const;
    float GetFloat(const std::string& key, float fallback) const;
    bool GetBool(const std::string& key, bool fallback) const;
    std::string GetString(const std::string& key, const std::string& fallback) const;
    void SetFloat(const std::string& key, float value) const;
    void SetBool(const std::string& key, bool value) const;
    void SetString(const std::string& key, const std::string& value) const;

private:
    entities::Entity* entity_ = nullptr;
};

class ScriptSceneHandle {
public:
    explicit ScriptSceneHandle(entities::Scene* scene = nullptr);

    [[nodiscard]] bool Valid() const;
    [[nodiscard]] ScriptEntityHandle FindEntity(const std::string& id) const;
    [[nodiscard]] bool ActionDown(const std::string& action) const;
    [[nodiscard]] std::string CameraMode() const;

    void ToggleCameraMode() const;
    void SetCameraMode(const std::string& mode) const;
    [[nodiscard]] float CameraZoom() const;
    void SetCameraZoom(float zoom) const;
    void SetCameraZoomSmooth(float zoom, float speed) const;
    void SetCameraFollow(bool enabled) const;
    bool ActivateVirtualCamera(const std::string& id) const;
    void ReleaseVirtualCamera() const;
    bool PlaySceneAnimation(const std::string& id, bool restart) const;
    bool StopSceneAnimation(const std::string& id, bool restore_state) const;
    void Log(const std::string& message) const;
    void PlaySound(const std::string& path) const;
    void PlayMusic(const std::string& path, bool loop) const;
    void StopAllAudio() const;
    bool LightEnabled(const std::string& name) const;
    void SetLightEnabled(const std::string& name, bool enabled) const;

private:
    entities::Scene* scene_ = nullptr;
};

class PythonScripting {
public:
    bool Initialize(const std::filesystem::path& project_root);
    void Shutdown();
    void SetProjectRoot(const std::filesystem::path& project_root);
    void ReloadChanged();

    void CallEntitySpawn(entities::Entity& entity, entities::Scene& scene);
    void CallEntityUpdate(entities::Entity& entity, entities::Scene& scene, float delta_time);
    void CallEntityTrigger(entities::Entity& entity, entities::Scene& scene, const std::string& trigger_name);
    bool EvaluateCondition(const assets::TriggerCondition& condition, entities::Scene& scene, const std::string& trigger_name);
    void ExecuteAction(const assets::TriggerAction& action, entities::Scene& scene, const std::string& trigger_name);

private:
    static std::string ModuleNameFromPath(const std::string& script_path);
    bool EvaluateBuiltinCondition(const assets::TriggerCondition& condition, entities::Scene& scene, const std::string& trigger_name);
    void ExecuteBuiltinAction(const assets::TriggerAction& action, entities::Scene& scene, const std::string& trigger_name);
    pybind11::object ImportModule(const std::string& module_name);
    void RefreshSysPath();
    void PrintPythonError(const pybind11::error_already_set& error) const;

    std::unique_ptr<pybind11::scoped_interpreter> interpreter_;
    std::filesystem::path project_root_;
    std::unordered_map<std::string, std::filesystem::file_time_type> tracked_scripts_;
};

}  // namespace novaiso::scripting
