#include "scripting/PythonScripting.h"

#include "entities/Entity.h"
#include "entities/Scene.h"
#include "PythonRuntimeConfig.h"
#include "core/FileIO.h"

#include <pybind11/stl.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace py = pybind11;

namespace novaiso::scripting {

namespace {

using json = nlohmann::json;

void ThrowIfPyConfigFailed(PyStatus status, PyConfig* config, const char* context) {
    if (PyStatus_Exception(status) == 0) {
        return;
    }

    const std::string message = PyStatus_IsError(status) != 0 && status.err_msg != nullptr
        ? std::string(status.err_msg)
        : std::string("CPython initialization failed.");
    PyConfig_Clear(config);
    throw std::runtime_error(std::string(context) + ": " + message);
}

std::unique_ptr<py::scoped_interpreter> CreateInterpreter() {
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;
    config.install_signal_handlers = 0;
    config.pathconfig_warnings = 0;
    config.site_import = 1;

    ThrowIfPyConfigFailed(PyConfig_SetString(&config, &config.home, NOVAISO_PYTHON_HOME), &config, "Failed to set Python home");
    ThrowIfPyConfigFailed(PyConfig_SetString(&config, &config.program_name, NOVAISO_PYTHON_EXECUTABLE), &config, "Failed to set Python program name");
    ThrowIfPyConfigFailed(PyConfig_SetString(&config, &config.executable, NOVAISO_PYTHON_EXECUTABLE), &config, "Failed to set Python executable");
    ThrowIfPyConfigFailed(PyConfig_SetString(&config, &config.base_executable, NOVAISO_PYTHON_EXECUTABLE), &config, "Failed to set Python base executable");

    config.module_search_paths_set = 1;
    ThrowIfPyConfigFailed(PyWideStringList_Append(&config.module_search_paths, NOVAISO_PYTHON_HOME), &config, "Failed to append Python home");
    ThrowIfPyConfigFailed(PyWideStringList_Append(&config.module_search_paths, NOVAISO_PYTHON_STDLIB), &config, "Failed to append Python stdlib");
    ThrowIfPyConfigFailed(PyWideStringList_Append(&config.module_search_paths, NOVAISO_PYTHON_DLLS), &config, "Failed to append Python DLLs");

    return std::make_unique<py::scoped_interpreter>(&config, 0, nullptr, true);
}

json ParseArgsJson(const std::string& text) {
    if (text.empty()) {
        return json::object();
    }
    const json parsed = json::parse(text, nullptr, false);
    return parsed.is_discarded() ? json::object() : parsed;
}

}  // namespace

ScriptEntityHandle::ScriptEntityHandle(entities::Entity* entity) : entity_(entity) {}

bool ScriptEntityHandle::Valid() const {
    return entity_ != nullptr;
}

std::string ScriptEntityHandle::Id() const {
    return entity_ != nullptr ? entity_->id : "";
}

std::pair<float, float> ScriptEntityHandle::Position() const {
    return entity_ != nullptr ? std::pair<float, float>{entity_->position.x, entity_->position.y} : std::pair<float, float>{0.0f, 0.0f};
}

std::pair<float, float> ScriptEntityHandle::Velocity() const {
    return entity_ != nullptr ? std::pair<float, float>{entity_->velocity.x, entity_->velocity.y} : std::pair<float, float>{0.0f, 0.0f};
}

float ScriptEntityHandle::Rotation() const {
    return entity_ != nullptr ? entity_->rotation : 0.0f;
}

bool ScriptEntityHandle::Grounded() const {
    return entity_ != nullptr && entity_->grounded;
}

void ScriptEntityHandle::SetPosition(float x, float y) const {
    if (entity_ != nullptr) {
        entity_->position = {x, y};
    }
}

void ScriptEntityHandle::Move(float dx, float dy) const {
    if (entity_ != nullptr) {
        entity_->position += glm::vec2(dx, dy);
    }
}

void ScriptEntityHandle::SetVelocity(float x, float y) const {
    if (entity_ != nullptr) {
        entity_->velocity = {x, y};
        entity_->facing_right = x >= 0.0f;
    }
}

void ScriptEntityHandle::SetRotation(float angle) const {
    if (entity_ != nullptr) {
        entity_->rotation = angle;
    }
}

void ScriptEntityHandle::SetTint(float r, float g, float b, float a) const {
    if (entity_ != nullptr) {
        entity_->tint = {r, g, b, a};
    }
}

void ScriptEntityHandle::SetActive(bool value) const {
    if (entity_ != nullptr) {
        entity_->active = value;
    }
}

void ScriptEntityHandle::SetVisible(bool value) const {
    if (entity_ != nullptr) {
        entity_->visible = value;
    }
}

float ScriptEntityHandle::GetFloat(const std::string& key, float fallback) const {
    if (entity_ != nullptr && entity_->properties.contains(key) && entity_->properties[key].is_number()) {
        return entity_->properties[key].get<float>();
    }
    return fallback;
}

bool ScriptEntityHandle::GetBool(const std::string& key, bool fallback) const {
    if (entity_ != nullptr && entity_->properties.contains(key) && entity_->properties[key].is_boolean()) {
        return entity_->properties[key].get<bool>();
    }
    return fallback;
}

std::string ScriptEntityHandle::GetString(const std::string& key, const std::string& fallback) const {
    if (entity_ != nullptr && entity_->properties.contains(key) && entity_->properties[key].is_string()) {
        return entity_->properties[key].get<std::string>();
    }
    return fallback;
}

void ScriptEntityHandle::SetFloat(const std::string& key, float value) const {
    if (entity_ != nullptr) {
        entity_->properties[key] = value;
    }
}

void ScriptEntityHandle::SetBool(const std::string& key, bool value) const {
    if (entity_ != nullptr) {
        entity_->properties[key] = value;
    }
}

void ScriptEntityHandle::SetString(const std::string& key, const std::string& value) const {
    if (entity_ != nullptr) {
        entity_->properties[key] = value;
    }
}

ScriptSceneHandle::ScriptSceneHandle(entities::Scene* scene) : scene_(scene) {}

bool ScriptSceneHandle::Valid() const {
    return scene_ != nullptr;
}

ScriptEntityHandle ScriptSceneHandle::FindEntity(const std::string& id) const {
    return scene_ != nullptr ? ScriptEntityHandle(scene_->FindEntity(id)) : ScriptEntityHandle{};
}

bool ScriptSceneHandle::ActionDown(const std::string& action) const {
    return scene_ != nullptr && scene_->ActionDown(action);
}

std::string ScriptSceneHandle::CameraMode() const {
    return scene_ != nullptr ? scene_->CameraModeName() : "side";
}

void ScriptSceneHandle::ToggleCameraMode() const {
    if (scene_ != nullptr) {
        scene_->ToggleCameraMode();
    }
}

void ScriptSceneHandle::SetCameraMode(const std::string& mode) const {
    if (scene_ == nullptr) {
        return;
    }
    scene_->SetCameraMode(mode == "isometric" ? camera::Mode::Isometric : camera::Mode::Side);
}

float ScriptSceneHandle::CameraZoom() const {
    return scene_ != nullptr ? scene_->CameraZoom() : 1.0f;
}

void ScriptSceneHandle::SetCameraZoom(float zoom) const {
    if (scene_ != nullptr) {
        scene_->SetCameraZoom(zoom);
    }
}

void ScriptSceneHandle::SetCameraZoomSmooth(float zoom, float speed) const {
    if (scene_ != nullptr) {
        scene_->SetCameraZoomSmooth(zoom, speed);
    }
}

void ScriptSceneHandle::SetCameraFollow(bool enabled) const {
    if (scene_ != nullptr) {
        scene_->SetCameraFollowEnabled(enabled);
    }
}

bool ScriptSceneHandle::ActivateVirtualCamera(const std::string& id) const {
    return scene_ != nullptr && scene_->ActivateVirtualCamera(id);
}

void ScriptSceneHandle::ReleaseVirtualCamera() const {
    if (scene_ != nullptr) {
        scene_->ReleaseVirtualCamera();
    }
}

bool ScriptSceneHandle::PlaySceneAnimation(const std::string& id, bool restart) const {
    return scene_ != nullptr && scene_->PlaySceneAnimation(id, restart);
}

bool ScriptSceneHandle::StopSceneAnimation(const std::string& id, bool restore_state) const {
    return scene_ != nullptr && scene_->StopSceneAnimation(id, restore_state);
}

void ScriptSceneHandle::Log(const std::string& message) const {
    if (scene_ != nullptr) {
        scene_->Log(message);
    }
}

void ScriptSceneHandle::PlaySound(const std::string& path) const {
    if (scene_ != nullptr) {
        scene_->PlaySound(path);
    }
}

void ScriptSceneHandle::PlayMusic(const std::string& path, bool loop) const {
    if (scene_ != nullptr) {
        scene_->PlayMusic(path, loop);
    }
}

void ScriptSceneHandle::StopAllAudio() const {
    if (scene_ != nullptr) {
        scene_->StopAllAudio();
    }
}

bool ScriptSceneHandle::LightEnabled(const std::string& name) const {
    const auto* light = scene_ != nullptr ? scene_->FindLight(name) : nullptr;
    return light != nullptr && light->enabled;
}

void ScriptSceneHandle::SetLightEnabled(const std::string& name, bool enabled) const {
    if (scene_ == nullptr) {
        return;
    }
    if (auto* light = scene_->FindLight(name); light != nullptr) {
        light->enabled = enabled;
    }
}

PYBIND11_EMBEDDED_MODULE(novaiso, m) {
    py::class_<ScriptEntityHandle>(m, "Entity")
        .def(py::init<>())
        .def("valid", &ScriptEntityHandle::Valid)
        .def("id", &ScriptEntityHandle::Id)
        .def("position", &ScriptEntityHandle::Position)
        .def("velocity", &ScriptEntityHandle::Velocity)
        .def("rotation", &ScriptEntityHandle::Rotation)
        .def("grounded", &ScriptEntityHandle::Grounded)
        .def("set_position", &ScriptEntityHandle::SetPosition)
        .def("move", &ScriptEntityHandle::Move)
        .def("set_velocity", &ScriptEntityHandle::SetVelocity)
        .def("set_rotation", &ScriptEntityHandle::SetRotation)
        .def("set_tint", &ScriptEntityHandle::SetTint)
        .def("set_active", &ScriptEntityHandle::SetActive)
        .def("set_visible", &ScriptEntityHandle::SetVisible)
        .def("get_float", &ScriptEntityHandle::GetFloat)
        .def("get_bool", &ScriptEntityHandle::GetBool)
        .def("get_string", &ScriptEntityHandle::GetString)
        .def("set_float", &ScriptEntityHandle::SetFloat)
        .def("set_bool", &ScriptEntityHandle::SetBool)
        .def("set_string", &ScriptEntityHandle::SetString);

    py::class_<ScriptSceneHandle>(m, "Scene")
        .def(py::init<>())
        .def("valid", &ScriptSceneHandle::Valid)
        .def("find_entity", &ScriptSceneHandle::FindEntity)
        .def("action_down", &ScriptSceneHandle::ActionDown)
        .def("camera_mode", &ScriptSceneHandle::CameraMode)
        .def("toggle_camera_mode", &ScriptSceneHandle::ToggleCameraMode)
        .def("set_camera_mode", &ScriptSceneHandle::SetCameraMode)
        .def("camera_zoom", &ScriptSceneHandle::CameraZoom)
        .def("set_camera_zoom", &ScriptSceneHandle::SetCameraZoom)
        .def("set_camera_zoom_smooth", &ScriptSceneHandle::SetCameraZoomSmooth)
        .def("set_camera_follow", &ScriptSceneHandle::SetCameraFollow)
        .def("activate_virtual_camera", &ScriptSceneHandle::ActivateVirtualCamera)
        .def("release_virtual_camera", &ScriptSceneHandle::ReleaseVirtualCamera)
        .def("play_scene_animation", &ScriptSceneHandle::PlaySceneAnimation)
        .def("stop_scene_animation", &ScriptSceneHandle::StopSceneAnimation)
        .def("log", &ScriptSceneHandle::Log)
        .def("play_sound", &ScriptSceneHandle::PlaySound)
        .def("play_music", &ScriptSceneHandle::PlayMusic)
        .def("stop_all_audio", &ScriptSceneHandle::StopAllAudio)
        .def("light_enabled", &ScriptSceneHandle::LightEnabled)
        .def("set_light_enabled", &ScriptSceneHandle::SetLightEnabled);

    m.def("_read_text", [](const std::string& path) {
        return core::FileIO::ReadText(path);
    });
    m.def("_file_exists", [](const std::string& path) {
        return core::FileIO::Exists(path);
    });
}

bool PythonScripting::Initialize(const std::filesystem::path& project_root) {
    if (!interpreter_) {
        interpreter_ = CreateInterpreter();
        py::gil_scoped_acquire acquire;
        py::module_::import("novaiso");
    }
    SetProjectRoot(project_root);
    return true;
}

void PythonScripting::Shutdown() {
    tracked_scripts_.clear();
    interpreter_.reset();
}

void PythonScripting::SetProjectRoot(const std::filesystem::path& project_root) {
    project_root_ = project_root;
    tracked_scripts_.clear();
    RefreshSysPath();
}

void PythonScripting::ReloadChanged() {
    if (!interpreter_) {
        return;
    }

    const auto scripts_root = project_root_ / "scripts";
    if (!std::filesystem::exists(scripts_root)) {
        return;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(scripts_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".py") {
            continue;
        }

        const auto relative = std::filesystem::relative(entry.path(), scripts_root);
        const auto module_name = ModuleNameFromPath(relative.generic_string());
        const auto timestamp = std::filesystem::last_write_time(entry.path());

        const auto existing = tracked_scripts_.find(module_name);
        if (existing == tracked_scripts_.end() || existing->second != timestamp) {
            try {
                py::gil_scoped_acquire acquire;
                py::module_ importlib = py::module_::import("importlib");
                py::object module = py::module_::import(module_name.c_str());
                importlib.attr("reload")(module);
            } catch (const py::error_already_set& error) {
                PrintPythonError(error);
            }
            tracked_scripts_[module_name] = timestamp;
        }
    }
}

void PythonScripting::CallEntitySpawn(entities::Entity& entity, entities::Scene& scene) {
    if (entity.script.empty()) {
        return;
    }
    try {
        py::gil_scoped_acquire acquire;
        py::object module = ImportModule(ModuleNameFromPath(entity.script));
        if (py::hasattr(module, "on_spawn")) {
            module.attr("on_spawn")(ScriptEntityHandle(&entity), ScriptSceneHandle(&scene));
        }
    } catch (const py::error_already_set& error) {
        PrintPythonError(error);
    }
}

void PythonScripting::CallEntityUpdate(entities::Entity& entity, entities::Scene& scene, float delta_time) {
    if (entity.script.empty()) {
        return;
    }
    try {
        py::gil_scoped_acquire acquire;
        py::object module = ImportModule(ModuleNameFromPath(entity.script));
        if (py::hasattr(module, "on_update")) {
            module.attr("on_update")(ScriptEntityHandle(&entity), ScriptSceneHandle(&scene), delta_time);
        }
    } catch (const py::error_already_set& error) {
        PrintPythonError(error);
    }
}

void PythonScripting::CallEntityTrigger(entities::Entity& entity, entities::Scene& scene, const std::string& trigger_name) {
    if (entity.script.empty()) {
        return;
    }
    const std::string hook = entity.on_trigger.empty() ? "on_trigger" : entity.on_trigger;
    try {
        py::gil_scoped_acquire acquire;
        py::object module = ImportModule(ModuleNameFromPath(entity.script));
        if (py::hasattr(module, hook.c_str())) {
            module.attr(hook.c_str())(ScriptEntityHandle(&entity), ScriptSceneHandle(&scene), trigger_name);
        }
    } catch (const py::error_already_set& error) {
        PrintPythonError(error);
    }
}

bool PythonScripting::EvaluateCondition(const assets::TriggerCondition& condition, entities::Scene& scene, const std::string& trigger_name) {
    if (condition.script.empty() || condition.script == "builtin") {
        return EvaluateBuiltinCondition(condition, scene, trigger_name);
    }
    if (condition.script.empty() || condition.function.empty()) {
        return true;
    }
    try {
        py::gil_scoped_acquire acquire;
        py::object module = ImportModule(ModuleNameFromPath(condition.script));
        if (py::hasattr(module, condition.function.c_str())) {
            return module.attr(condition.function.c_str())(ScriptSceneHandle(&scene), trigger_name, condition.args_json).cast<bool>();
        }
    } catch (const py::error_already_set& error) {
        PrintPythonError(error);
    }
    return false;
}

void PythonScripting::ExecuteAction(const assets::TriggerAction& action, entities::Scene& scene, const std::string& trigger_name) {
    json args = ParseArgsJson(action.args_json);
    const float delay = std::max(args.value("delay", 0.0f), 0.0f);
    if (delay > 0.0f) {
        if (scene.DebugTraceEnabled()) {
            scene.Trace("Delay action " + action.function + " from " + trigger_name + " by " + std::to_string(delay) + "s");
        }
        assets::TriggerAction delayed = action;
        args.erase("delay");
        delayed.args_json = args.dump();
        scene.ScheduleTriggerAction(delayed, trigger_name, delay);
        return;
    }

    if (action.script.empty() || action.script == "builtin") {
        if (scene.DebugTraceEnabled()) {
            scene.Trace("Execute builtin action " + action.function + " from " + trigger_name + "");
        }
        ExecuteBuiltinAction(action, scene, trigger_name);
        return;
    }
    if (action.script.empty() || action.function.empty()) {
        return;
    }
    try {
        if (scene.DebugTraceEnabled()) {
            scene.Trace("Execute script action " + action.function + " from " + trigger_name + " script=" + action.script + "");
        }
        py::gil_scoped_acquire acquire;
        py::object module = ImportModule(ModuleNameFromPath(action.script));
        if (py::hasattr(module, action.function.c_str())) {
            module.attr(action.function.c_str())(ScriptSceneHandle(&scene), trigger_name, action.args_json);
        }
    } catch (const py::error_already_set& error) {
        PrintPythonError(error);
    }
}

std::string PythonScripting::ModuleNameFromPath(const std::string& script_path) {
    std::filesystem::path path(script_path);
    if (path.extension() == ".py") {
        path.replace_extension();
    }

    std::ostringstream builder;
    bool first = true;
    for (const auto& part : path) {
        if (part == "scripts") {
            continue;
        }
        if (!first) {
            builder << '.';
        }
        builder << part.string();
        first = false;
    }
    return builder.str();
}

bool PythonScripting::EvaluateBuiltinCondition(const assets::TriggerCondition& condition, entities::Scene& scene, const std::string& trigger_name) {
    const json args = ParseArgsJson(condition.args_json);
    if (condition.function.empty() || condition.function == "always_true") {
        return true;
    }
    if (condition.function == "require_side") {
        return scene.CameraModeName() == "side";
    }
    if (condition.function == "require_iso") {
        return scene.CameraModeName() == "isometric";
    }
    if (condition.function == "light_enabled") {
        const auto* light = scene.FindLight(args.value("light", ""));
        return light != nullptr && light->enabled == args.value("enabled", true);
    }
    if (condition.function == "entity_active") {
        const auto* entity = scene.FindEntity(args.value("entity", ""));
        return entity != nullptr && entity->active == args.value("active", true);
    }
    if (condition.function == "entity_visible") {
        const auto* entity = scene.FindEntity(args.value("entity", ""));
        return entity != nullptr && entity->visible == args.value("visible", true);
    }
    if (condition.function == "audio_source_enabled") {
        const auto* source = scene.FindAudioSource(args.value("audio_source", ""));
        return source != nullptr && source->enabled == args.value("enabled", true);
    }
    if (condition.function == "audio_pak_enabled") {
        const auto* pak = scene.FindAudioPak(args.value("audio_pak", ""));
        return pak != nullptr && pak->enabled == args.value("enabled", true);
    }
    if (condition.function == "random_chance") {
        const float chance = std::clamp(args.value("chance", 1.0f), 0.0f, 1.0f);
        return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) <= chance;
    }
    if (condition.function == "time_since_start") {
        return scene.TimeSeconds() >= std::max(args.value("seconds", 0.0f), 0.0f);
    }
    if (condition.function == "cooldown_ready") {
        const float seconds = std::max(args.value("seconds", 0.0f), 0.0f);
        const std::string trigger = args.value("trigger", std::string{});
        const float last = scene.TriggerLastFiredTime(trigger.empty() ? trigger_name : trigger);
        return last < 0.0f || (scene.TimeSeconds() - last) >= seconds;
    }
    return true;
}

void PythonScripting::ExecuteBuiltinAction(const assets::TriggerAction& action, entities::Scene& scene, const std::string&) {
    const json args = ParseArgsJson(action.args_json);
    if (action.function == "toggle_camera") {
        scene.ToggleCameraMode();
        return;
    }
    if (action.function == "set_camera_mode") {
        scene.SetCameraMode(args.value("mode", "side") == "isometric" ? camera::Mode::Isometric : camera::Mode::Side);
        return;
    }
    if (action.function == "set_camera_zoom") {
        const float zoom = args.value("zoom", 1.0f);
        const float speed = args.value("speed", 0.0f);
        if (speed > 0.0f) {
            scene.SetCameraZoomSmooth(zoom, speed);
        } else {
            scene.SetCameraZoom(zoom);
        }
        return;
    }
    if (action.function == "set_camera_follow") {
        scene.SetCameraFollowEnabled(args.value("enabled", true));
        return;
    }
    if (action.function == "activate_virtual_camera") {
        scene.ActivateVirtualCamera(args.value("camera", std::string{}));
        return;
    }
    if (action.function == "release_virtual_camera") {
        scene.ReleaseVirtualCamera();
        return;
    }
    if (action.function == "play_scene_animation") {
        scene.PlaySceneAnimation(args.value("animation", std::string{}), args.value("restart", true));
        return;
    }
    if (action.function == "stop_scene_animation") {
        scene.StopSceneAnimation(args.value("animation", std::string{}), args.value("restore_state", true));
        return;
    }
    if (action.function == "play_particle_emitter") {
        scene.PlayParticleEmitter(args.value("emitter", std::string{}), args.value("restart", true));
        return;
    }
    if (action.function == "stop_particle_emitter") {
        scene.StopParticleEmitter(args.value("emitter", std::string{}));
        return;
    }
    if (action.function == "burst_particle_emitter") {
        scene.BurstParticleEmitter(args.value("emitter", std::string{}), args.value("count", -1));
        return;
    }
    if (action.function == "log_message") {
        scene.Log(args.value("message", std::string("Trigger fired.")));
        return;
    }
    if (action.function == "play_sound") {
        scene.PlaySound(args.value("sound", std::string{}));
        return;
    }
    if (action.function == "play_music") {
        scene.PlayMusic(args.value("music", std::string{}), args.value("loop", true));
        return;
    }
    if (action.function == "stop_audio") {
        scene.StopAllAudio();
        return;
    }
    if (action.function == "set_entity_visible" || action.function == "set_entity_active") {
        if (auto* entity = scene.FindEntity(args.value("entity", "")); entity != nullptr) {
            if (action.function == "set_entity_visible") {
                entity->visible = args.value("visible", true);
            } else {
                entity->active = args.value("active", true);
            }
        }
        return;
    }
    if (action.function == "delete_entity") {
        if (auto* entity = scene.FindEntity(args.value("entity", "")); entity != nullptr) {
            entity->active = false;
            entity->visible = false;
        }
        return;
    }
    if (action.function == "set_light_enabled") {
        if (auto* light = scene.FindLight(args.value("light", "")); light != nullptr) {
            light->enabled = args.value("enabled", true);
        }
        return;
    }
    if (action.function == "set_audio_source_enabled") {
        if (auto* source = scene.FindAudioSource(args.value("audio_source", "")); source != nullptr) {
            source->enabled = args.value("enabled", true);
        }
        return;
    }
    if (action.function == "set_audio_pak_enabled") {
        if (auto* pak = scene.FindAudioPak(args.value("audio_pak", "")); pak != nullptr) {
            pak->enabled = args.value("enabled", true);
        }
        return;
    }
    if (action.function == "set_trigger_enabled") {
        for (auto& trigger : scene.EditableLevel().triggers) {
            if (trigger.id == args.value("trigger", std::string{}) || trigger.name == args.value("trigger", std::string{})) {
                trigger.enabled = args.value("enabled", true);
                break;
            }
        }
        return;
    }
    if (action.function == "set_music_volume") {
        scene.Assets().SetMusicVolume(std::clamp(args.value("volume", 1.0f), 0.0f, 1.0f));
        return;
    }
    if (action.function == "set_sound_volume") {
        scene.Assets().SetSoundVolume(std::clamp(args.value("volume", 1.0f), 0.0f, 1.0f));
        return;
    }
}

py::object PythonScripting::ImportModule(const std::string& module_name) {
    return py::module_::import(module_name.c_str());
}

void PythonScripting::RefreshSysPath() {
    if (!interpreter_) {
        return;
    }

    try {
        py::gil_scoped_acquire acquire;
        py::module_ sys = py::module_::import("sys");
        const std::string project = project_root_.string();
        const std::string scripts = (project_root_ / "scripts").string();
        sys.attr("path").attr("insert")(0, project);
        sys.attr("path").attr("insert")(0, scripts);
        if (core::FileIO::PackMounted() && !std::filesystem::exists(project_root_ / "scripts")) {
            py::exec(R"py(
import importlib.abc
import importlib.util
import sys
import novaiso

class _NovaIsoPackedImporter(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    def _candidates(self, fullname):
        module_path = fullname.replace(".", "/")
        return [
            f"scripts/{module_path}.py",
            f"scripts/{module_path}/__init__.py",
        ]

    def find_spec(self, fullname, path=None, target=None):
        for candidate in self._candidates(fullname):
            if novaiso._file_exists(candidate):
                is_package = candidate.endswith("/__init__.py")
                return importlib.util.spec_from_loader(fullname, self, origin=candidate, is_package=is_package)
        return None

    def create_module(self, spec):
        return None

    def exec_module(self, module):
        origin = module.__spec__.origin
        source = novaiso._read_text(origin)
        module.__file__ = origin
        if origin.endswith("/__init__.py"):
            module.__path__ = [origin.rsplit("/", 1)[0]]
        exec(compile(source, origin, "exec"), module.__dict__)

if not any(type(importer).__name__ == "_NovaIsoPackedImporter" for importer in sys.meta_path):
    sys.meta_path.insert(0, _NovaIsoPackedImporter())
)py");
        }
    } catch (const py::error_already_set& error) {
        PrintPythonError(error);
    }
}

void PythonScripting::PrintPythonError(const py::error_already_set& error) const {
    std::cerr << error.what() << '\n';
    try {
        py::gil_scoped_acquire acquire;
        py::module_::import("traceback").attr("print_exc")();
    } catch (...) {
    }
}

}  // namespace novaiso::scripting
