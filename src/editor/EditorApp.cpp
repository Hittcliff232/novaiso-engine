#include "editor/EditorApp.h"

#include "core/EmbeddedResources.h"
#include "core/FileIO.h"
#include "core/ImageIO.h"
#include "core/ThreadPool.h"
#include "core/VulkanBootstrap.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <stb_image.h>

#include <imgui.h>
#include <SDL.h>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <ctime>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <shobjidl.h>
#include <shellapi.h>
#ifdef PlaySound
#undef PlaySound
#endif
#endif

namespace novaiso::editor {

namespace {

TextEditor::LanguageDefinition PythonDefinition() {
    auto language = TextEditor::LanguageDefinition::Lua();
    language.mName = "Python";
    language.mSingleLineComment = "#";
    language.mCommentStart.clear();
    language.mCommentEnd.clear();
    language.mKeywords.insert("def");
    language.mKeywords.insert("class");
    language.mKeywords.insert("import");
    language.mKeywords.insert("from");
    language.mKeywords.insert("return");
    language.mKeywords.insert("if");
    language.mKeywords.insert("elif");
    language.mKeywords.insert("else");
    language.mKeywords.insert("while");
    language.mKeywords.insert("for");
    language.mKeywords.insert("in");
    language.mKeywords.insert("True");
    language.mKeywords.insert("False");
    language.mKeywords.insert("None");
    return language;
}

bool IsEditableTextExtension(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    if (path.filename() == "CMakeLists.txt") {
        return true;
    }

    static const std::array<std::string_view, 26> extensions{
        ".py", ".json", ".niso", ".nsprite", ".nobject", ".ntrigger", ".nanim", ".nparticle", ".nshader",
        ".html", ".css", ".js",
        ".vert", ".frag", ".glsl", ".shader",
        ".cpp", ".cxx", ".cc", ".c",
        ".h", ".hpp", ".hxx", ".inl",
        ".txt", ".md"
    };
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
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

const TextEditor::LanguageDefinition& LanguageDefinitionForPath(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    if (extension == ".py") {
        static TextEditor::LanguageDefinition python = PythonDefinition();
        return python;
    }
    if (extension == ".vert" || extension == ".frag" || extension == ".glsl" || extension == ".shader") {
        return TextEditor::LanguageDefinition::GLSL();
    }
    if (extension == ".html" || extension == ".css" || extension == ".js") {
        return TextEditor::LanguageDefinition::Lua();
    }
    if (extension == ".c") {
        return TextEditor::LanguageDefinition::C();
    }
    if (extension == ".cpp" || extension == ".cxx" || extension == ".cc" ||
        extension == ".h" || extension == ".hpp" || extension == ".hxx" ||
        extension == ".inl" || path.filename() == "CMakeLists.txt") {
        return TextEditor::LanguageDefinition::CPlusPlus();
    }
    if (extension == ".json" || extension == ".niso" || extension == ".nsprite" ||
        extension == ".nobject" || extension == ".ntrigger" || extension == ".nanim" || extension == ".nparticle" ||
        extension == ".nshader" || extension == ".txt" ||
        extension == ".md") {
        return TextEditor::LanguageDefinition::Lua();
    }
    return TextEditor::LanguageDefinition::CPlusPlus();
}

bool RegisterHoveredTextInput(std::string& value);

bool EditString(const char* label, std::string& value, std::size_t capacity = 512) {
    std::vector<char> buffer(capacity, '\0');
    std::strncpy(buffer.data(), value.c_str(), capacity - 1);
    bool changed = false;
    if (ImGui::InputText(label, buffer.data(), buffer.size())) {
        value = buffer.data();
        changed = true;
    }
    changed |= RegisterHoveredTextInput(value);
    return changed;
}

bool EditPasswordString(const char* label, std::string& value, std::size_t capacity = 512) {
    std::vector<char> buffer(capacity, '\0');
    std::strncpy(buffer.data(), value.c_str(), capacity - 1);
    bool changed = false;
    if (ImGui::InputText(label, buffer.data(), buffer.size(), ImGuiInputTextFlags_Password)) {
        value = buffer.data();
        changed = true;
    }
    changed |= RegisterHoveredTextInput(value);
    return changed;
}

bool EditJsonString(const char* label, std::string& value, std::size_t capacity = 2048) {
    std::vector<char> buffer(capacity, '\0');
    std::strncpy(buffer.data(), value.c_str(), capacity - 1);
    bool changed = false;
    if (ImGui::InputTextMultiline(label, buffer.data(), buffer.size(), ImVec2(-1.0f, 96.0f))) {
        value = buffer.data();
        changed = true;
    }
    changed |= RegisterHoveredTextInput(value);
    return changed;
}

std::string* g_hovered_text_input_target = nullptr;

void ResetHoveredTextInputTarget() {
    g_hovered_text_input_target = nullptr;
}

std::optional<std::string> AcceptTextInputPathPayload() {
    static constexpr std::array<const char*, 7> kPayloadTypes{{
        "NOVAISO_GENERIC_PATH",
        "NOVAISO_TEXTURE_PATH",
        "NOVAISO_AUDIO_PATH",
        "NOVAISO_OBJECT_PATH",
        "NOVAISO_TRIGGER_PATH",
        "NOVAISO_ANIMATION_PATH",
        "NOVAISO_PARTICLE_PATH"
    }};

    if (!ImGui::BeginDragDropTarget()) {
        return std::nullopt;
    }

    std::optional<std::string> result;
    for (const char* payload_type : kPayloadTypes) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(payload_type)) {
            result = std::string(static_cast<const char*>(payload->Data));
            break;
        }
    }
    ImGui::EndDragDropTarget();
    return result;
}

bool RegisterHoveredTextInput(std::string& value) {
    bool changed = false;
    if (const auto dropped = AcceptTextInputPathPayload(); dropped.has_value()) {
        value = *dropped;
        changed = true;
    }
    if (ImGui::IsItemActive() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        g_hovered_text_input_target = &value;
    }
    return changed;
}

std::string NormalizeDroppedPathForInput(const std::filesystem::path& path, const std::filesystem::path& project_root) {
    if (path.empty()) {
        return {};
    }
    std::error_code error;
    const std::filesystem::path normalized_path = std::filesystem::weakly_canonical(path, error);
    const std::filesystem::path safe_path = error ? path.lexically_normal() : normalized_path.lexically_normal();
    if (!project_root.empty()) {
        error.clear();
        const std::filesystem::path normalized_root = std::filesystem::weakly_canonical(project_root, error);
        if (!error && !normalized_root.empty()) {
            const std::string root_text = normalized_root.generic_string();
            const std::string path_text = safe_path.generic_string();
            const bool inside_root =
                path_text == root_text ||
                (path_text.size() > root_text.size() &&
                 path_text.starts_with(root_text) &&
                 (path_text[root_text.size()] == '/' || path_text[root_text.size()] == '\\'));
            if (inside_root) {
                std::error_code relative_error;
                const std::filesystem::path relative = std::filesystem::relative(safe_path, normalized_root, relative_error);
                if (!relative_error) {
                    return relative.generic_string();
                }
            }
        }
    }
    return safe_path.generic_string();
}

bool TryAssignDroppedPathToHoveredInput(const std::filesystem::path& path, const std::filesystem::path& project_root) {
    if (g_hovered_text_input_target == nullptr) {
        return false;
    }
    *g_hovered_text_input_target = NormalizeDroppedPathForInput(path, project_root);
    return true;
}

std::string StableWindowLabel(std::string_view visible_title, std::string_view stable_id) {
    return std::string(visible_title) + "###" + std::string(stable_id);
}

std::string StableWindowLabel(std::string_view window_tag, std::string_view visible_title, std::string_view stable_id) {
    return "[" + std::string(window_tag) + "] " + std::string(visible_title) + "###" + std::string(stable_id);
}

std::string GeneratePasswordString(std::size_t length = 24) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^&*";
    std::random_device seed_device;
    std::mt19937_64 random(seed_device());
    std::uniform_int_distribution<std::size_t> distribution(0, alphabet.size() - 1);
    std::string password;
    password.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        password.push_back(alphabet[distribution(random)]);
    }
    return password;
}

ImU32 ToImColor(glm::vec4 color, float alpha_scale = 1.0f) {
    return IM_COL32(
        static_cast<int>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<int>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<int>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
        static_cast<int>(std::clamp(color.a * alpha_scale, 0.0f, 1.0f) * 255.0f)
    );
}

void DecorateEditorWindow(glm::vec4 accent = {0.28f, 0.62f, 0.98f, 1.0f}) {
    if (ImGui::IsWindowCollapsed()) {
        return;
    }

    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 max{min.x + size.x, min.y + size.y};
    const float rounding = ImGui::GetStyle().WindowRounding;
    ImGuiViewport* viewport = ImGui::GetWindowViewport();

    ImDrawList* background = ImGui::GetBackgroundDrawList(viewport);
    background->AddRectFilled(
        {min.x + 8.0f, min.y + 10.0f},
        {max.x + 8.0f, max.y + 12.0f},
        IM_COL32(0, 0, 0, 20),
        rounding + 4.0f);
    background->AddRectFilled(
        {min.x + 4.0f, min.y + 5.0f},
        {max.x + 4.0f, max.y + 7.0f},
        IM_COL32(0, 0, 0, 10),
        rounding + 3.0f);

    ImDrawList* foreground = ImGui::GetWindowDrawList();
    foreground->AddRectFilledMultiColor(
        min,
        {max.x, std::min(max.y, min.y + 34.0f)},
        ToImColor(accent, 0.16f),
        ToImColor({accent.r * 0.82f, accent.g * 1.02f, accent.b * 1.08f, accent.a}, 0.08f),
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 0));
    foreground->AddRectFilled(
        {min.x + 1.0f, min.y + 1.0f},
        {min.x + 5.0f, max.y - 1.0f},
        ToImColor(accent, 0.88f),
        rounding);
    foreground->AddRect(min, max, IM_COL32(120, 146, 178, 96), rounding, 0, 1.0f);
}

bool IsImageExtension(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
           extension == ".tga" || extension == ".ppm" || extension == ".gif";
}

bool IsAudioExtension(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3";
}

void EnsureTileLayerStorage(assets::TileLayer& layer) {
    layer.width = std::max(layer.width, 1);
    layer.height = std::max(layer.height, 1);
    const std::size_t tile_count = static_cast<std::size_t>(layer.width) * static_cast<std::size_t>(layer.height);
    if (layer.tiles.size() != tile_count) {
        layer.tiles.resize(tile_count, 0);
    }
}

void SyncLevelTilesetLayout(assets::LevelData& level, glm::ivec2 texture_size) {
    level.tile_width = std::max(level.tile_width, 1);
    level.tile_height = std::max(level.tile_height, 1);
    level.tileset.tile_width = level.tile_width;
    level.tileset.tile_height = level.tile_height;
    level.tileset.columns = std::max(1, texture_size.x / std::max(level.tile_width, 1));
    level.tileset.rows = std::max(1, texture_size.y / std::max(level.tile_height, 1));
}

int TilesetTileCount(const assets::LevelData& level) {
    return std::max(1, level.tileset.columns) * std::max(1, level.tileset.rows);
}

glm::vec4 TilesetTileUv(const assets::LevelData& level, int tile_index) {
    const int columns = std::max(1, level.tileset.columns);
    const int rows = std::max(1, level.tileset.rows);
    const glm::vec2 uv_step{1.0f / static_cast<float>(columns), 1.0f / static_cast<float>(rows)};
    const int clamped_index = std::clamp(tile_index, 0, std::max(columns * rows - 1, 0));
    const int uv_x = clamped_index % columns;
    const int uv_y = clamped_index / columns;
    return {
        uv_x * uv_step.x,
        uv_y * uv_step.y,
        (uv_x + 1) * uv_step.x,
        (uv_y + 1) * uv_step.y
    };
}

bool IsScriptExtension(const std::filesystem::path& path) {
    return path.extension() == ".py";
}

std::string SanitizeName(std::string value) {
    for (char& character : value) {
        const unsigned char current = static_cast<unsigned char>(character);
        if (std::isalnum(current) != 0) {
            character = static_cast<char>(std::tolower(current));
        } else {
            character = '_';
        }
    }
    value.erase(std::unique(value.begin(), value.end(), [](char a, char b) { return a == '_' && b == '_'; }), value.end());
    while (!value.empty() && value.front() == '_') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == '_') {
        value.pop_back();
    }
    return value.empty() ? "resource" : value;
}

struct ReliefMaterialSettings {
    bool enabled = false;
    float bump_strength = 1.0f;
    float relief_depth = 0.035f;
    float parallax_depth = 0.018f;
    float relief_contrast = 1.35f;
};

struct GeneratedReliefMaps {
    std::string normal_map;
    std::string height_map;
    std::string displacement_map;
    ReliefMaterialSettings settings;
};

void ClampReliefMaterial(ReliefMaterialSettings& settings) {
    settings.bump_strength = std::clamp(settings.bump_strength, 0.0f, 4.0f);
    settings.relief_depth = std::clamp(settings.relief_depth, 0.0f, 0.12f);
    settings.parallax_depth = std::clamp(settings.parallax_depth, 0.0f, 0.08f);
    settings.relief_contrast = std::clamp(settings.relief_contrast, 0.2f, 4.0f);
}

bool AnalyzeReliefMaterialFromPixels(
    const std::vector<float>& height_values,
    int width,
    int height,
    ReliefMaterialSettings& settings) {
    if (width <= 1 || height <= 1 || height_values.empty()) {
        return false;
    }

    double luminance_sum = 0.0;
    double luminance_sum_sq = 0.0;
    double edge_sum = 0.0;
    std::uint64_t sample_count = 0;
    std::uint64_t edge_count = 0;

    auto value_at = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return static_cast<double>(height_values[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]);
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double luminance = value_at(x, y);
            luminance_sum += luminance;
            luminance_sum_sq += luminance * luminance;
            ++sample_count;
            if (x + 1 < width) {
                edge_sum += std::abs(luminance - value_at(x + 1, y));
                ++edge_count;
            }
            if (y + 1 < height) {
                edge_sum += std::abs(luminance - value_at(x, y + 1));
                ++edge_count;
            }
        }
    }

    if (sample_count == 0 || edge_count == 0) {
        return false;
    }

    const double mean = luminance_sum / static_cast<double>(sample_count);
    const double variance = std::max(0.0, luminance_sum_sq / static_cast<double>(sample_count) - mean * mean);
    const double edge_energy = edge_sum / static_cast<double>(edge_count);
    settings.enabled = true;
    settings.bump_strength = static_cast<float>(std::clamp(0.85 + edge_energy * 8.0 + variance * 4.0, 0.55, 3.8));
    settings.relief_depth = static_cast<float>(std::clamp(0.014 + edge_energy * 0.16 + variance * 0.08, 0.010, 0.10));
    settings.parallax_depth = static_cast<float>(std::clamp(settings.relief_depth * (0.55f + static_cast<float>(edge_energy) * 1.35f), 0.0f, 0.06f));
    settings.relief_contrast = static_cast<float>(std::clamp(0.94 + edge_energy * 8.2 + variance * 3.4, 0.8, 3.6));
    ClampReliefMaterial(settings);
    return true;
}

bool GenerateReliefMaterialFromTexture(const std::filesystem::path& absolute_texture_path, ReliefMaterialSettings& settings) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* raw = stbi_load(absolute_texture_path.string().c_str(), &width, &height, &channels, 4);
    if (raw == nullptr || width <= 1 || height <= 1) {
        if (raw != nullptr) {
            stbi_image_free(raw);
        }
        return false;
    }

    std::vector<float> height_values(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0f);
    auto luminance_at = [&](int x, int y) -> float {
        const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) * 4;
        const float r = static_cast<float>(raw[index + 0]) / 255.0f;
        const float g = static_cast<float>(raw[index + 1]) / 255.0f;
        const float b = static_cast<float>(raw[index + 2]) / 255.0f;
        const float a = static_cast<float>(raw[index + 3]) / 255.0f;
        return std::clamp((r * 0.299f + g * 0.587f + b * 0.114f) * (0.78f + a * 0.22f), 0.0f, 1.0f);
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            height_values[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = luminance_at(x, y);
        }
    }

    stbi_image_free(raw);
    return AnalyzeReliefMaterialFromPixels(height_values, width, height, settings);
}

std::optional<GeneratedReliefMaps> GenerateReliefMapsFromTexture(
    const std::filesystem::path& project_root,
    const std::string& texture_relative,
    const std::string& name_hint) {
    if (texture_relative.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path absolute_texture_path = project_root / texture_relative;
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* raw = stbi_load(absolute_texture_path.string().c_str(), &width, &height, &channels, 4);
    if (raw == nullptr || width <= 1 || height <= 1) {
        if (raw != nullptr) {
            stbi_image_free(raw);
        }
        return std::nullopt;
    }

    std::vector<float> base_height(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0f);
    auto index_of = [&](int x, int y) -> std::size_t {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = index_of(x, y) * 4;
            const float r = static_cast<float>(raw[index + 0]) / 255.0f;
            const float g = static_cast<float>(raw[index + 1]) / 255.0f;
            const float b = static_cast<float>(raw[index + 2]) / 255.0f;
            const float a = static_cast<float>(raw[index + 3]) / 255.0f;
            base_height[index_of(x, y)] = std::clamp((r * 0.299f + g * 0.587f + b * 0.114f) * (0.76f + a * 0.24f), 0.0f, 1.0f);
        }
    }

    std::vector<float> blurred_height = base_height;
    std::vector<float> scratch = base_height;
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float accum = 0.0f;
                float weight_sum = 0.0f;
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int sx = std::clamp(x + ox, 0, width - 1);
                        const int sy = std::clamp(y + oy, 0, height - 1);
                        const float weight = (ox == 0 && oy == 0) ? 4.0f : ((ox == 0 || oy == 0) ? 2.0f : 1.0f);
                        accum += scratch[index_of(sx, sy)] * weight;
                        weight_sum += weight;
                    }
                }
                blurred_height[index_of(x, y)] = accum / std::max(weight_sum, 0.0001f);
            }
        }
        scratch = blurred_height;
    }

    ReliefMaterialSettings settings;
    if (!AnalyzeReliefMaterialFromPixels(blurred_height, width, height, settings)) {
        stbi_image_free(raw);
        return std::nullopt;
    }

    std::vector<std::uint8_t> height_pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 255);
    std::vector<std::uint8_t> displacement_pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 255);
    std::vector<std::uint8_t> normal_pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 255);

    auto height_at = [&](int x, int y) {
        return blurred_height[index_of(std::clamp(x, 0, width - 1), std::clamp(y, 0, height - 1))];
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float current = height_at(x, y);
            const float left = height_at(x - 1, y);
            const float right = height_at(x + 1, y);
            const float up = height_at(x, y - 1);
            const float down = height_at(x, y + 1);
            const float dx = right - left;
            const float dy = down - up;
            const float laplacian = (left + right + up + down) * 0.25f - current;
            const float displacement = std::clamp(current * 0.72f + laplacian * 1.65f + 0.5f, 0.0f, 1.0f);
            const glm::vec3 normal = glm::normalize(glm::vec3(
                -dx * (10.0f + settings.bump_strength * 5.0f),
                -dy * (10.0f + settings.bump_strength * 5.0f),
                1.0f
            ));

            const std::size_t pixel_index = index_of(x, y) * 4;
            const std::uint8_t height_byte = static_cast<std::uint8_t>(std::clamp(current, 0.0f, 1.0f) * 255.0f);
            const std::uint8_t displacement_byte = static_cast<std::uint8_t>(displacement * 255.0f);
            height_pixels[pixel_index + 0] = height_byte;
            height_pixels[pixel_index + 1] = height_byte;
            height_pixels[pixel_index + 2] = height_byte;
            height_pixels[pixel_index + 3] = raw[pixel_index + 3];
            displacement_pixels[pixel_index + 0] = displacement_byte;
            displacement_pixels[pixel_index + 1] = displacement_byte;
            displacement_pixels[pixel_index + 2] = displacement_byte;
            displacement_pixels[pixel_index + 3] = raw[pixel_index + 3];
            normal_pixels[pixel_index + 0] = static_cast<std::uint8_t>(std::clamp(normal.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            normal_pixels[pixel_index + 1] = static_cast<std::uint8_t>(std::clamp(normal.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            normal_pixels[pixel_index + 2] = static_cast<std::uint8_t>(std::clamp(normal.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            normal_pixels[pixel_index + 3] = raw[pixel_index + 3];
        }
    }

    stbi_image_free(raw);

    const std::string safe_hint = SanitizeName(name_hint.empty() ? std::filesystem::path(texture_relative).stem().string() : name_hint);
    auto make_unique_map_path = [&](std::string_view suffix) {
        std::filesystem::path relative;
        int serial = 0;
        do {
            const std::string tail = serial == 0 ? "" : "_" + std::to_string(serial);
            relative = std::filesystem::path("assets/images/generated") / (safe_hint + tail + "_" + std::string(suffix) + ".png");
            ++serial;
        } while (std::filesystem::exists(project_root / relative));
        return relative.generic_string();
    };

    GeneratedReliefMaps maps;
    maps.normal_map = make_unique_map_path("normal");
    maps.height_map = make_unique_map_path("height");
    maps.displacement_map = make_unique_map_path("displacement");
    maps.settings = settings;
    std::filesystem::create_directories((project_root / std::filesystem::path(maps.normal_map)).parent_path());

    const bool normal_written = stbi_write_png((project_root / std::filesystem::path(maps.normal_map)).string().c_str(), width, height, 4, normal_pixels.data(), width * 4) != 0;
    const bool height_written = stbi_write_png((project_root / std::filesystem::path(maps.height_map)).string().c_str(), width, height, 4, height_pixels.data(), width * 4) != 0;
    const bool displacement_written = stbi_write_png((project_root / std::filesystem::path(maps.displacement_map)).string().c_str(), width, height, 4, displacement_pixels.data(), width * 4) != 0;
    if (!normal_written || !height_written || !displacement_written) {
        return std::nullopt;
    }

    return maps;
}

std::optional<std::string> ResolvePreviewTextureForObjectAsset(const std::filesystem::path& project_root, const assets::ObjectAsset& object) {
    if (!object.sprite.empty() && std::filesystem::exists(project_root / object.sprite)) {
        const assets::SpriteAsset sprite = assets::LoadSpriteAsset(project_root / object.sprite);
        const std::string desired_animation = object.default_animation.empty() ? sprite.default_animation : object.default_animation;
        for (const auto& animation : sprite.animations) {
            if (animation.frames.empty()) {
                continue;
            }
            if (animation.name == desired_animation) {
                return animation.frames.front().texture;
            }
        }
        for (const auto& animation : sprite.animations) {
            if (!animation.frames.empty()) {
                return animation.frames.front().texture;
            }
        }
    }
    return std::nullopt;
}

glm::vec4 ToastColorForMessage(const std::string& message) {
    std::string lowered = message;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lowered.find("failed") != std::string::npos ||
        lowered.find("error") != std::string::npos ||
        lowered.find("ошиб") != std::string::npos) {
        return {0.96f, 0.32f, 0.32f, 1.0f};
    }
    if (lowered.find("shader") != std::string::npos) {
        return {0.98f, 0.72f, 0.26f, 1.0f};
    }
    if (lowered.find("saved") != std::string::npos ||
        lowered.find("generated") != std::string::npos ||
        lowered.find("build") != std::string::npos ||
        lowered.find("compiled") != std::string::npos) {
        return {0.24f, 0.82f, 0.56f, 1.0f};
    }
    return {0.24f, 0.68f, 0.98f, 1.0f};
}

#if defined(_WIN32)
struct SystemPerformanceSample {
    float system_cpu_percent = 0.0f;
    float process_cpu_percent = 0.0f;
    float process_cpu_core_equivalent_percent = 0.0f;
    std::vector<float> core_cpu_percent;
    float ram_used_mb = 0.0f;
    float ram_total_mb = 0.0f;
    float process_ram_mb = 0.0f;
    float vram_used_mb = 0.0f;
    float vram_budget_mb = 0.0f;
};

ULONGLONG FileTimeToUint64(const FILETIME& value) {
    return (static_cast<ULONGLONG>(value.dwHighDateTime) << 32ULL) | static_cast<ULONGLONG>(value.dwLowDateTime);
}

class WindowsPerformanceMonitor {
public:
    ~WindowsPerformanceMonitor() {
        Shutdown();
    }

    SystemPerformanceSample Sample() {
        EnsureInitialized();

        SystemPerformanceSample sample;
        if (pdh_ready_) {
            PdhCollectQueryData(query_);
            sample.system_cpu_percent = ReadCounter(total_cpu_counter_);
            sample.core_cpu_percent.reserve(core_cpu_counters_.size());
            for (PDH_HCOUNTER counter : core_cpu_counters_) {
                sample.core_cpu_percent.push_back(ReadCounter(counter));
            }
        }

        MEMORYSTATUSEX memory_status{};
        memory_status.dwLength = sizeof(memory_status);
        if (GlobalMemoryStatusEx(&memory_status) != FALSE) {
            sample.ram_total_mb = static_cast<float>(memory_status.ullTotalPhys / (1024.0 * 1024.0));
            sample.ram_used_mb = static_cast<float>((memory_status.ullTotalPhys - memory_status.ullAvailPhys) / (1024.0 * 1024.0));
        }

        PROCESS_MEMORY_COUNTERS_EX process_memory{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process_memory), sizeof(process_memory)) != FALSE) {
            sample.process_ram_mb = static_cast<float>(process_memory.WorkingSetSize / (1024.0 * 1024.0));
        }

        if (adapter_ != nullptr) {
            DXGI_QUERY_VIDEO_MEMORY_INFO video_info{};
            if (SUCCEEDED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &video_info))) {
                sample.vram_used_mb = static_cast<float>(video_info.CurrentUsage / (1024.0 * 1024.0));
                sample.vram_budget_mb = static_cast<float>(video_info.Budget / (1024.0 * 1024.0));
            }
        }

        FILETIME creation_time{};
        FILETIME exit_time{};
        FILETIME kernel_time{};
        FILETIME user_time{};
        FILETIME now{};
        if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time) != FALSE) {
            GetSystemTimeAsFileTime(&now);
            const ULONGLONG current_wall = FileTimeToUint64(now);
            const ULONGLONG current_kernel = FileTimeToUint64(kernel_time);
            const ULONGLONG current_user = FileTimeToUint64(user_time);
            if (last_wall_time_100ns_ != 0 && current_wall > last_wall_time_100ns_) {
                const ULONGLONG delta_wall = current_wall - last_wall_time_100ns_;
                const ULONGLONG delta_cpu = (current_kernel - last_process_kernel_100ns_) + (current_user - last_process_user_100ns_);
                const double normalized = static_cast<double>(delta_cpu) / static_cast<double>(delta_wall);
                sample.process_cpu_core_equivalent_percent = static_cast<float>(std::max(normalized * 100.0, 0.0));
                sample.process_cpu_percent = static_cast<float>(std::clamp(normalized * 100.0 / std::max<DWORD>(processor_count_, 1), 0.0, 100.0));
            }
            last_wall_time_100ns_ = current_wall;
            last_process_kernel_100ns_ = current_kernel;
            last_process_user_100ns_ = current_user;
        }

        return sample;
    }

private:
    void EnsureInitialized() {
        if (initialized_) {
            return;
        }
        initialized_ = true;

        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        processor_count_ = std::max<DWORD>(info.dwNumberOfProcessors, 1);

        if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(query_, L"\\Processor(_Total)\\% Processor Time", 0, &total_cpu_counter_) == ERROR_SUCCESS) {
                for (DWORD core_index = 0; core_index < processor_count_; ++core_index) {
                    std::wstring path = L"\\Processor(" + std::to_wstring(core_index) + L")\\% Processor Time";
                    PDH_HCOUNTER counter = nullptr;
                    if (PdhAddEnglishCounterW(query_, path.c_str(), 0, &counter) == ERROR_SUCCESS) {
                        core_cpu_counters_.push_back(counter);
                    }
                }
                PdhCollectQueryData(query_);
                pdh_ready_ = true;
            }
        }

        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT adapter_index = 0;; ++adapter_index) {
                IDXGIAdapter1* candidate = nullptr;
                if (factory->EnumAdapters1(adapter_index, &candidate) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc{};
                candidate->GetDesc1(&desc);
                const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                if (!software) {
                    IDXGIAdapter3* adapter3 = nullptr;
                    if (SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
                        adapter_ = adapter3;
                        candidate->Release();
                        break;
                    }
                }
                candidate->Release();
            }
            factory->Release();
        }
    }

    void Shutdown() {
        if (adapter_ != nullptr) {
            adapter_->Release();
            adapter_ = nullptr;
        }
        if (query_ != nullptr) {
            PdhCloseQuery(query_);
            query_ = nullptr;
        }
        total_cpu_counter_ = nullptr;
        core_cpu_counters_.clear();
        pdh_ready_ = false;
        initialized_ = false;
    }

    float ReadCounter(PDH_HCOUNTER counter) const {
        PDH_FMT_COUNTERVALUE value{};
        if (counter == nullptr || PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) {
            return 0.0f;
        }
        return static_cast<float>(std::clamp(value.doubleValue, 0.0, 100.0));
    }

    bool initialized_ = false;
    bool pdh_ready_ = false;
    DWORD processor_count_ = 1;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER total_cpu_counter_ = nullptr;
    std::vector<PDH_HCOUNTER> core_cpu_counters_;
    IDXGIAdapter3* adapter_ = nullptr;
    ULONGLONG last_wall_time_100ns_ = 0;
    ULONGLONG last_process_kernel_100ns_ = 0;
    ULONGLONG last_process_user_100ns_ = 0;
};
#endif

glm::u8vec4 ToByteColor(glm::vec4 color) {
    return {
        static_cast<std::uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f)
    };
}

std::vector<std::uint8_t> MakeBlankPixels(int width, int height) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(width * height * 4), 0);
}

std::vector<std::uint8_t> MakeRadialParticlePixels(int width,
                                                   int height,
                                                   glm::vec4 inner,
                                                   glm::vec4 outer,
                                                   float power = 1.6f) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4), 0);
    const glm::vec2 center{(static_cast<float>(width) - 1.0f) * 0.5f, (static_cast<float>(height) - 1.0f) * 0.5f};
    const float radius = std::max(1.0f, std::min(static_cast<float>(width), static_cast<float>(height)) * 0.5f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const glm::vec2 point{static_cast<float>(x), static_cast<float>(y)};
            const float distance = glm::length(point - center) / radius;
            const float falloff = std::pow(std::clamp(1.0f - distance, 0.0f, 1.0f), power);
            const glm::vec4 color = glm::mix(outer, inner, falloff);
            const glm::u8vec4 byte_color = ToByteColor(color);
            const std::size_t index = static_cast<std::size_t>((y * width + x) * 4);
            pixels[index + 0] = byte_color.r;
            pixels[index + 1] = byte_color.g;
            pixels[index + 2] = byte_color.b;
            pixels[index + 3] = byte_color.a;
        }
    }
    return pixels;
}

std::vector<std::uint8_t> MakeSparkParticlePixels(int width, int height, glm::vec4 color) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4), 0);
    const glm::vec2 center{(static_cast<float>(width) - 1.0f) * 0.5f, (static_cast<float>(height) - 1.0f) * 0.5f};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float dx = std::abs(static_cast<float>(x) - center.x) / std::max(center.x, 1.0f);
            const float dy = std::abs(static_cast<float>(y) - center.y) / std::max(center.y, 1.0f);
            const float line = std::clamp(1.0f - std::min(dx, dy), 0.0f, 1.0f);
            const float core = std::clamp(1.0f - glm::length(glm::vec2(dx, dy)), 0.0f, 1.0f);
            const float alpha = std::pow(std::max(line, core), 2.2f);
            const glm::u8vec4 byte_color = ToByteColor(glm::vec4(color.r, color.g, color.b, color.a * alpha));
            const std::size_t index = static_cast<std::size_t>((y * width + x) * 4);
            pixels[index + 0] = byte_color.r;
            pixels[index + 1] = byte_color.g;
            pixels[index + 2] = byte_color.b;
            pixels[index + 3] = byte_color.a;
        }
    }
    return pixels;
}

std::filesystem::path NormalizeExistingPath(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : absolute.lexically_normal();
}

bool ReadTga32(const std::filesystem::path& path, int& width, int& height, std::vector<std::uint8_t>& pixels) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    std::array<std::uint8_t, 18> header{};
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!stream || header[2] != 2) {
        return false;
    }

    width = header[12] | (header[13] << 8);
    height = header[14] | (header[15] << 8);
    const int bits_per_pixel = header[16];
    if (width <= 0 || height <= 0 || (bits_per_pixel != 32 && bits_per_pixel != 24)) {
        return false;
    }

    const std::size_t source_size = static_cast<std::size_t>(width * height * (bits_per_pixel / 8));
    std::vector<std::uint8_t> source(source_size);
    stream.read(reinterpret_cast<char*>(source.data()), static_cast<std::streamsize>(source.size()));
    if (!stream) {
        return false;
    }

    pixels.resize(static_cast<std::size_t>(width * height * 4));
    const bool top_left_origin = (header[17] & 0x20) != 0;
    for (int y = 0; y < height; ++y) {
        const int src_y = top_left_origin ? y : (height - 1 - y);
        for (int x = 0; x < width; ++x) {
            const std::size_t source_index = static_cast<std::size_t>((src_y * width + x) * (bits_per_pixel / 8));
            const std::size_t target_index = static_cast<std::size_t>((y * width + x) * 4);
            pixels[target_index + 0] = source[source_index + 2];
            pixels[target_index + 1] = source[source_index + 1];
            pixels[target_index + 2] = source[source_index + 0];
            pixels[target_index + 3] = bits_per_pixel == 32 ? source[source_index + 3] : 255;
        }
    }

    return true;
}

void WriteTga32(const std::filesystem::path& path, int width, int height, const std::vector<std::uint8_t>& pixels) {
    core::FileIO::EnsureDirectory(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    std::array<std::uint8_t, 18> header{};
    header[2] = 2;
    header[12] = static_cast<std::uint8_t>(width & 0xFF);
    header[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);
    header[14] = static_cast<std::uint8_t>(height & 0xFF);
    header[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);
    header[16] = 32;
    header[17] = 0x20;
    stream.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>((y * width + x) * 4);
            const std::array<std::uint8_t, 4> bgra{
                pixels[index + 2],
                pixels[index + 1],
                pixels[index + 0],
                pixels[index + 3]
            };
            stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
        }
    }
}
bool PopulateSpriteFromDecodedGif(
    const std::filesystem::path& project_root,
    const std::function<std::string(const std::filesystem::path&, const std::string&, const std::string&)>& make_unique_relative_path,
    const std::string& sprite_name,
    const std::string& texture_path,
    assets::SpriteAsset& sprite) {
    if (std::filesystem::path(texture_path).extension() != ".gif") {
        return false;
    }

    core::DecodedImage decoded;
    if (!core::DecodeImageFile(project_root / texture_path, decoded) || decoded.frames.empty()) {
        return false;
    }

    sprite.canvas_size = {
        std::max(decoded.width, 16),
        std::max(decoded.height, 16)
    };
    sprite.default_animation = "idle";
    sprite.animations.clear();

    assets::SpriteAnimation animation;
    animation.name = "idle";
    animation.loop = true;

    int frame_index = 0;
    for (const auto& frame : decoded.frames) {
        if (frame.rgba.empty()) {
            continue;
        }
        const std::string frame_texture = make_unique_relative_path("assets/images/generated", sprite_name + "_gif_frame_" + std::to_string(frame_index++), ".tga");
        WriteTga32(project_root / frame_texture, decoded.width, decoded.height, frame.rgba);
        animation.frames.push_back({
            .texture = frame_texture,
            .duration = std::clamp(static_cast<float>(frame.duration_ms) / 1000.0f, 0.02f, 5.0f)
        });
    }

    if (animation.frames.empty()) {
        return false;
    }

    sprite.animations.push_back(std::move(animation));
    return true;
}

void EnsureSpriteStructure(assets::SpriteAsset& sprite, const std::string& default_texture) {
    if (sprite.animations.empty()) {
        sprite.animations.push_back(assets::SpriteAnimation{.name = "idle", .loop = true});
    }
    if (sprite.default_animation.empty()) {
        sprite.default_animation = sprite.animations.front().name;
    }
    for (auto& animation : sprite.animations) {
        if (animation.name.empty()) {
            animation.name = "animation";
        }
        if (animation.frames.empty()) {
            animation.frames.push_back(assets::SpriteFrame{.texture = default_texture, .duration = 0.12f});
        }
    }
}

bool IsBuiltinTriggerScript(const std::string& script) {
    return script.empty() || script == "builtin" || script == "scripts/triggers.py";
}

bool Vec2Near(glm::vec2 lhs, glm::vec2 rhs) {
    return std::abs(lhs.x - rhs.x) < 0.001f && std::abs(lhs.y - rhs.y) < 0.001f;
}

void MergeJsonDefaults(nlohmann::json& destination, const nlohmann::json& defaults) {
    if (!defaults.is_object()) {
        return;
    }
    if (!destination.is_object()) {
        destination = nlohmann::json::object();
    }
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!destination.contains(it.key())) {
            destination[it.key()] = it.value();
        }
    }
}

struct ObjectPresetOption {
    const char* value;
    const char* label;
};

constexpr std::array<ObjectPresetOption, 7> kObjectPresetOptions{{
    {"none", "None"},
    {"player", "Player"},
    {"geometry_dash", "Geometry Dash"},
    {"enemy", "Enemy"},
    {"friend", "Friend"},
    {"vendor", "Vendor"},
    {"storage", "Storage"},
}};

struct ParticlePresetOption {
    const char* value;
    const char* label;
};

constexpr std::array<ParticlePresetOption, 7> kParticlePresetOptions{{
    {"custom", "Custom"},
    {"fire_loop", "Fire Loop"},
    {"flash_burst", "Flash Burst"},
    {"sparks", "Sparks"},
    {"smoke", "Smoke"},
    {"magic_glow", "Magic Glow"},
    {"embers", "Embers"},
}};

std::string NormalizeParticlePreset(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    for (const auto& option : kParticlePresetOptions) {
        if (value == option.value) {
            return value;
        }
    }
    return "custom";
}

int ParticlePresetIndex(std::string_view preset) {
    for (int index = 0; index < static_cast<int>(kParticlePresetOptions.size()); ++index) {
        if (preset == kParticlePresetOptions[static_cast<std::size_t>(index)].value) {
            return index;
        }
    }
    return 0;
}

void ApplyParticlePresetDefaults(assets::ParticleEffectAsset& particle) {
    particle.preset = NormalizeParticlePreset(particle.preset);
    if (particle.name.empty()) {
        particle.name = "particle";
    }
    if (particle.preset == "custom") {
        return;
    }
    if (particle.preset == "fire_loop") {
        particle.start_color = {1.0f, 0.74f, 0.28f, 0.95f};
        particle.end_color = {0.40f, 0.06f, 0.02f, 0.0f};
        particle.acceleration = {0.0f, -42.0f};
        particle.start_size = {26.0f, 26.0f};
        particle.end_size = {9.0f, 9.0f};
        particle.velocity_angle_min = -112.0f;
        particle.velocity_angle_max = -68.0f;
        particle.speed_min = 32.0f;
        particle.speed_max = 120.0f;
        particle.lifetime_min = 0.40f;
        particle.lifetime_max = 0.95f;
        particle.spawn_rate = 42.0f;
        particle.burst_count = 22;
        particle.emission_radius = 10.0f;
        particle.drag = 0.8f;
        particle.angular_velocity_min = -42.0f;
        particle.angular_velocity_max = 42.0f;
        particle.loop = true;
        particle.additive = true;
        particle.align_to_velocity = false;
    } else if (particle.preset == "flash_burst") {
        particle.start_color = {1.0f, 0.96f, 0.78f, 1.0f};
        particle.end_color = {1.0f, 0.72f, 0.18f, 0.0f};
        particle.acceleration = {0.0f, 0.0f};
        particle.start_size = {44.0f, 44.0f};
        particle.end_size = {10.0f, 10.0f};
        particle.velocity_angle_min = -180.0f;
        particle.velocity_angle_max = 180.0f;
        particle.speed_min = 110.0f;
        particle.speed_max = 260.0f;
        particle.lifetime_min = 0.12f;
        particle.lifetime_max = 0.28f;
        particle.spawn_rate = 0.0f;
        particle.burst_count = 34;
        particle.emission_radius = 4.0f;
        particle.drag = 2.4f;
        particle.angular_velocity_min = -180.0f;
        particle.angular_velocity_max = 180.0f;
        particle.loop = false;
        particle.additive = true;
        particle.align_to_velocity = false;
    } else if (particle.preset == "sparks") {
        particle.start_color = {1.0f, 0.92f, 0.56f, 0.96f};
        particle.end_color = {1.0f, 0.38f, 0.12f, 0.0f};
        particle.acceleration = {0.0f, 210.0f};
        particle.start_size = {16.0f, 5.0f};
        particle.end_size = {8.0f, 2.0f};
        particle.velocity_angle_min = -165.0f;
        particle.velocity_angle_max = -15.0f;
        particle.speed_min = 90.0f;
        particle.speed_max = 280.0f;
        particle.lifetime_min = 0.18f;
        particle.lifetime_max = 0.48f;
        particle.spawn_rate = 0.0f;
        particle.burst_count = 24;
        particle.emission_radius = 2.0f;
        particle.drag = 1.4f;
        particle.angular_velocity_min = -320.0f;
        particle.angular_velocity_max = 320.0f;
        particle.loop = false;
        particle.additive = true;
        particle.align_to_velocity = true;
    } else if (particle.preset == "smoke") {
        particle.start_color = {0.82f, 0.82f, 0.86f, 0.34f};
        particle.end_color = {0.10f, 0.10f, 0.12f, 0.0f};
        particle.acceleration = {0.0f, -10.0f};
        particle.start_size = {26.0f, 26.0f};
        particle.end_size = {58.0f, 58.0f};
        particle.velocity_angle_min = -112.0f;
        particle.velocity_angle_max = -68.0f;
        particle.speed_min = 12.0f;
        particle.speed_max = 38.0f;
        particle.lifetime_min = 0.85f;
        particle.lifetime_max = 1.80f;
        particle.spawn_rate = 18.0f;
        particle.burst_count = 12;
        particle.emission_radius = 12.0f;
        particle.drag = 0.3f;
        particle.angular_velocity_min = -26.0f;
        particle.angular_velocity_max = 26.0f;
        particle.loop = true;
        particle.additive = false;
        particle.align_to_velocity = false;
    } else if (particle.preset == "magic_glow") {
        particle.start_color = {0.42f, 0.78f, 1.0f, 0.95f};
        particle.end_color = {0.14f, 0.22f, 0.72f, 0.0f};
        particle.acceleration = {0.0f, -12.0f};
        particle.start_size = {18.0f, 18.0f};
        particle.end_size = {6.0f, 6.0f};
        particle.velocity_angle_min = -180.0f;
        particle.velocity_angle_max = 180.0f;
        particle.speed_min = 12.0f;
        particle.speed_max = 68.0f;
        particle.lifetime_min = 0.45f;
        particle.lifetime_max = 1.10f;
        particle.spawn_rate = 22.0f;
        particle.burst_count = 18;
        particle.emission_radius = 14.0f;
        particle.drag = 0.8f;
        particle.angular_velocity_min = -90.0f;
        particle.angular_velocity_max = 90.0f;
        particle.loop = true;
        particle.additive = true;
        particle.align_to_velocity = false;
    } else if (particle.preset == "embers") {
        particle.start_color = {1.0f, 0.54f, 0.18f, 0.92f};
        particle.end_color = {0.40f, 0.06f, 0.02f, 0.0f};
        particle.acceleration = {0.0f, -18.0f};
        particle.start_size = {12.0f, 12.0f};
        particle.end_size = {4.0f, 4.0f};
        particle.velocity_angle_min = -128.0f;
        particle.velocity_angle_max = -52.0f;
        particle.speed_min = 18.0f;
        particle.speed_max = 84.0f;
        particle.lifetime_min = 0.65f;
        particle.lifetime_max = 1.40f;
        particle.spawn_rate = 16.0f;
        particle.burst_count = 14;
        particle.emission_radius = 18.0f;
        particle.drag = 0.45f;
        particle.angular_velocity_min = -60.0f;
        particle.angular_velocity_max = 60.0f;
        particle.loop = true;
        particle.additive = true;
        particle.align_to_velocity = false;
    }
}

bool IsParticleEmitterEntity(const assets::EntityDefinition& entity) {
    if (entity.archetype == "particle_emitter") {
        return true;
    }
    if (entity.properties.is_object()) {
        return entity.properties.value("particle_emitter", false);
    }
    return false;
}

std::string ParticleAssetPath(const assets::EntityDefinition& entity) {
    if (entity.properties.is_object()) {
        return entity.properties.value("particle_asset", std::string{});
    }
    return {};
}

std::string NormalizeObjectPreset(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    for (const auto& option : kObjectPresetOptions) {
        if (value == option.value) {
            return value;
        }
    }
    return "none";
}

int ObjectPresetIndex(std::string_view preset) {
    for (int index = 0; index < static_cast<int>(kObjectPresetOptions.size()); ++index) {
        if (preset == kObjectPresetOptions[static_cast<std::size_t>(index)].value) {
            return index;
        }
    }
    return 0;
}

const char* PresetScriptPath(std::string_view preset) {
    if (preset == "player") {
        return "scripts/preset_player.py";
    }
    if (preset == "geometry_dash") {
        return "scripts/preset_geometry_dash.py";
    }
    if (preset == "enemy") {
        return "scripts/preset_enemy.py";
    }
    if (preset == "friend") {
        return "scripts/preset_friend.py";
    }
    if (preset == "vendor") {
        return "scripts/preset_vendor.py";
    }
    if (preset == "storage") {
        return "scripts/preset_storage.py";
    }
    return "";
}

void ClearPresetDerivedProperties(nlohmann::json& properties) {
    if (!properties.is_object()) {
        properties = nlohmann::json::object();
        return;
    }
    static const std::array<const char*, 27> keys{
        "preset",
        "role",
        "faction",
        "interaction_type",
        "is_player",
        "is_hostile",
        "is_friendly",
        "can_trade",
        "can_store_items",
        "move_speed",
        "jump_speed",
        "patrol_speed",
        "display_name",
        "dialogue",
        "shop_id",
        "storage_id",
        "capacity",
        "interaction_prompt",
        "gd_auto_forward",
        "gd_direction",
        "gd_rotation_speed",
        "gd_snap_rotation",
        "gd_snap_increment",
        "landing_sound",
        "landing_particle_asset",
        "landing_particle_count",
        "landing_particle_offset_y"
    };
    for (const char* key : keys) {
        properties.erase(key);
    }
}

void ApplyObjectPresetDefaults(assets::ObjectAsset& object) {
    object.preset = NormalizeObjectPreset(object.preset);
    ClearPresetDerivedProperties(object.properties);

    if (object.script == "scripts/preset_player.py" ||
        object.script == "scripts/preset_geometry_dash.py" ||
        object.script == "scripts/preset_enemy.py" ||
        object.script == "scripts/preset_friend.py" ||
        object.script == "scripts/preset_vendor.py" ||
        object.script == "scripts/preset_storage.py") {
        object.script.clear();
    }

    if (object.preset == "none") {
        return;
    }

    object.default_animation = object.default_animation.empty() ? "idle" : object.default_animation;
    object.collider_offset = {0.0f, 0.0f};
    object.collider_size = {
        std::max(object.size.x, 1.0f),
        std::max(object.size.y, 1.0f)
    };

    object.properties["preset"] = object.preset;
    object.properties["role"] = object.preset;
    object.script = PresetScriptPath(object.preset);

    if (object.preset == "player") {
        object.dynamic = true;
        object.collidable = true;
        object.properties["faction"] = "player";
        object.properties["is_player"] = true;
        object.properties["move_speed"] = 260.0f;
        object.properties["jump_speed"] = -680.0f;
    } else if (object.preset == "geometry_dash") {
        object.dynamic = true;
        object.collidable = true;
        object.properties["faction"] = "player";
        object.properties["is_player"] = true;
        object.properties["move_speed"] = 430.0f;
        object.properties["jump_speed"] = -760.0f;
        object.properties["gd_auto_forward"] = true;
        object.properties["gd_direction"] = 1.0f;
        object.properties["gd_rotation_speed"] = 900.0f;
        object.properties["gd_snap_rotation"] = true;
        object.properties["gd_snap_increment"] = 90.0f;
        object.properties["landing_particle_asset"] = "resources/particles/sparks.nparticle";
        object.properties["landing_particle_count"] = 18;
        object.properties["landing_particle_offset_y"] = -2.0f;
    } else if (object.preset == "enemy") {
        object.dynamic = true;
        object.collidable = true;
        object.properties["faction"] = "hostile";
        object.properties["is_hostile"] = true;
        object.properties["patrol_speed"] = 90.0f;
        object.properties["interaction_type"] = "combat";
    } else if (object.preset == "friend") {
        object.dynamic = false;
        object.collidable = true;
        object.properties["faction"] = "friendly";
        object.properties["is_friendly"] = true;
        object.properties["interaction_type"] = "dialogue";
        object.properties["display_name"] = object.name;
        object.properties["dialogue"] = "Hello there.";
        object.properties["interaction_prompt"] = "Talk";
    } else if (object.preset == "vendor") {
        object.dynamic = false;
        object.collidable = true;
        object.properties["faction"] = "neutral";
        object.properties["can_trade"] = true;
        object.properties["interaction_type"] = "vendor";
        object.properties["display_name"] = object.name;
        object.properties["shop_id"] = "default_shop";
        object.properties["interaction_prompt"] = "Trade";
    } else if (object.preset == "storage") {
        object.dynamic = false;
        object.collidable = true;
        object.properties["faction"] = "neutral";
        object.properties["can_store_items"] = true;
        object.properties["interaction_type"] = "storage";
        object.properties["storage_id"] = "default_storage";
        object.properties["capacity"] = 24;
        object.properties["interaction_prompt"] = "Open";
    }
}

void ApplyObjectAssetToEntity(assets::EntityDefinition& entity, const assets::ObjectAsset& object, const std::string& object_path) {
    entity.object_asset = object_path;
    entity.archetype = object.preset != "none" ? object.preset : object.name;
    entity.sprite_asset = object.sprite;
    entity.animation = object.default_animation;
    entity.size = object.size;
    entity.tint = object.tint;
    entity.layer = object.layer;
    entity.dynamic = object.dynamic;
    entity.collidable = object.collidable;
    entity.reflection = object.reflection;
    entity.normal_map = object.normal_map;
    entity.height_map = object.height_map;
    entity.displacement_map = object.displacement_map;
    entity.relief_enabled = object.relief_enabled;
    entity.bump_strength = object.bump_strength;
    entity.relief_depth = object.relief_depth;
    entity.parallax_depth = object.parallax_depth;
    entity.relief_contrast = object.relief_contrast;
    entity.pseudo_3d = object.pseudo_3d;
    entity.collider_offset = object.collider_offset;
    entity.collider_size = object.collider_size;
    entity.pseudo_3d_height = object.pseudo_3d_height;
    entity.sound = object.sound;
    entity.script = object.script;
    entity.properties = object.properties.is_object() ? object.properties : nlohmann::json::object();
}

struct ShaderDesignerState {
    std::string effect_name = "cinematic_stack";
    bool grayscale = false;
    float grayscale_amount = 1.0f;
    float vignette = 0.35f;
    float grain = 0.04f;
    float scanlines = 0.0f;
    float chromatic_aberration = 0.0f;
    float wave = 0.0f;
    float wave_frequency = 12.0f;
    float sharpen = 0.0f;
    int posterize_steps = 0;
    float brightness = 1.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    glm::vec3 tint{1.0f, 1.0f, 1.0f};
};

std::string FloatLiteral(float value, int precision = 4) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

nlohmann::json ShaderDesignerToJson(const ShaderDesignerState& state) {
    return {
        {"effect_name", state.effect_name},
        {"grayscale", state.grayscale},
        {"grayscale_amount", state.grayscale_amount},
        {"vignette", state.vignette},
        {"grain", state.grain},
        {"scanlines", state.scanlines},
        {"chromatic_aberration", state.chromatic_aberration},
        {"wave", state.wave},
        {"wave_frequency", state.wave_frequency},
        {"sharpen", state.sharpen},
        {"posterize_steps", state.posterize_steps},
        {"brightness", state.brightness},
        {"contrast", state.contrast},
        {"saturation", state.saturation},
        {"tint", nlohmann::json::array({state.tint.x, state.tint.y, state.tint.z})}
    };
}

void ShaderDesignerFromJson(const nlohmann::json& root, ShaderDesignerState& state) {
    state.effect_name = root.value("effect_name", state.effect_name);
    state.grayscale = root.value("grayscale", state.grayscale);
    state.grayscale_amount = root.value("grayscale_amount", state.grayscale_amount);
    state.vignette = root.value("vignette", state.vignette);
    state.grain = root.value("grain", state.grain);
    state.scanlines = root.value("scanlines", state.scanlines);
    state.chromatic_aberration = root.value("chromatic_aberration", state.chromatic_aberration);
    state.wave = root.value("wave", state.wave);
    state.wave_frequency = root.value("wave_frequency", state.wave_frequency);
    state.sharpen = root.value("sharpen", state.sharpen);
    state.posterize_steps = root.value("posterize_steps", state.posterize_steps);
    state.brightness = root.value("brightness", state.brightness);
    state.contrast = root.value("contrast", state.contrast);
    state.saturation = root.value("saturation", state.saturation);
    const auto tint = root.value("tint", nlohmann::json::array({1.0f, 1.0f, 1.0f}));
    if (tint.is_array() && tint.size() >= 3) {
        state.tint = {tint[0].get<float>(), tint[1].get<float>(), tint[2].get<float>()};
    }
}

std::string BuildShaderDesignerFragment(const ShaderDesignerState& state) {
    std::ostringstream shader;
    shader << R"glsl(#version 460 core
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform vec2 u_texelSize;
uniform float u_time;

float noise_hash(vec2 value) {
    return fract(sin(dot(value, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 apply_saturation(vec3 color, float saturation) {
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(luminance), color, saturation);
}

void main() {
    vec2 uv = v_uv;
)glsl";
    if (state.wave > 0.0001f) {
        shader << "    uv.y += sin(uv.x * " << FloatLiteral(state.wave_frequency) << " + u_time * 1.8) * " << FloatLiteral(state.wave) << ";\n";
    }
    shader << R"glsl(
    vec4 base_sample = texture(u_scene, uv);
    vec3 color = base_sample.rgb;
)glsl";
    if (state.chromatic_aberration > 0.0001f) {
        shader << "    color.r = texture(u_scene, uv + vec2(" << FloatLiteral(state.chromatic_aberration) << ", 0.0)).r;\n";
        shader << "    color.b = texture(u_scene, uv - vec2(" << FloatLiteral(state.chromatic_aberration) << ", 0.0)).b;\n";
    }
    if (state.sharpen > 0.0001f) {
        shader << R"glsl(
    vec3 blur = (
        texture(u_scene, uv + vec2(u_texelSize.x, 0.0)).rgb +
        texture(u_scene, uv - vec2(u_texelSize.x, 0.0)).rgb +
        texture(u_scene, uv + vec2(0.0, u_texelSize.y)).rgb +
        texture(u_scene, uv - vec2(0.0, u_texelSize.y)).rgb
    ) * 0.25;
)glsl";
        shader << "    color = mix(color, color + (color - blur), " << FloatLiteral(state.sharpen) << ");\n";
    }
    if (state.grayscale) {
        shader << "    color = mix(color, vec3(dot(color, vec3(0.299, 0.587, 0.114))), " << FloatLiteral(state.grayscale_amount) << ");\n";
    }
    if (state.posterize_steps > 1) {
        shader << "    color = floor(color * " << state.posterize_steps << ".0) / " << state.posterize_steps << ".0;\n";
    }
    shader << "    color = (color - 0.5) * " << FloatLiteral(state.contrast) << " + 0.5;\n";
    shader << "    color *= " << FloatLiteral(state.brightness) << ";\n";
    shader << "    color = apply_saturation(color, " << FloatLiteral(state.saturation) << ");\n";
    shader << "    color *= vec3(" << FloatLiteral(state.tint.x) << ", " << FloatLiteral(state.tint.y) << ", " << FloatLiteral(state.tint.z) << ");\n";
    if (state.scanlines > 0.0001f) {
        shader << "    color *= mix(1.0, 0.55 + 0.45 * sin((uv.y + u_time * 0.12) * 900.0), " << FloatLiteral(state.scanlines) << ");\n";
    }
    if (state.grain > 0.0001f) {
        shader << "    color += (noise_hash(uv * vec2(1920.0, 1080.0) + u_time * 18.0) - 0.5) * " << FloatLiteral(state.grain) << ";\n";
    }
    if (state.vignette > 0.0001f) {
        shader << "    vec2 vignette_uv = uv - 0.5;\n";
        shader << "    float vignette = clamp(1.0 - dot(vignette_uv, vignette_uv) * " << FloatLiteral(state.vignette * 3.5f) << ", 0.0, 1.0);\n";
        shader << "    color *= vignette;\n";
    }
    shader << R"glsl(
    frag_color = vec4(clamp(color, 0.0, 1.0), base_sample.a);
}
)glsl";
    return shader.str();
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::vector<std::string> TokenizeWords(const std::string& value) {
    std::vector<std::string> tokens;
    std::string current;
    for (char character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

bool ContainsAnyToken(const std::string& searchable, std::initializer_list<std::string_view> tokens) {
    return std::any_of(tokens.begin(), tokens.end(), [&](std::string_view token) {
        return searchable.find(token) != std::string::npos;
    });
}

float ScorePromptTokens(const std::string& searchable, const std::vector<std::string>& tokens) {
    float score = 0.0f;
    for (const std::string& token : tokens) {
        if (token.size() >= 3 && searchable.find(token) != std::string::npos) {
            score += 1.0f;
        }
    }
    return score;
}

struct AiObjectCandidate {
    std::string path;
    assets::ObjectAsset asset;
    std::string searchable;
};

struct AiTextureCandidate {
    std::string path;
    glm::vec2 size{32.0f, 32.0f};
    std::string searchable;
};

struct AiAudioCandidate {
    std::string path;
    std::string searchable;
};

template <typename T>
const T* RandomChoice(const std::vector<const T*>& items, std::mt19937& rng) {
    if (items.empty()) {
        return nullptr;
    }
    std::uniform_int_distribution<std::size_t> distribution(0, items.size() - 1);
    return items[distribution(rng)];
}

nlohmann::json ParseTriggerArgs(const std::string& args_json) {
    const auto parsed = nlohmann::json::parse(args_json, nullptr, false);
    return parsed.is_discarded() ? nlohmann::json::object() : parsed;
}

std::string DumpTriggerArgs(const nlohmann::json& args) {
    return args.dump();
}

bool DrawBuiltinConditionEditor(assets::TriggerCondition& condition) {
    bool changed = false;
    static const std::array<const char*, 11> condition_kinds{
        "always_true",
        "require_side",
        "require_iso",
        "light_enabled",
        "entity_active",
        "entity_visible",
        "audio_source_enabled",
        "audio_pak_enabled",
        "random_chance",
        "time_since_start",
        "cooldown_ready"
    };

    int current_kind = 0;
    for (int i = 0; i < static_cast<int>(condition_kinds.size()); ++i) {
        if (condition.function == condition_kinds[static_cast<std::size_t>(i)]) {
            current_kind = i;
            break;
        }
    }

    if (ImGui::BeginCombo("Condition Type", condition_kinds[static_cast<std::size_t>(current_kind)])) {
        for (int i = 0; i < static_cast<int>(condition_kinds.size()); ++i) {
            const bool selected = current_kind == i;
            if (ImGui::Selectable(condition_kinds[static_cast<std::size_t>(i)], selected)) {
                current_kind = i;
                condition.function = condition_kinds[static_cast<std::size_t>(i)];
                condition.script = "builtin";
                condition.args_json = "{}";
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    auto args = ParseTriggerArgs(condition.args_json);
    if (condition.function == "light_enabled") {
        std::string light = args.value("light", "");
        bool enabled = args.value("enabled", true);
        changed |= EditString("Light", light);
        changed |= ImGui::Checkbox("Expected Enabled", &enabled);
        args["light"] = light;
        args["enabled"] = enabled;
    } else if (condition.function == "entity_active") {
        std::string entity = args.value("entity", "");
        bool active = args.value("active", true);
        changed |= EditString("Entity", entity);
        changed |= ImGui::Checkbox("Expected Active", &active);
        args["entity"] = entity;
        args["active"] = active;
    } else if (condition.function == "entity_visible") {
        std::string entity = args.value("entity", "");
        bool visible = args.value("visible", true);
        changed |= EditString("Entity", entity);
        changed |= ImGui::Checkbox("Expected Visible", &visible);
        args["entity"] = entity;
        args["visible"] = visible;
    } else if (condition.function == "audio_source_enabled") {
        std::string audio_source = args.value("audio_source", "");
        bool enabled = args.value("enabled", true);
        changed |= EditString("Audio Source", audio_source);
        changed |= ImGui::Checkbox("Expected Enabled", &enabled);
        args["audio_source"] = audio_source;
        args["enabled"] = enabled;
    } else if (condition.function == "audio_pak_enabled") {
        std::string audio_pak = args.value("audio_pak", "");
        bool enabled = args.value("enabled", true);
        changed |= EditString("Audio Pak", audio_pak);
        changed |= ImGui::Checkbox("Expected Enabled", &enabled);
        args["audio_pak"] = audio_pak;
        args["enabled"] = enabled;
    } else if (condition.function == "random_chance") {
        float chance = args.value("chance", 1.0f);
        changed |= ImGui::SliderFloat("Chance", &chance, 0.0f, 1.0f, "%.2f");
        args["chance"] = chance;
    } else if (condition.function == "time_since_start") {
        float seconds = args.value("seconds", 0.0f);
        changed |= ImGui::InputFloat("Seconds", &seconds);
        args["seconds"] = std::max(seconds, 0.0f);
    } else if (condition.function == "cooldown_ready") {
        std::string trigger = args.value("trigger", "");
        float seconds = args.value("seconds", 1.0f);
        changed |= EditString("Trigger Id/Name", trigger);
        changed |= ImGui::InputFloat("Cooldown Seconds", &seconds);
        args["trigger"] = trigger;
        args["seconds"] = std::max(seconds, 0.0f);
    }

    if (changed) {
        condition.args_json = DumpTriggerArgs(args);
        condition.script = "builtin";
    }
    return changed;
}

bool DrawBuiltinActionEditor(assets::TriggerAction& action) {
    bool changed = false;
    static const std::array<const char*, 24> action_kinds{
        "log_message",
        "play_sound",
        "play_music",
        "stop_audio",
        "toggle_camera",
        "set_camera_mode",
        "set_camera_zoom",
        "set_camera_follow",
        "activate_virtual_camera",
        "release_virtual_camera",
        "play_scene_animation",
        "stop_scene_animation",
        "play_particle_emitter",
        "stop_particle_emitter",
        "burst_particle_emitter",
        "set_entity_visible",
        "set_entity_active",
        "delete_entity",
        "set_light_enabled",
        "set_audio_source_enabled",
        "set_audio_pak_enabled",
        "set_trigger_enabled",
        "set_music_volume",
        "set_sound_volume"
    };

    int current_kind = 0;
    for (int i = 0; i < static_cast<int>(action_kinds.size()); ++i) {
        if (action.function == action_kinds[static_cast<std::size_t>(i)]) {
            current_kind = i;
            break;
        }
    }

    if (ImGui::BeginCombo("Action Type", action_kinds[static_cast<std::size_t>(current_kind)])) {
        for (int i = 0; i < static_cast<int>(action_kinds.size()); ++i) {
            const bool selected = current_kind == i;
            if (ImGui::Selectable(action_kinds[static_cast<std::size_t>(i)], selected)) {
                current_kind = i;
                action.function = action_kinds[static_cast<std::size_t>(i)];
                action.script = "builtin";
                action.args_json = "{}";
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    auto args = ParseTriggerArgs(action.args_json);
    float delay = args.value("delay", 0.0f);
    if (ImGui::InputFloat("Delay", &delay)) {
        args["delay"] = std::max(delay, 0.0f);
        changed = true;
    }
    if (action.function == "log_message") {
        std::string message = args.value("message", "Triggered");
        changed |= EditString("Message", message, 1024);
        args["message"] = message;
    } else if (action.function == "play_sound") {
        std::string sound = args.value("sound", "");
        changed |= EditString("Sound", sound);
        args["sound"] = sound;
    } else if (action.function == "play_music") {
        std::string music = args.value("music", "");
        bool loop = args.value("loop", true);
        changed |= EditString("Music", music);
        changed |= ImGui::Checkbox("Loop", &loop);
        args["music"] = music;
        args["loop"] = loop;
    } else if (action.function == "set_camera_mode") {
        std::string mode = args.value("mode", "side");
        int index = mode == "isometric" ? 1 : 0;
        if (ImGui::Combo("Mode", &index, "side\0isometric\0")) {
            mode = index == 1 ? "isometric" : "side";
            changed = true;
        }
        args["mode"] = mode;
    } else if (action.function == "set_camera_zoom") {
        float zoom = args.value("zoom", 1.0f);
        float speed = args.value("speed", 0.0f);
        changed |= ImGui::InputFloat("Zoom", &zoom);
        changed |= ImGui::InputFloat("Smooth Speed", &speed);
        args["zoom"] = zoom;
        args["speed"] = speed;
    } else if (action.function == "set_camera_follow") {
        bool enabled = args.value("enabled", true);
        changed |= ImGui::Checkbox("Follow Player", &enabled);
        args["enabled"] = enabled;
    } else if (action.function == "activate_virtual_camera") {
        std::string camera = args.value("camera", "");
        changed |= EditString("Camera Id/Name", camera);
        args["camera"] = camera;
    } else if (action.function == "play_scene_animation") {
        std::string animation = args.value("animation", "");
        bool restart = args.value("restart", true);
        changed |= EditString("Animation Id/Name", animation);
        changed |= ImGui::Checkbox("Restart", &restart);
        args["animation"] = animation;
        args["restart"] = restart;
    } else if (action.function == "stop_scene_animation") {
        std::string animation = args.value("animation", "");
        bool restore_state = args.value("restore_state", true);
        changed |= EditString("Animation Id/Name", animation);
        changed |= ImGui::Checkbox("Restore State", &restore_state);
        args["animation"] = animation;
        args["restore_state"] = restore_state;
    } else if (action.function == "play_particle_emitter") {
        std::string emitter = args.value("emitter", "");
        bool restart = args.value("restart", true);
        changed |= EditString("Emitter Id", emitter);
        changed |= ImGui::Checkbox("Restart", &restart);
        args["emitter"] = emitter;
        args["restart"] = restart;
    } else if (action.function == "stop_particle_emitter") {
        std::string emitter = args.value("emitter", "");
        changed |= EditString("Emitter Id", emitter);
        args["emitter"] = emitter;
    } else if (action.function == "burst_particle_emitter") {
        std::string emitter = args.value("emitter", "");
        int count = args.value("count", -1);
        changed |= EditString("Emitter Id", emitter);
        changed |= ImGui::InputInt("Burst Count", &count);
        args["emitter"] = emitter;
        args["count"] = count;
    } else if (action.function == "set_entity_visible") {
        std::string entity = args.value("entity", "");
        bool visible = args.value("visible", true);
        changed |= EditString("Entity", entity);
        changed |= ImGui::Checkbox("Visible", &visible);
        args["entity"] = entity;
        args["visible"] = visible;
    } else if (action.function == "set_entity_active") {
        std::string entity = args.value("entity", "");
        bool active = args.value("active", true);
        changed |= EditString("Entity", entity);
        changed |= ImGui::Checkbox("Active", &active);
        args["entity"] = entity;
        args["active"] = active;
    } else if (action.function == "delete_entity") {
        std::string entity = args.value("entity", "");
        changed |= EditString("Entity", entity);
        args["entity"] = entity;
    } else if (action.function == "set_light_enabled") {
        std::string light = args.value("light", "");
        bool enabled = args.value("enabled", true);
        changed |= EditString("Light", light);
        changed |= ImGui::Checkbox("Enabled", &enabled);
        args["light"] = light;
        args["enabled"] = enabled;
    } else if (action.function == "set_audio_source_enabled") {
        std::string audio_source = args.value("audio_source", "");
        bool enabled = args.value("enabled", true);
        changed |= EditString("Audio Source", audio_source);
        changed |= ImGui::Checkbox("Enabled", &enabled);
        args["audio_source"] = audio_source;
        args["enabled"] = enabled;
    } else if (action.function == "set_audio_pak_enabled") {
        std::string audio_pak = args.value("audio_pak", "");
        bool enabled = args.value("enabled", true);
        changed |= EditString("Audio Pak", audio_pak);
        changed |= ImGui::Checkbox("Enabled", &enabled);
        args["audio_pak"] = audio_pak;
        args["enabled"] = enabled;
    } else if (action.function == "set_trigger_enabled") {
        std::string trigger = args.value("trigger", "");
        bool enabled = args.value("enabled", true);
        changed |= EditString("Trigger Id/Name", trigger);
        changed |= ImGui::Checkbox("Enabled", &enabled);
        args["trigger"] = trigger;
        args["enabled"] = enabled;
    } else if (action.function == "set_music_volume") {
        float volume = args.value("volume", 1.0f);
        changed |= ImGui::SliderFloat("Music Volume", &volume, 0.0f, 1.0f, "%.2f");
        args["volume"] = std::clamp(volume, 0.0f, 1.0f);
    } else if (action.function == "set_sound_volume") {
        float volume = args.value("volume", 1.0f);
        changed |= ImGui::SliderFloat("Sound Volume", &volume, 0.0f, 1.0f, "%.2f");
        args["volume"] = std::clamp(volume, 0.0f, 1.0f);
    }

    if (changed) {
        action.args_json = DumpTriggerArgs(args);
        action.script = "builtin";
    }
    return changed;
}

void DrawTriggerLogicEditor(std::vector<assets::TriggerCondition>& conditions, std::vector<assets::TriggerAction>& actions, bool& changed) {
    if (ImGui::Button("Add Condition")) {
        conditions.push_back({"builtin", "always_true", "{}"});
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Action")) {
        actions.push_back({"builtin", "log_message", "{\"message\":\"Triggered\"}"});
        changed = true;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Conditions");
    for (int i = 0; i < static_cast<int>(conditions.size()); ++i) {
        ImGui::PushID(i);
        auto& condition = conditions[i];
        bool custom = !IsBuiltinTriggerScript(condition.script);
        if (ImGui::Checkbox("Custom Script", &custom)) {
            condition.script = custom ? "scripts/custom_trigger.py" : "builtin";
            if (!custom && condition.function.empty()) {
                condition.function = "always_true";
            }
            changed = true;
        }
        if (custom) {
            changed |= EditString("Script", condition.script);
            changed |= EditString("Function", condition.function);
            changed |= EditJsonString("Args", condition.args_json);
        } else {
            changed |= DrawBuiltinConditionEditor(condition);
        }
        if (ImGui::Button("Remove Condition")) {
            conditions.erase(conditions.begin() + i);
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::TextUnformatted("Actions");
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        ImGui::PushID(1000 + i);
        auto& action = actions[i];
        bool custom = !IsBuiltinTriggerScript(action.script);
        if (ImGui::Checkbox("Custom Script", &custom)) {
            action.script = custom ? "scripts/custom_trigger.py" : "builtin";
            if (!custom && action.function.empty()) {
                action.function = "log_message";
            }
            changed = true;
        }
        if (custom) {
            changed |= EditString("Script", action.script);
            changed |= EditString("Function", action.function);
            changed |= EditJsonString("Args", action.args_json);
        } else {
            changed |= DrawBuiltinActionEditor(action);
        }
        if (ImGui::Button("Remove Action")) {
            actions.erase(actions.begin() + i);
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::Separator();
        ImGui::PopID();
    }
}

#if defined(_WIN32)
std::filesystem::path BrowseForFolderWindows(const std::wstring& title) {
    std::filesystem::path result;
    const HRESULT initialize_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool should_uninitialize = SUCCEEDED(initialize_result);

    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title.c_str());

        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    result = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (should_uninitialize) {
        CoUninitialize();
    }
    return result;
}

std::filesystem::path BrowseForFileWindows(const std::wstring& title) {
    std::filesystem::path result;
    const HRESULT initialize_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool should_uninitialize = SUCCEEDED(initialize_result);

    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title.c_str());

        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    result = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (should_uninitialize) {
        CoUninitialize();
    }
    return result;
}

void LaunchExecutableWindows(const std::filesystem::path& executable) {
    const std::wstring file = executable.wstring();
    const std::wstring directory = executable.parent_path().wstring();
    ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, directory.c_str(), SW_SHOWNORMAL);
}
#endif

}  // namespace

EditorApp::EditorApp(std::filesystem::path project_root)
    : Application({
          .title = "NovaIso Editor",
          .width = 1680,
          .height = 960,
          .enable_gui = true,
          .maximized = true,
          .vsync = true,
      }),
      project_root_(std::move(project_root)) {}

bool EditorApp::OnInit() {
    RefreshCodeEditorLanguage();
    python_editor_.SetShowWhitespaces(false);
    launcher_create_parent_ = ExecutableDirectory().string();
    launcher_open_path_ = ExecutableDirectory().string();
    LoadRecentProjects();
    RefreshProjectLauncherEntries();
    if (!project_root_.empty()) {
        TryOpenProject(project_root_);
    }
    return true;
}

void EditorApp::OnShutdown() {
    if (project_loaded_) {
        SaveAll();
    }
    if (gpu_time_queries_[0] != 0 || gpu_time_queries_[1] != 0) {
        glDeleteQueries(2, gpu_time_queries_);
        gpu_time_queries_[0] = 0;
        gpu_time_queries_[1] = 0;
    }
    post_stack_.Shutdown();
    renderer_.Shutdown();
    asset_manager_.Shutdown();
    scripting_.Shutdown();
}

void EditorApp::PushToast(std::string message, glm::vec4 color, float duration) {
    if (message.empty()) {
        return;
    }
    toast_notifications_.push_back({
        .message = std::move(message),
        .color = color,
        .created_at = elapsed_time_,
        .duration = duration
    });
    if (toast_notifications_.size() > 12) {
        toast_notifications_.erase(toast_notifications_.begin(), toast_notifications_.begin() + (toast_notifications_.size() - 12));
    }
}

void EditorApp::SyncEditorNotifications() {
    if (!status_message_.empty() && status_message_ != last_toasted_status_message_) {
        PushToast(status_message_, ToastColorForMessage(status_message_), 4.4f);
        last_toasted_status_message_ = status_message_;
    }

    for (const auto& notification : renderer::ShaderProgram::ConsumeNotifications()) {
        PushToast(notification.message, notification.success ? glm::vec4{0.24f, 0.82f, 0.56f, 1.0f} : glm::vec4{0.98f, 0.36f, 0.34f, 1.0f}, notification.success ? 3.5f : 6.0f);
        scene_.Log(std::string(notification.success ? "[Shader] " : "[Shader Error] ") + notification.message);
    }

    toast_notifications_.erase(std::remove_if(toast_notifications_.begin(), toast_notifications_.end(),
        [&](const ToastNotification& notification) {
            return elapsed_time_ - notification.created_at > notification.duration;
        }),
        toast_notifications_.end());
}

void EditorApp::DrawToastNotifications() {
    if (toast_notifications_.empty()) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const float margin = 18.0f;
    float offset_y = margin;
    for (auto it = toast_notifications_.rbegin(); it != toast_notifications_.rend(); ++it) {
        const float age = std::max(elapsed_time_ - it->created_at, 0.0f);
        const float fade = std::clamp(std::min(age / 0.18f, (it->duration - age) / 0.32f), 0.0f, 1.0f);
        if (fade <= 0.0f) {
            continue;
        }

        ImGui::SetNextWindowBgAlpha(0.86f * fade);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - offset_y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoMove;
        const std::string window_name = "ToastNotification##" + std::to_string(static_cast<std::size_t>(std::distance(toast_notifications_.rbegin(), it)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
        if (ImGui::Begin(window_name.c_str(), nullptr, flags)) {
            const ImVec2 min = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            const ImVec2 max{min.x + size.x, min.y + size.y};
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(min, max, IM_COL32(18, 24, 34, static_cast<int>(220.0f * fade)), 12.0f);
            draw_list->AddRectFilled(ImVec2(min.x, min.y), ImVec2(min.x + 4.0f, max.y), ToImColor(it->color, fade), 12.0f, ImDrawFlags_RoundCornersLeft);
            draw_list->AddRect(min, max, IM_COL32(120, 146, 178, static_cast<int>(90.0f * fade)), 12.0f);
            ImGui::PushTextWrapPos(min.x + 360.0f);
            ImGui::TextUnformatted(it->message.c_str());
            ImGui::PopTextWrapPos();
        }
        const float height = ImGui::GetWindowSize().y;
        ImGui::End();
        ImGui::PopStyleVar(2);
        offset_y += height + 10.0f;
    }
}

void EditorApp::OnUpdate(float delta_time) {
    const auto update_start = std::chrono::steady_clock::now();
    elapsed_time_ += delta_time;
    performance_.frame_ms = delta_time * 1000.0f;
    performance_.fps = delta_time > 0.00001f ? (1.0f / delta_time) : 0.0f;
    auto push_history = [](std::vector<float>& history, float value) {
        history.push_back(value);
        if (history.size() > 180) {
            history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - 180));
        }
    };
    push_history(performance_.frame_ms_history, performance_.frame_ms);
    push_history(performance_.fps_history, performance_.fps);

    SyncEditorNotifications();
    scene_.SetDebugTraceEnabled(preview_trace_enabled_);
    if (!project_loaded_) {
        update_cpu_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - update_start).count();
        performance_.update_ms = static_cast<float>(update_cpu_ms_);
        return;
    }

    if (GetInput().WasKeyPressed(SDL_SCANCODE_F11)) {
        ToggleFullscreen();
    }

    const bool ctrl_down = GetInput().IsKeyDown(SDL_SCANCODE_LCTRL) || GetInput().IsKeyDown(SDL_SCANCODE_RCTRL);

    if (ctrl_down && GetInput().WasKeyPressed(SDL_SCANCODE_S)) {
        SaveAll();
    }
    if (!ImGui::GetIO().WantTextInput && ctrl_down && GetInput().WasKeyPressed(SDL_SCANCODE_C)) {
        CopyHierarchySelection();
    }
    if (!ImGui::GetIO().WantTextInput && ctrl_down && GetInput().WasKeyPressed(SDL_SCANCODE_V)) {
        PasteHierarchySelection();
    }
    if (!ImGui::GetIO().WantTextInput && GetInput().WasKeyPressed(SDL_SCANCODE_F5)) {
        BuildAndRunGamePackage();
    }
    if (!ImGui::GetIO().WantTextInput && GetInput().WasKeyPressed(SDL_SCANCODE_F9)) {
        preview_paused_ = !preview_paused_;
        status_message_ = preview_paused_ ? "Preview paused." : "Preview resumed.";
    }
    if (!ImGui::GetIO().WantTextInput && GetInput().WasKeyPressed(SDL_SCANCODE_F10)) {
        preview_paused_ = true;
        preview_step_requested_ = true;
        status_message_ = "Preview step queued.";
    }
    if (!ImGui::GetIO().WantTextInput &&
        ctrl_down &&
        GetInput().WasKeyPressed(SDL_SCANCODE_Z) &&
        history_cursor_ > 0) {
        RestoreHistoryIndex(history_cursor_ - 1);
    }
    if (GetInput().WasKeyPressed(SDL_SCANCODE_TAB)) {
        scene_.ToggleCameraMode();
    }
    if (GetInput().WasKeyPressed(SDL_SCANCODE_R)) {
        scene_.ResetSimulation(false);
    }
    if (!ImGui::GetIO().WantTextInput && GetInput().WasKeyPressed(SDL_SCANCODE_DELETE)) {
        DeleteSelection();
    }

    if (ai_build_running_ && !ai_build_queue_.empty()) {
        ai_build_accumulator_ += delta_time * static_cast<float>(std::max(ai_builder_settings_.operations_per_second, 1));
        bool scene_changed = false;
        int safety_counter = 96;
        while (ai_build_cursor_ < ai_build_queue_.size() && ai_build_accumulator_ >= 1.0f && safety_counter-- > 0) {
            pending_history_label_ = "AI build level";
            ApplyAiBuildOperation(ai_build_queue_[ai_build_cursor_]);
            ++ai_build_cursor_;
            ai_build_accumulator_ -= 1.0f;
            scene_changed = true;
        }
        if (scene_changed) {
            scene_.ResetSimulation(false);
        }
        if (ai_build_cursor_ >= ai_build_queue_.size()) {
            ai_build_running_ = false;
            ai_build_accumulator_ = 0.0f;
            status_message_ = "AI builder finished: " + std::to_string(ai_build_queue_.size()) + " steps applied.";
        }
    }

    const bool can_preview = live_preview_ && viewport_mode_ == ViewportMode::Preview;
    if (can_preview) {
        if (preview_step_requested_) {
            scene_.Trace("Preview step");
            scene_.Update(std::clamp(delta_time, 1.0f / 240.0f, 1.0f / 30.0f), GetInput());
            preview_step_requested_ = false;
        } else if (!preview_paused_) {
            scene_.Update(delta_time, GetInput());
        }
    } else {
        preview_step_requested_ = false;
    }

#if defined(_WIN32)
    static WindowsPerformanceMonitor performance_monitor;
    static SystemPerformanceSample perf_sample{};
    static float perf_sample_accumulator = 1.0f;
    perf_sample_accumulator += delta_time;
    if (perf_sample_accumulator >= 0.25f || perf_sample.core_cpu_percent.empty()) {
        perf_sample = performance_monitor.Sample();
        perf_sample_accumulator = 0.0f;
    }
    performance_.system_cpu_percent = perf_sample.system_cpu_percent;
    performance_.process_cpu_percent = perf_sample.process_cpu_percent;
    performance_.process_cpu_core_equivalent_percent = perf_sample.process_cpu_core_equivalent_percent;
    performance_.core_cpu_usage = perf_sample.core_cpu_percent;
    performance_.ram_used_mb = perf_sample.ram_used_mb;
    performance_.ram_total_mb = perf_sample.ram_total_mb;
    performance_.process_ram_mb = perf_sample.process_ram_mb;
    performance_.vram_used_mb = perf_sample.vram_used_mb;
    performance_.vram_budget_mb = perf_sample.vram_budget_mb;
#endif

    SyncHistorySnapshot();
    update_cpu_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - update_start).count();
    performance_.update_ms = static_cast<float>(update_cpu_ms_);
}

void EditorApp::OnRender() {
    const auto render_start = std::chrono::steady_clock::now();
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    if (!project_loaded_) {
        renderer::Framebuffer::BindDefault(WindowSize());
        glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        render_cpu_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_start).count();
        performance_.render_ms = static_cast<float>(render_cpu_ms_);
        return;
    }

    const bool needs_scene_view = show_viewport_;
    if (!needs_scene_view) {
        performance_.scene_render_ms = 0.0f;
        performance_.post_process_ms = 0.0f;
        renderer::Framebuffer::BindDefault(WindowSize());
        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        render_cpu_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_start).count();
        performance_.render_ms = static_cast<float>(render_cpu_ms_);
        return;
    }

    if (gpu_time_queries_[0] == 0 && gpu_time_queries_[1] == 0) {
        glGenQueries(2, gpu_time_queries_);
    }
    const int resolve_query_index = 1 - gpu_time_query_index_;
    if (gpu_time_query_ready_[resolve_query_index] && gpu_time_queries_[resolve_query_index] != 0) {
        GLuint available = 0;
        glGetQueryObjectuiv(gpu_time_queries_[resolve_query_index], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available != 0) {
            GLuint64 elapsed_ns = 0;
            glGetQueryObjectui64v(gpu_time_queries_[resolve_query_index], GL_QUERY_RESULT, &elapsed_ns);
            performance_.gpu_frame_ms = static_cast<float>(elapsed_ns / 1000000.0);
        }
    }
    if (gpu_time_queries_[gpu_time_query_index_] != 0) {
        glBeginQuery(GL_TIME_ELAPSED, gpu_time_queries_[gpu_time_query_index_]);
    }

    scene_.Camera().SetViewport(glm::vec2(project_.game_viewport_size));
    glm::ivec2 target_render_size = scene_render_size_;
    if (viewport_image_size_.x > 64.0f && viewport_image_size_.y > 64.0f) {
        const float scale_x = viewport_image_size_.x / static_cast<float>(std::max(scene_render_size_.x, 1));
        const float scale_y = viewport_image_size_.y / static_cast<float>(std::max(scene_render_size_.y, 1));
        const float scale = std::clamp(std::min(scale_x, scale_y), 0.25f, 1.0f);
        target_render_size.x = std::max(static_cast<int>(std::round(static_cast<float>(scene_render_size_.x) * scale)), 320);
        target_render_size.y = std::max(static_cast<int>(std::round(static_cast<float>(scene_render_size_.y) * scale)), 180);
    }
    scene_target_.Resize(target_render_size);
    scene_target_.Bind();
    const auto clear = scene_.Level().clear_color;
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    renderer_.Begin(scene_.Camera());
    scene_.Render(renderer_, draw_debug_);
    renderer_.Flush();
    performance_.scene_render_ms = scene_.LastRenderTotalCpuMs();

    viewport_texture_ = post_stack_.Process(scene_target_.Texture(), scene_target_.Size(), scene_.Level().post_effects, elapsed_time_);
    performance_.post_process_ms = post_stack_.LastTotalCpuMs();
    renderer::Framebuffer::BindDefault(WindowSize());
    glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    if (gpu_time_queries_[gpu_time_query_index_] != 0) {
        glEndQuery(GL_TIME_ELAPSED);
        gpu_time_query_ready_[gpu_time_query_index_] = true;
        gpu_time_query_index_ = resolve_query_index;
    }

    render_cpu_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_start).count();
    performance_.render_ms = static_cast<float>(render_cpu_ms_);
}

void EditorApp::LoadProject(const std::filesystem::path& root) {
    SaveCurrentScript();
    SaveSelectedResource();
    if (project_loaded_) {
        asset_manager_.Shutdown();
        scripting_.Shutdown();
        renderer_.Shutdown();
        post_stack_.Shutdown();
    }

    project_root_ = NormalizeExistingPath(root);
    project_ = {};
    selection_kind_ = SelectionKind::None;
    selection_index_ = -1;
    ClearHierarchySelection();
    browser_selection_kind_ = BrowserSelectionKind::None;
    browser_selection_relative_.clear();
    current_script_relative_.clear();
    current_code_relative_.clear();
    InvalidateAssetBrowserCache();
    placement_kind_ = BrowserSelectionKind::None;
    placement_relative_.clear();
    script_dirty_ = false;
    resource_dirty_ = false;
    python_editor_.SetText("");
    EnsureProjectLayout();

    const auto project_path = project_root_ / "project.json";
    if (std::filesystem::exists(project_path)) {
        project_ = assets::LoadProject(project_path);
    } else {
        project_.name = project_root_.filename().string();
        project_.startup_level = "levels/main.niso";
        project_.levels = {project_.startup_level};
        project_.preview_image = "";
        assets::SaveProject(project_path, project_);
    }

    if (project_.levels.empty()) {
        project_.levels.push_back("levels/main.niso");
    }
    if (project_.startup_level.empty()) {
        project_.startup_level = project_.levels.front();
    }
    if (project_.supported_languages.empty()) {
        project_.supported_languages = {"en", "ru"};
    }
    project_.worker_threads = std::max(project_.worker_threads, 0);
    project_.game_viewport_size.x = std::max(project_.game_viewport_size.x, 320);
    project_.game_viewport_size.y = std::max(project_.game_viewport_size.y, 180);
    if (project_.splash_image.empty()) {
        if (std::filesystem::exists(project_root_ / "logo.png")) {
            project_.splash_image = "logo.png";
        } else if (std::filesystem::exists(ExecutableDirectory() / "logo.png")) {
            project_.splash_image = (ExecutableDirectory() / "logo.png").string();
        }
    }
    EnsureProjectLayout();
    EnsureLocalizationFiles();
    LoadEditorLocalization();
    if (!std::filesystem::exists(project_root_ / project_.startup_level)) {
        assets::LevelData level;
        level.name = "main";
        level.tile_layers.push_back({
            .name = "Ground",
            .width = 64,
            .height = 24,
            .depth = 0.0f,
            .visible = true,
            .collidable = true,
            .tiles = std::vector<int>(64 * 24, 0)
        });
        assets::SaveLevel(project_root_ / project_.startup_level, level);
    }

    renderer_.Initialize(ActiveShaderRoot());
    post_stack_.Initialize(ActiveShaderRoot());
    asset_manager_.Initialize(project_root_);
    asset_manager_.SetAudioEnabled(editor_audio_enabled_);
    core::ThreadPool::Shared().Configure(ResolveProjectWorkerCount(project_));
    scripting_.Initialize(project_root_);
    scene_render_size_ = project_.editor_render_size;
    scene_render_size_.x = std::max(scene_render_size_.x, 320);
    scene_render_size_.y = std::max(scene_render_size_.y, 180);
    const float aspect = static_cast<float>(std::max(project_.game_viewport_size.x, 1)) /
                         static_cast<float>(std::max(project_.game_viewport_size.y, 1));
    scene_render_size_.y = std::max(static_cast<int>(std::round(static_cast<float>(scene_render_size_.x) / aspect)), 180);

    scene_.Load(project_root_, project_, project_.startup_level, asset_manager_, scripting_);
    project_loaded_ = true;
    launcher_open_path_ = project_root_.string();
    launcher_create_parent_ = project_root_.parent_path().string();
    AddRecentProject(project_root_);
    RefreshProjectLauncherEntries();
    ApplyProjectPresentation();
    history_entries_.clear();
    history_cursor_ = -1;
    last_history_snapshot_.clear();
    RecordHistorySnapshot("Open project", true);
}

void EditorApp::EnsureProjectLayout() const {
    core::FileIO::EnsureDirectory(project_root_);
    core::FileIO::EnsureDirectory(project_root_ / "assets");
    core::FileIO::EnsureDirectory(project_root_ / "assets/images");
    core::FileIO::EnsureDirectory(project_root_ / "assets/audio");
    core::FileIO::EnsureDirectory(project_root_ / "scripts");
    core::FileIO::EnsureDirectory(project_root_ / "ui");
    core::FileIO::EnsureDirectory(project_root_ / "levels");
    core::FileIO::EnsureDirectory(project_root_ / "localization");
    core::FileIO::EnsureDirectory(project_root_ / "shaders");
    core::FileIO::EnsureDirectory(project_root_ / "shaders/designer");
    core::FileIO::EnsureDirectory(project_root_ / "resources/sprites");
    core::FileIO::EnsureDirectory(project_root_ / "resources/objects");
    core::FileIO::EnsureDirectory(project_root_ / "resources/triggers");
    core::FileIO::EnsureDirectory(project_root_ / "resources/animations");
    core::FileIO::EnsureDirectory(project_root_ / "resources/particles");
    core::FileIO::EnsureDirectory(project_root_ / project_.export_directory);
    EnsurePresetScripts();
    EnsureProjectShaders();
    EnsureParticleResources();
}

void EditorApp::EnsurePresetScripts() const {
    auto ensure_script = [&](const std::string& relative_path, const char* text, auto&& should_repair) {
        const std::filesystem::path full_path = project_root_ / relative_path;
        if (!std::filesystem::exists(full_path)) {
            core::FileIO::WriteText(full_path, text);
            return;
        }
        const std::string current = core::FileIO::ReadText(full_path);
        if (should_repair(current)) {
            core::FileIO::WriteText(full_path, text);
        }
    };

    ensure_script("scripts/preset_player.py", R"py(MOVE_SPEED = 260.0
JUMP_SPEED = -680.0


def on_spawn(entity, scene):
    entity.set_bool("is_player", True)


def on_update(entity, scene, dt):
    speed = entity.get_float("move_speed", MOVE_SPEED)
    jump_speed = entity.get_float("jump_speed", JUMP_SPEED)
    vx = 0.0
    if scene.action_down("MoveLeft"):
        vx -= speed
    if scene.action_down("MoveRight"):
        vx += speed

    _, vy = entity.velocity()
    if scene.action_down("Jump") and entity.grounded():
        vy = jump_speed

    entity.set_velocity(vx, vy)
)py", [](const std::string& current) {
        return current.empty() ||
               current.find("gd_auto_forward") != std::string::npos ||
               current.find("gd_rotation") != std::string::npos ||
               current.find("travel_sign") != std::string::npos ||
               current.find("MoveLeft") == std::string::npos ||
               current.find("MoveRight") == std::string::npos;
    });

    ensure_script("scripts/preset_geometry_dash.py", R"py(MOVE_SPEED = 430.0
JUMP_SPEED = -760.0
ROTATION_SPEED = 900.0
SNAP_INCREMENT = 90.0


def snap_angle(angle, increment):
    if increment <= 0.0:
        return angle
    return round(angle / increment) * increment


def on_spawn(entity, scene):
    entity.set_bool("is_player", True)
    entity.set_bool("gd_prev_grounded", entity.grounded())
    entity.set_float("gd_rotation", entity.rotation())


def on_update(entity, scene, dt):
    speed = entity.get_float("move_speed", MOVE_SPEED)
    jump_speed = entity.get_float("jump_speed", JUMP_SPEED)
    direction = entity.get_float("gd_direction", 1.0)
    auto_forward = entity.get_bool("gd_auto_forward", True)
    rotation_speed = entity.get_float("gd_rotation_speed", ROTATION_SPEED)
    snap_rotation = entity.get_bool("gd_snap_rotation", True)
    snap_increment = entity.get_float("gd_snap_increment", SNAP_INCREMENT)

    grounded = entity.grounded()
    was_grounded = entity.get_bool("gd_prev_grounded", grounded)
    rotation = entity.get_float("gd_rotation", entity.rotation())
    travel_sign = 1.0 if direction >= 0.0 else -1.0

    vx = 0.0
    if auto_forward:
        vx = speed * travel_sign
    else:
        if scene.action_down("MoveLeft"):
            vx -= speed
        if scene.action_down("MoveRight"):
            vx += speed

    _, vy = entity.velocity()
    if scene.action_down("Jump") and grounded:
        vy = jump_speed

    if grounded:
        if snap_rotation:
            rotation = snap_angle(rotation, snap_increment)
    else:
        rotation += rotation_speed * dt * travel_sign

    entity.set_velocity(vx, vy)
    entity.set_rotation(rotation)
    entity.set_float("gd_rotation", rotation)
    entity.set_bool("gd_prev_grounded", grounded)
)py", [](const std::string& current) {
        return current.empty() ||
               current.find("gd_auto_forward") == std::string::npos ||
               current.find("gd_rotation") == std::string::npos;
    });

    ensure_script("scripts/preset_enemy.py", R"py(def on_spawn(entity, scene):
    if entity.get_float("patrol_dir", 0.0) == 0.0:
        entity.set_float("patrol_dir", 1.0)
        x, _ = entity.position()
        entity.set_float("patrol_min_x", x - 96.0)
        entity.set_float("patrol_max_x", x + 96.0)


def on_update(entity, scene, dt):
    direction = entity.get_float("patrol_dir", 1.0)
    speed = entity.get_float("patrol_speed", 90.0)
    min_x = entity.get_float("patrol_min_x", entity.position()[0] - 96.0)
    max_x = entity.get_float("patrol_max_x", entity.position()[0] + 96.0)
    x, _ = entity.position()
    _, vy = entity.velocity()

    if x <= min_x:
        direction = 1.0
    elif x >= max_x:
        direction = -1.0

    entity.set_float("patrol_dir", direction)
    entity.set_velocity(direction * speed, vy)
)py", [](const std::string& current) { return current.empty(); });

    ensure_script("scripts/preset_friend.py", R"py(def on_spawn(entity, scene):
    entity.set_bool("is_friendly", True)


def on_update(entity, scene, dt):
    pass


def on_trigger(entity, scene, trigger_name):
    name = entity.get_string("display_name", entity.id())
    message = entity.get_string("dialogue", "Hello there.")
    scene.log(f"{name}: {message}")
)py", [](const std::string& current) { return current.empty(); });

    ensure_script("scripts/preset_vendor.py", R"py(def on_spawn(entity, scene):
    entity.set_bool("can_trade", True)


def on_update(entity, scene, dt):
    pass


def on_trigger(entity, scene, trigger_name):
    name = entity.get_string("display_name", entity.id())
    shop_id = entity.get_string("shop_id", "default_shop")
    scene.log(f"{name} opens shop: {shop_id}")
)py", [](const std::string& current) { return current.empty(); });

    ensure_script("scripts/preset_storage.py", R"py(def on_spawn(entity, scene):
    entity.set_bool("can_store_items", True)


def on_update(entity, scene, dt):
    pass


def on_trigger(entity, scene, trigger_name):
    storage_id = entity.get_string("storage_id", "default_storage")
    capacity = entity.get_float("capacity", 24.0)
    scene.log(f"Storage {storage_id} capacity: {int(capacity)}")
)py", [](const std::string& current) { return current.empty(); });
}

void EditorApp::EnsureProjectShaders() const {
    const std::filesystem::path source_root = ExecutableDirectory() / "shaders";
    const std::filesystem::path target_root = project_root_ / "shaders";
    if (!std::filesystem::exists(source_root)) {
        return;
    }

    const std::filesystem::path obsolete_reflection = target_root / "wet_reflection.frag";
    if (std::filesystem::exists(obsolete_reflection)) {
        std::filesystem::remove(obsolete_reflection);
    }

    for (const auto& entry : std::filesystem::directory_iterator(source_root)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::filesystem::path destination = target_root / entry.path().filename();
        if (!std::filesystem::exists(destination)) {
            std::filesystem::copy_file(entry.path(), destination, std::filesystem::copy_options::overwrite_existing);
        }
    }
}

void EditorApp::EnsureParticleResources() const {
    const std::filesystem::path particle_root = project_root_ / "resources/particles";
    const std::filesystem::path texture_root = project_root_ / "assets/images/generated";
    core::FileIO::EnsureDirectory(particle_root);
    core::FileIO::EnsureDirectory(texture_root);

    auto ensure_texture = [&](const std::string& relative_path, std::vector<std::uint8_t> pixels, int width, int height) {
        const std::filesystem::path full_path = project_root_ / relative_path;
        if (!std::filesystem::exists(full_path)) {
            WriteTga32(full_path, width, height, pixels);
        }
    };

    ensure_texture("assets/images/generated/particle_fire.tga",
        MakeRadialParticlePixels(64, 64, {1.0f, 0.78f, 0.38f, 0.98f}, {0.42f, 0.04f, 0.02f, 0.0f}, 1.7f),
        64, 64);
    ensure_texture("assets/images/generated/particle_flash.tga",
        MakeRadialParticlePixels(64, 64, {1.0f, 0.96f, 0.88f, 1.0f}, {1.0f, 0.72f, 0.22f, 0.0f}, 2.3f),
        64, 64);
    ensure_texture("assets/images/generated/particle_smoke.tga",
        MakeRadialParticlePixels(64, 64, {0.84f, 0.84f, 0.88f, 0.36f}, {0.05f, 0.05f, 0.06f, 0.0f}, 1.0f),
        64, 64);
    ensure_texture("assets/images/generated/particle_magic.tga",
        MakeRadialParticlePixels(64, 64, {0.68f, 0.92f, 1.0f, 0.95f}, {0.10f, 0.18f, 0.72f, 0.0f}, 1.9f),
        64, 64);
    ensure_texture("assets/images/generated/particle_spark.tga",
        MakeSparkParticlePixels(64, 64, {1.0f, 0.88f, 0.44f, 1.0f}),
        64, 64);

    auto ensure_particle_asset = [&](std::string relative_path, assets::ParticleEffectAsset particle) {
        ApplyParticlePresetDefaults(particle);
        const std::filesystem::path full_path = project_root_ / relative_path;
        if (!std::filesystem::exists(full_path)) {
            assets::SaveParticleEffectAsset(full_path, particle);
        }
    };

    ensure_particle_asset("resources/particles/fire_loop.nparticle", [] {
        assets::ParticleEffectAsset particle;
        particle.name = "fire_loop";
        particle.preset = "fire_loop";
        particle.texture = "assets/images/generated/particle_fire.tga";
        return particle;
    }());
    ensure_particle_asset("resources/particles/flash_burst.nparticle", [] {
        assets::ParticleEffectAsset particle;
        particle.name = "flash_burst";
        particle.preset = "flash_burst";
        particle.texture = "assets/images/generated/particle_flash.tga";
        return particle;
    }());
    ensure_particle_asset("resources/particles/sparks.nparticle", [] {
        assets::ParticleEffectAsset particle;
        particle.name = "sparks";
        particle.preset = "sparks";
        particle.texture = "assets/images/generated/particle_spark.tga";
        return particle;
    }());
    ensure_particle_asset("resources/particles/smoke.nparticle", [] {
        assets::ParticleEffectAsset particle;
        particle.name = "smoke";
        particle.preset = "smoke";
        particle.texture = "assets/images/generated/particle_smoke.tga";
        return particle;
    }());
    ensure_particle_asset("resources/particles/magic_glow.nparticle", [] {
        assets::ParticleEffectAsset particle;
        particle.name = "magic_glow";
        particle.preset = "magic_glow";
        particle.texture = "assets/images/generated/particle_magic.tga";
        return particle;
    }());
    ensure_particle_asset("resources/particles/embers.nparticle", [] {
        assets::ParticleEffectAsset particle;
        particle.name = "embers";
        particle.preset = "embers";
        particle.texture = "assets/images/generated/particle_fire.tga";
        return particle;
    }());
}

void EditorApp::CreateDefaultHtmlUiFiles() {
    const std::filesystem::path html_path = project_root_ / project_.html_ui.html_path;
    const std::filesystem::path css_path = project_root_ / project_.html_ui.css_path;
    const std::filesystem::path script_path = project_root_ / project_.html_ui.script_path;

    core::FileIO::WriteText(html_path, R"html(<ui-root>
  <screen id="main-menu" class="menu-screen" position="0.50 0.50">
    <panel id="main-panel" class="glass-panel hero" panel-size="820 0">
      <label class="eyebrow">${ui.brand}</label>
      <title>{project}</title>
      <subtitle>${ui.tagline}</subtitle>
      <row class="hero-metrics" size="0 104">
        <card class="metric-card" size="224 0">
          <label>${ui.metric_resolution}</label>
          <text>{setting_resolution}</text>
        </card>
        <card class="metric-card accent-green" size="224 0">
          <label>${ui.metric_mode}</label>
          <text>{camera_mode}</text>
        </card>
        <card class="metric-card warm" size="224 0">
          <label>${ui.metric_audio}</label>
          <text>{setting_master_volume}</text>
        </card>
      </row>
      <row class="hero-actions" size="0 64">
        <button class="primary hero-button" action="start_game">${ui.start_label}</button>
        <button class="secondary hero-button" action="open_settings">${ui.settings_label}</button>
        <button class="secondary hero-button" action="toggle_camera_mode">${ui.camera_label}</button>
        <button class="danger hero-button" action="quit_game">${ui.quit_label}</button>
      </row>
    </panel>
  </screen>

  <screen id="pause-menu" class="menu-screen" position="0.50 0.50">
    <panel class="glass-panel warm-panel" panel-size="760 0">
      <label class="eyebrow">${ui.pause_kicker}</label>
      <title>${ui.pause_title}</title>
      <subtitle>${ui.pause_subtitle}</subtitle>
      <row class="hero-actions" size="0 64">
        <button class="primary hero-button" action="resume_game">${ui.resume_label}</button>
        <button class="secondary hero-button" action="open_settings">${ui.settings_label}</button>
        <button class="secondary hero-button" action="restart_level">${ui.restart_label}</button>
      </row>
      <row class="hero-actions" size="0 64">
        <button class="secondary hero-button" action="open_main_menu">${ui.menu_label}</button>
        <button class="danger hero-button" action="quit_game">${ui.quit_label}</button>
      </row>
    </panel>
  </screen>

  <screen id="settings-menu" class="menu-screen" position="0.50 0.50">
    <panel class="glass-panel settings-panel" panel-size="920 0">
      <label class="eyebrow">${ui.settings_kicker}</label>
      <title>${ui.settings_title}</title>
      <subtitle>${ui.settings_subtitle}</subtitle>
      <divider></divider>

      <row class="setting-row" size="0 96">
        <stack class="setting-copy" size="520 0">
          <label>${ui.resolution_label}</label>
          <text>{setting_resolution}</text>
        </stack>
        <row class="setting-buttons" size="260 52">
          <button class="secondary compact" action="resolution_prev">${ui.prev_label}</button>
          <button class="secondary compact" action="resolution_next">${ui.next_label}</button>
        </row>
      </row>

      <row class="setting-row" size="0 96">
        <stack class="setting-copy" size="520 0">
          <label>${ui.fullscreen_label}</label>
          <text>{setting_fullscreen}</text>
        </stack>
        <row class="setting-buttons" size="260 52">
          <button class="secondary wide-button" action="toggle_fullscreen">${ui.toggle_fullscreen_label}</button>
        </row>
      </row>

      <row class="setting-row" size="0 96">
        <stack class="setting-copy" size="520 0">
          <label>${ui.master_label}</label>
          <text>{setting_master_volume}</text>
        </stack>
        <row class="setting-buttons" size="260 52">
          <button class="secondary compact" action="master_volume_down">${ui.minus_label}</button>
          <button class="secondary compact" action="master_volume_up">${ui.plus_label}</button>
        </row>
      </row>

      <row class="setting-row" size="0 96">
        <stack class="setting-copy" size="520 0">
          <label>${ui.music_label}</label>
          <text>{setting_music_volume}</text>
        </stack>
        <row class="setting-buttons" size="260 52">
          <button class="secondary compact" action="music_volume_down">${ui.minus_label}</button>
          <button class="secondary compact" action="music_volume_up">${ui.plus_label}</button>
        </row>
      </row>

      <row class="setting-row" size="0 96">
        <stack class="setting-copy" size="520 0">
          <label>${ui.effects_label}</label>
          <text>{setting_sound_volume}</text>
        </stack>
        <row class="setting-buttons" size="260 52">
          <button class="secondary compact" action="sound_volume_down">${ui.minus_label}</button>
          <button class="secondary compact" action="sound_volume_up">${ui.plus_label}</button>
        </row>
      </row>

      <row class="hero-actions footer-actions" size="0 64">
        <button class="primary footer-button" action="close_settings">${ui.back_label}</button>
      </row>
    </panel>
  </screen>

  <screen id="hud">
    <widget id="status" class="hud-card" anchor="0.03 0.03" size="360 92">
      <stack class="hud-copy">
        <label>${ui.hud_status}</label>
        <text>{project}</text>
      </stack>
    </widget>
    <widget id="camera" class="hud-card accent-green" anchor="0.76 0.03" size="300 92">
      <stack class="hud-copy">
        <label>${ui.hud_camera}</label>
        <text>{camera_mode}</text>
      </stack>
    </widget>
    <widget id="message" class="hud-card wide" anchor="0.03 0.88" size="620 94">
      <stack class="hud-copy">
        <label>${ui.hud_message}</label>
        <text>{last_message}</text>
      </stack>
    </widget>
  </screen>
</ui-root>
)html");

    core::FileIO::WriteText(css_path, R"css(:root {
  --overlay: rgba(4, 8, 16, 0.78);
  --panel: rgba(10, 18, 34, 0.94);
  --panel-warm: rgba(28, 18, 10, 0.95);
  --hud: rgba(7, 13, 24, 0.82);
  --card: rgba(14, 24, 44, 0.88);
  --row: rgba(12, 20, 36, 0.86);
  --accent: #43d8ff;
  --accent-green: #7dffbf;
  --accent-warm: #ffb356;
  --text: #f4fbff;
  --muted: #9db7d5;
  --danger: #ff6f8d;
}

screen { background: var(--overlay); }
panel { background: var(--panel); accent: var(--accent); title-color: var(--text); subtitle-color: var(--muted); padding: 28px; gap: 14px; radius: 30px; }
widget { background: var(--hud); accent: var(--accent); color: var(--text); padding: 16px; gap: 8px; radius: 20px; }
row { gap: 12px; padding: 0px; radius: 18px; }
stack { gap: 8px; padding: 0px; radius: 0px; }
card { background: var(--card); accent: var(--accent); padding: 16px; gap: 6px; radius: 20px; }
label { color: var(--muted); scale: 0.92; }
title { color: var(--text); scale: 1.78; }
subtitle { color: var(--muted); scale: 1.0; }
text { color: var(--text); scale: 1.06; }
button { background: rgba(23, 39, 62, 1.0); hover-background: rgba(43, 67, 99, 1.0); color: var(--text); width: 320px; height: 56px; radius: 18px; }
.primary { background: rgba(32, 103, 142, 1.0); hover-background: rgba(47, 136, 184, 1.0); }
.secondary { background: rgba(29, 43, 67, 1.0); hover-background: rgba(44, 63, 94, 1.0); }
.danger { background: rgba(120, 44, 63, 1.0); hover-background: rgba(159, 54, 80, 1.0); }
.warm-panel { background: var(--panel-warm); accent: var(--accent-warm); }
.accent-green { accent: var(--accent-green); }
.warm { accent: var(--accent-warm); }
.hero { gap: 16px; }
.hero-metrics { background: rgba(0, 0, 0, 0.0); padding: 0px; gap: 14px; }
.metric-card { background: rgba(15, 24, 46, 0.90); padding: 16px; gap: 6px; radius: 20px; }
.hero-actions { background: rgba(0, 0, 0, 0.0); padding: 0px; gap: 12px; }
.hero-button { width: 182px; height: 56px; }
.settings-panel { accent: #87c9ff; gap: 12px; }
.setting-row { background: var(--row); accent: rgba(135, 201, 255, 1.0); padding: 18px; gap: 14px; radius: 20px; }
.setting-copy { background: rgba(0, 0, 0, 0.0); padding: 0px; gap: 6px; }
.setting-buttons { background: rgba(0, 0, 0, 0.0); padding: 0px; gap: 10px; }
.compact { width: 86px; height: 48px; }
.wide-button { width: 240px; height: 48px; }
.footer-actions { gap: 0px; }
.footer-button { width: 220px; height: 54px; }
.hud-copy { background: rgba(0, 0, 0, 0.0); padding: 0px; gap: 6px; }
.wide { width: 620px; }
)css");

    core::FileIO::WriteText(script_path, R"js(window.ui.brand = "HTML UI";
window.ui.tagline = "NovaIso layered runtime interface";
window.ui.start_label = "Start Mission";
window.ui.settings_label = "Settings";
window.ui.camera_label = "Switch Camera";
window.ui.quit_label = "Exit";
window.ui.metric_resolution = "Resolution";
window.ui.metric_mode = "Camera";
window.ui.metric_audio = "Master";
window.ui.pause_kicker = "SYSTEM";
window.ui.pause_title = "Paused";
window.ui.pause_subtitle = "Flow is halted. Resume when ready.";
window.ui.settings_kicker = "SYSTEM";
window.ui.settings_title = "Settings";
window.ui.settings_subtitle = "Display, fullscreen and audio options.";
window.ui.resume_label = "Resume";
window.ui.restart_label = "Restart Level";
window.ui.menu_label = "Main Menu";
window.ui.resolution_label = "Resolution";
window.ui.fullscreen_label = "Fullscreen";
window.ui.toggle_fullscreen_label = "Toggle Fullscreen";
window.ui.master_label = "Master Volume";
window.ui.music_label = "Music Volume";
window.ui.effects_label = "Effects Volume";
window.ui.prev_label = "Prev";
window.ui.next_label = "Next";
window.ui.minus_label = "-";
window.ui.plus_label = "+";
window.ui.back_label = "Back";
window.ui.hud_status = "STATUS";
window.ui.hud_camera = "CAMERA";
window.ui.hud_message = "LAST EVENT";
)js");
    InvalidateAssetBrowserCache();
}

void EditorApp::SaveAll() {
    if (!project_loaded_) {
        return;
    }
    SaveCurrentScript();
    SaveSelectedResource();
    project_.camera_mode = scene_.CameraModeName();
    project_.game_viewport_size.x = std::max(project_.game_viewport_size.x, 320);
    project_.game_viewport_size.y = std::max(project_.game_viewport_size.y, 180);
    project_.editor_render_size = scene_render_size_;
    assets::SaveProject(project_root_ / "project.json", project_);
    scene_.SaveLevelToDisk();
    ApplyProjectPresentation();
}

void EditorApp::SyncSceneResources() {
    if (!project_loaded_) {
        return;
    }
    scene_.RefreshResources(project_, false);
}

void EditorApp::LoadRecentProjects() {
    recent_projects_.clear();
    const std::filesystem::path path = RecentProjectsPath();
    if (!std::filesystem::exists(path)) {
        if (std::filesystem::exists(ExecutableDirectory() / "demo_project" / "project.json")) {
            recent_projects_.push_back(NormalizeExistingPath(ExecutableDirectory() / "demo_project"));
        }
        return;
    }

    const auto root = nlohmann::json::parse(core::FileIO::ReadText(path), nullptr, false);
    if (!root.is_array()) {
        return;
    }

    for (const auto& value : root) {
        if (!value.is_string()) {
            continue;
        }
        const std::filesystem::path project_root = NormalizeExistingPath(value.get<std::string>());
        if (std::filesystem::exists(project_root / "project.json")) {
            recent_projects_.push_back(project_root);
        }
    }
}

void EditorApp::SaveRecentProjects() const {
    nlohmann::json root = nlohmann::json::array();
    for (const auto& path : recent_projects_) {
        root.push_back(path.string());
    }
    core::FileIO::WriteText(RecentProjectsPath(), root.dump(2));
}

void EditorApp::AddRecentProject(const std::filesystem::path& root) {
    const std::filesystem::path normalized = NormalizeExistingPath(root);
    recent_projects_.erase(std::remove(recent_projects_.begin(), recent_projects_.end(), normalized), recent_projects_.end());
    recent_projects_.insert(recent_projects_.begin(), normalized);
    if (recent_projects_.size() > 12) {
        recent_projects_.resize(12);
    }
    SaveRecentProjects();
}

void EditorApp::RefreshProjectLauncherEntries() {
    project_launcher_entries_.clear();
    for (const auto& root : recent_projects_) {
        const std::filesystem::path project_file = root / "project.json";
        if (!std::filesystem::exists(project_file)) {
            continue;
        }
        try {
            ProjectLauncherEntry entry;
            entry.root = root;
            entry.project = assets::LoadProject(project_file);
            project_launcher_entries_.push_back(std::move(entry));
        } catch (...) {
        }
    }
}

std::filesystem::path EditorApp::RecentProjectsPath() const {
    return ExecutableDirectory() / "recent_projects.json";
}

std::filesystem::path EditorApp::BrowseForFolder(const std::string& title) const {
#if defined(_WIN32)
    std::wstring wide_title(title.begin(), title.end());
    return BrowseForFolderWindows(wide_title);
#else
    (void)title;
    return {};
#endif
}

std::filesystem::path EditorApp::BrowseForFile(const std::string& title) const {
#if defined(_WIN32)
    std::wstring wide_title(title.begin(), title.end());
    return BrowseForFileWindows(wide_title);
#else
    (void)title;
    return {};
#endif
}

bool EditorApp::TryOpenProject(const std::filesystem::path& root) {
    const std::filesystem::path normalized = NormalizeExistingPath(root);
    if (!std::filesystem::exists(normalized / "project.json")) {
        status_message_ = "project.json not found in selected folder.";
        return false;
    }
    LoadProject(normalized);
    status_message_ = "Opened project: " + normalized.string();
    return true;
}

void EditorApp::CreateNewProject() {
    if (launcher_create_parent_.empty() || launcher_create_name_.empty()) {
        status_message_ = "Choose a parent folder and project name first.";
        return;
    }

    const std::filesystem::path parent = NormalizeExistingPath(launcher_create_parent_);
    const std::filesystem::path root = parent / SanitizeName(launcher_create_name_);
    if (std::filesystem::exists(root / "project.json")) {
        status_message_ = "Project already exists: " + root.string();
        TryOpenProject(root);
        return;
    }

    core::FileIO::EnsureDirectory(root);
    LoadProject(root);
    project_.name = launcher_create_name_;
    project_.html_ui.enabled = true;
    CreateDefaultHtmlUiFiles();
    assets::SaveProject(project_root_ / "project.json", project_);
    status_message_ = "Created project: " + root.string();
}

std::filesystem::path EditorApp::ExportPackagePath() const {
    const std::string safe_name = SanitizeName(project_.name);
    return project_root_ / project_.export_directory / safe_name / (safe_name + ".exe");
}

void EditorApp::BuildAndRunGamePackage() {
    if (!project_loaded_) {
        return;
    }
    ExportGamePackage();
    const std::filesystem::path executable = ExportPackagePath();
    if (!std::filesystem::exists(executable)) {
        status_message_ = "Built package executable not found: " + executable.string();
        return;
    }
    RunPackagedGame(executable);
}

void EditorApp::RunPackagedGame(const std::filesystem::path& executable) const {
#if defined(_WIN32)
    LaunchExecutableWindows(executable);
#else
    (void)executable;
#endif
}

void EditorApp::DrawProjectLauncher() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
    ImGui::Begin("Project Launcher###ProjectLauncher", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings);
    DecorateEditorWindow({0.34f, 0.64f, 0.98f, 1.0f});

    ImGui::TextUnformatted("NovaIso Project Launcher");
    ImGui::Separator();
    ImGui::TextWrapped("Open an existing project folder or create a new project before entering the editor.");

    ImGui::Columns(2, nullptr, true);

    ImGui::TextUnformatted("Recent Projects");
    ImGui::Separator();
    if (project_launcher_entries_.empty()) {
        ImGui::TextDisabled("No recent projects yet.");
    }
    for (auto& entry : project_launcher_entries_) {
        ImGui::PushID(entry.root.string().c_str());
        ImGui::BeginGroup();
        const std::string preview_relative = !entry.project.preview_image.empty() ? entry.project.preview_image : entry.project.icon;
        if (!entry.preview_loaded && !preview_relative.empty()) {
            entry.preview_texture.LoadFromFile(entry.root / preview_relative);
            entry.preview_loaded = true;
        }
        if (entry.preview_texture.Id() != 0) {
            ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(entry.preview_texture.Id())), ImVec2(160.0f, 90.0f), ImVec2(0, 1), ImVec2(1, 0));
        } else {
            ImGui::Button("NO PREVIEW", ImVec2(160.0f, 90.0f));
        }
        ImGui::TextUnformatted(entry.project.name.c_str());
        ImGui::TextDisabled("%s", entry.root.string().c_str());
        if (!entry.project.developer.empty() || !entry.project.version.empty()) {
            ImGui::TextDisabled("%s  %s", entry.project.developer.c_str(), entry.project.version.c_str());
        }
        if (ImGui::Button("Open Project", ImVec2(160.0f, 0.0f))) {
            TryOpenProject(entry.root);
        }
        ImGui::Separator();
        ImGui::EndGroup();
        ImGui::PopID();
    }

    ImGui::NextColumn();

    ImGui::TextUnformatted("Open Existing");
    EditString("Project Folder", launcher_open_path_, 1024);
    if (ImGui::Button("Browse Folder")) {
        const std::filesystem::path selected = BrowseForFolder("Select NovaIso project folder");
        if (!selected.empty()) {
            launcher_open_path_ = selected.string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        TryOpenProject(launcher_open_path_);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Create New Project");
    EditString("Parent Folder", launcher_create_parent_, 1024);
    if (ImGui::Button("Browse Parent")) {
        const std::filesystem::path selected = BrowseForFolder("Select parent folder for new NovaIso project");
        if (!selected.empty()) {
            launcher_create_parent_ = selected.string();
        }
    }
    EditString("Project Name", launcher_create_name_);
    if (ImGui::Button("Create Project")) {
        CreateNewProject();
    }

    if (!status_message_.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextWrapped("%s", status_message_.c_str());
    }

    ImGui::Columns(1);
    ImGui::End();
}

void EditorApp::LoadCurrentScript() {
    if (current_code_relative_.empty()) {
        python_editor_.SetText("");
        script_dirty_ = false;
        return;
    }

    const auto path = project_root_ / current_code_relative_;
    if (!std::filesystem::exists(path)) {
        core::FileIO::EnsureDirectory(path.parent_path());
        core::FileIO::WriteText(path, "");
    }
    RefreshCodeEditorLanguage();
    python_editor_.SetText(core::FileIO::ReadText(path));
    script_dirty_ = false;
}

void EditorApp::SaveCurrentScript() {
    if (!script_dirty_ || current_code_relative_.empty()) {
        return;
    }
    const auto path = project_root_ / current_code_relative_;
    core::FileIO::EnsureDirectory(path.parent_path());
    core::FileIO::WriteText(path, python_editor_.GetText());
    script_dirty_ = false;

    if (browser_selection_relative_ == current_code_relative_) {
        if (browser_selection_kind_ == BrowserSelectionKind::Sprite ||
            browser_selection_kind_ == BrowserSelectionKind::Object ||
            browser_selection_kind_ == BrowserSelectionKind::Trigger ||
            browser_selection_kind_ == BrowserSelectionKind::Animation ||
            browser_selection_kind_ == BrowserSelectionKind::Particle) {
            LoadSelectedResource();
            SyncSceneResources();
        } else if (std::filesystem::path(current_code_relative_).extension() == ".niso" &&
                   scene_.LevelPath() == path) {
            scene_.Load(project_root_, project_, std::filesystem::relative(scene_.LevelPath(), project_root_).generic_string(), asset_manager_, scripting_);
        } else if (std::filesystem::path(current_code_relative_).filename() == "project.json") {
            LoadProject(project_root_);
        }
    }
}

void EditorApp::OpenCodeEditorFile(const std::filesystem::path& path) {
    current_code_relative_ = path.generic_string();
    LoadCurrentScript();
}

void EditorApp::DeleteBrowserResource(BrowserSelectionKind kind, const std::string& relative_path) {
    if (relative_path.empty()) {
        return;
    }

    SaveCurrentScript();
    SaveSelectedResource();

    const std::filesystem::path full_path = project_root_ / relative_path;
    if (!std::filesystem::exists(full_path)) {
        return;
    }

    auto erase_from = [&](std::vector<std::string>& values) {
        values.erase(std::remove(values.begin(), values.end(), relative_path), values.end());
    };

    if (kind == BrowserSelectionKind::Sprite) {
        erase_from(project_.sprite_assets);
    } else if (kind == BrowserSelectionKind::Object) {
        erase_from(project_.object_assets);
    } else if (kind == BrowserSelectionKind::Trigger) {
        erase_from(project_.trigger_assets);
    } else if (kind == BrowserSelectionKind::Animation) {
        erase_from(project_.animation_assets);
    } else if (kind == BrowserSelectionKind::Particle) {
        erase_from(project_.particle_assets);
    } else if (std::filesystem::path(relative_path).extension() == ".niso") {
        erase_from(project_.levels);
        if (project_.startup_level == relative_path) {
            project_.startup_level = project_.levels.empty() ? "" : project_.levels.front();
        }
    } else if (std::filesystem::path(relative_path).extension() == ".py") {
        erase_from(project_.scripts);
    }

    std::error_code error;
    std::filesystem::remove(full_path, error);
    if (error) {
        status_message_ = "Failed to delete: " + relative_path;
        return;
    }

    if (project_.icon == relative_path) {
        project_.icon.clear();
    }
    if (kind == BrowserSelectionKind::Animation) {
        for (auto& animation : scene_.EditableLevel().animations) {
            if (animation.asset == relative_path) {
                animation.asset.clear();
            }
        }
    } else if (kind == BrowserSelectionKind::Particle) {
        for (auto& entity : scene_.EditableLevel().entities) {
            if (entity.properties.is_object() && entity.properties.value("particle_asset", std::string{}) == relative_path) {
                entity.properties["particle_asset"] = "";
            }
        }
    }
    if (current_script_relative_ == relative_path) {
        current_script_relative_.clear();
    }
    if (current_code_relative_ == relative_path) {
        current_code_relative_.clear();
        python_editor_.SetText("");
        script_dirty_ = false;
    }
    if (browser_selection_relative_ == relative_path) {
        browser_selection_relative_.clear();
        browser_selection_kind_ = BrowserSelectionKind::None;
    }
    if (placement_relative_ == relative_path) {
        placement_relative_.clear();
        placement_kind_ = BrowserSelectionKind::None;
    }

    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    SyncSceneResources();
    status_message_ = "Deleted: " + relative_path;
}

bool EditorApp::IsEditableTextFile(const std::filesystem::path& path) const {
    return IsEditableTextExtension(path);
}

void EditorApp::RefreshCodeEditorLanguage() {
    python_editor_.SetLanguageDefinition(LanguageDefinitionForPath(current_code_relative_.empty() ? std::filesystem::path("script.py") : std::filesystem::path(current_code_relative_)));
}

void EditorApp::LoadSelectedResource() {
    resource_dirty_ = false;
    sprite_animation_index_ = 0;
    sprite_frame_index_ = 0;
    pixel_canvas_ = {};

    if (browser_selection_relative_.empty()) {
        return;
    }

    const auto full_path = project_root_ / browser_selection_relative_;
    if (browser_selection_kind_ == BrowserSelectionKind::Sprite && std::filesystem::exists(full_path)) {
        selected_sprite_ = assets::LoadSpriteAsset(full_path);
        EnsureSpriteStructure(selected_sprite_, "assets/images/generated/blank_frame.tga");
        LoadSpriteCanvas();
    } else if (browser_selection_kind_ == BrowserSelectionKind::Object && std::filesystem::exists(full_path)) {
        selected_object_ = assets::LoadObjectAsset(full_path);
    } else if (browser_selection_kind_ == BrowserSelectionKind::Trigger && std::filesystem::exists(full_path)) {
        selected_trigger_ = assets::LoadTriggerAsset(full_path);
    } else if (browser_selection_kind_ == BrowserSelectionKind::Animation && std::filesystem::exists(full_path)) {
        selected_animation_ = assets::LoadObjectAnimationAsset(full_path);
    } else if (browser_selection_kind_ == BrowserSelectionKind::Particle && std::filesystem::exists(full_path)) {
        selected_particle_ = assets::LoadParticleEffectAsset(full_path);
    }
}

void EditorApp::SaveSelectedResource() {
    if (!resource_dirty_ || browser_selection_relative_.empty()) {
        return;
    }

    const auto full_path = project_root_ / browser_selection_relative_;
    if (browser_selection_kind_ == BrowserSelectionKind::Sprite) {
        SaveSpriteCanvas();
        assets::SaveSpriteAsset(full_path, selected_sprite_);
    } else if (browser_selection_kind_ == BrowserSelectionKind::Object) {
        assets::SaveObjectAsset(full_path, selected_object_);
    } else if (browser_selection_kind_ == BrowserSelectionKind::Trigger) {
        assets::SaveTriggerAsset(full_path, selected_trigger_);
    } else if (browser_selection_kind_ == BrowserSelectionKind::Animation) {
        assets::SaveObjectAnimationAsset(full_path, selected_animation_);
    } else if (browser_selection_kind_ == BrowserSelectionKind::Particle) {
        assets::SaveParticleEffectAsset(full_path, selected_particle_);
    }

    resource_dirty_ = false;
    SyncSceneResources();
}

void EditorApp::OnGui() {
    const auto gui_start = std::chrono::steady_clock::now();
    auto finish_gui = [&]() {
        gui_cpu_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gui_start).count();
        performance_.gui_ms = static_cast<float>(gui_cpu_ms_);
    };

    ResetHoveredTextInputTarget();
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    if (!project_loaded_) {
        DrawProjectLauncher();
        DrawToastNotifications();
        finish_gui();
        return;
    }

    if (ImGui::BeginMainMenuBar()) {
        const bool russian = project_.editor_language == "ru";
        const auto tr_menu = [&](std::string_view key, std::string_view en, std::string_view ru) {
            return Tr(key, russian ? ru : en);
        };
        const std::string file_menu = Tr("editor.menu.file", "File");
        const std::string load_project_menu = tr_menu("editor.action.load_project", "Load Project", "Загрузить проект");
        const std::string create_new_menu = tr_menu("editor.action.create_new", "Create New", "Создать новый");
        const std::string save_menu = Tr("editor.action.save", "Save");
        const std::string build_menu = Tr("editor.action.build", "Build Package");
        const std::string build_and_run_menu = Tr("editor.action.build_run", "Build And Run");
        const std::string reset_menu = Tr("editor.action.reset", "Reset Simulation");
        const std::string quit_menu = Tr("editor.action.quit", "Quit");
        const std::string view_menu = Tr("editor.menu.view", "View");
        const std::string screen_menu = Tr("editor.menu.screen", "Screen");
        const std::string menus_menu = Tr("editor.menu.menus", "Menus");
        const std::string ui_menu = Tr("editor.menu.ui", "UI");
        const std::string shaders_menu = Tr("editor.menu.shaders", "Shaders");
        const std::string live_preview = Tr("editor.toggle.live_preview", "Live Preview");
        const std::string debug_draw = Tr("editor.toggle.debug_draw", "Debug Draw");
        const std::string editor_audio = Tr("editor.toggle.editor_audio", "Editor Audio");

        if (ImGui::BeginMenu(file_menu.c_str())) {
            if (ImGui::MenuItem(load_project_menu.c_str())) {
                const std::filesystem::path selected = BrowseForFolder("Select NovaIso project folder");
                if (!selected.empty()) {
                    TryOpenProject(selected);
                }
            }
            if (ImGui::MenuItem(create_new_menu.c_str())) {
                if (launcher_create_parent_.empty()) {
                    launcher_create_parent_ = project_root_.empty() ? ExecutableDirectory().string() : project_root_.parent_path().string();
                }
                if (launcher_create_name_.empty()) {
                    launcher_create_name_ = "NewProject";
                }
                open_create_project_popup_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(save_menu.c_str(), "Ctrl+S")) {
                SaveAll();
            }
            if (ImGui::MenuItem(build_menu.c_str())) {
                ExportGamePackage();
            }
            if (ImGui::MenuItem(build_and_run_menu.c_str())) {
                BuildAndRunGamePackage();
            }
            if (ImGui::MenuItem(reset_menu.c_str(), "R")) {
                scene_.ResetSimulation(false);
            }
            if (ImGui::MenuItem(quit_menu.c_str())) {
                RequestQuit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(view_menu.c_str())) {
            ImGui::MenuItem(live_preview.c_str(), nullptr, &live_preview_);
            ImGui::MenuItem(debug_draw.c_str(), nullptr, &draw_debug_);
            ImGui::MenuItem(editor_audio.c_str(), nullptr, &editor_audio_enabled_);
            asset_manager_.SetAudioEnabled(editor_audio_enabled_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(screen_menu.c_str())) {
            bool fullscreen = IsFullscreen();
            if (ImGui::MenuItem("Fullscreen", "F11", &fullscreen)) {
                SetFullscreen(fullscreen);
            }
            ImGui::Separator();
            ImGui::MenuItem("Toolbar", nullptr, &show_toolbar_);
            ImGui::MenuItem("AI Builder", nullptr, &show_ai_builder_);
            ImGui::MenuItem("Performance", nullptr, &show_performance_panel_);
            ImGui::MenuItem("Viewport", nullptr, &show_viewport_);
            ImGui::MenuItem("Hierarchy", nullptr, &show_hierarchy_panel_);
            ImGui::MenuItem("Layers", nullptr, &show_layers_panel_);
            ImGui::MenuItem("Properties", nullptr, &show_properties_panel_);
            ImGui::MenuItem("Trigger Editor", nullptr, &show_trigger_panel_);
            ImGui::MenuItem("Content Browser", nullptr, &show_asset_browser_);
            ImGui::MenuItem("Resource Inspector", nullptr, &show_resource_inspector_);
            ImGui::MenuItem("Project Settings", nullptr, &show_project_settings_);
            ImGui::MenuItem("Code Editor", nullptr, &show_code_editor_);
            ImGui::MenuItem("Sprite Editor", nullptr, &show_sprite_editor_);
            ImGui::MenuItem("Animation Editor", nullptr, &show_animation_editor_);
            ImGui::MenuItem("Audio Mixer", nullptr, &show_audio_mixer_);
            ImGui::MenuItem("History", nullptr, &show_history_panel_);
            ImGui::MenuItem("Output", nullptr, &show_output_panel_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(menus_menu.c_str())) {
            ImGui::MenuItem("Menu Designer", nullptr, &show_menu_designer_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ui_menu.c_str())) {
            ImGui::MenuItem("UI Editor", nullptr, &show_ui_editor_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(shaders_menu.c_str())) {
            bool rt_enabled = scene_.EditableLevel().lighting.rt_enabled;
            if (ImGui::MenuItem("RT Enable", nullptr, &rt_enabled)) {
                scene_.EditableLevel().lighting.rt_enabled = rt_enabled;
                if (rt_enabled) {
                    scene_.EditableLevel().lighting.enabled = true;
                }
                pending_history_label_ = "Toggle RT lighting";
            }
            ImGui::MenuItem("Shader Stack", nullptr, &show_shader_panel_);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    const bool russian = project_.editor_language == "ru";
    const auto tr_menu = [&](std::string_view key, std::string_view en, std::string_view ru) {
        return Tr(key, russian ? ru : en);
    };
    const std::string create_project_popup = tr_menu("editor.project.create_popup", "Create New Project", "Создание нового проекта");
    const std::string create_project_popup_id = create_project_popup + "###create_project_popup";
    if (open_create_project_popup_) {
        ImGui::OpenPopup(create_project_popup_id.c_str());
        open_create_project_popup_ = false;
    }
    if (ImGui::BeginPopupModal(create_project_popup_id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string parent_folder_label = tr_menu("editor.project.parent_folder", "Parent Folder", "Родительская папка");
        const std::string browse_text = tr_menu("editor.action.browse", "Browse", "Обзор");
        const std::string project_name_label = tr_menu("editor.project.project_name", "Project Name", "Имя проекта");
        const std::string create_text = tr_menu("editor.action.create", "Create", "Создать");
        const std::string cancel_text = tr_menu("editor.action.cancel", "Cancel", "Отмена");

        EditString(parent_folder_label.c_str(), launcher_create_parent_, 1024);
        if (ImGui::Button(browse_text.c_str())) {
            const std::filesystem::path selected = BrowseForFolder("Select parent folder for new NovaIso project");
            if (!selected.empty()) {
                launcher_create_parent_ = selected.string();
            }
        }

        EditString(project_name_label.c_str(), launcher_create_name_);
        const bool can_create = !launcher_create_parent_.empty() && !launcher_create_name_.empty();
        if (!can_create) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(create_text.c_str(), ImVec2(160.0f, 0.0f))) {
            CreateNewProject();
            ImGui::CloseCurrentPopup();
        }
        if (!can_create) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button(cancel_text.c_str(), ImVec2(160.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        if (!status_message_.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", status_message_.c_str());
        }

        ImGui::EndPopup();
    }

    if (show_toolbar_) {
        DrawToolbar();
    }
    if (show_ai_builder_) {
        DrawAiBuilderPanel();
    }
    if (show_hierarchy_panel_) {
        DrawHierarchyPanel();
    }
    if (show_layers_panel_) {
        DrawLayersPanel();
    }
    if (show_properties_panel_) {
        DrawPropertiesPanel();
    }
    if (show_trigger_panel_) {
        DrawTriggerPanel();
    }
    if (show_asset_browser_) {
        DrawAssetBrowser();
    }
    if (show_resource_inspector_) {
        DrawResourceInspector();
    }
    if (show_project_settings_) {
        DrawProjectSettingsPanel();
    }
    DrawMenuDesignerPanel();
    DrawUiEditorPanel();
    DrawShaderPanel();
    DrawAudioMixerPanel();
    if (show_code_editor_) {
        DrawScriptPanel();
    }
    if (show_sprite_editor_) {
        DrawSpriteEditorWindow();
    }
    if (show_animation_editor_) {
        DrawAnimationEditorWindow();
    }
    if (show_history_panel_) {
        DrawHistoryPanel();
    }
    if (show_performance_panel_) {
        DrawPerformancePanel();
    }
    if (show_output_panel_) {
        DrawMessagesPanel();
    }
    if (show_viewport_) {
        DrawViewport();
    }
    DrawToastNotifications();
    finish_gui();
}

void EditorApp::OnDropFile(const std::filesystem::path& path) {
    if (TryAssignDroppedPathToHoveredInput(path, project_loaded_ ? project_root_ : std::filesystem::path{})) {
        status_message_ = "Inserted path: " + NormalizeDroppedPathForInput(path, project_loaded_ ? project_root_ : std::filesystem::path{});
        return;
    }
    if (!project_loaded_) {
        launcher_open_path_ = path.parent_path().string();
        status_message_ = "Open a project first, then import files.";
        return;
    }
    const auto imported = asset_manager_.ImportFile(path);
    InvalidateAssetBrowserCache();
    status_message_ = "Imported: " + imported.generic_string();
    if (script_panel_hovered_ && !current_code_relative_.empty()) {
        python_editor_.InsertText("\"" + imported.generic_string() + "\"");
        script_dirty_ = true;
    }
    glm::vec2 world_position{};
    if (viewport_hovered_ && viewport_mode_ == ViewportMode::Edit && ViewportMouseToWorld(world_position)) {
        if (IsAudioExtension(imported)) {
            PlaceAudioSource(imported.generic_string(), world_position);
        } else if (IsImageExtension(imported)) {
            PlaceTextureEntity(imported.generic_string(), world_position);
        }
    }
    SelectBrowserResource(BrowserSelectionKind::File, imported.generic_string());
}

void EditorApp::DrawHierarchyPanel() {
    if (!show_hierarchy_panel_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("SCN", Tr("editor.window.hierarchy", "Hierarchy"), "HierarchyWindow").c_str(), &show_hierarchy_panel_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.31f, 0.70f, 0.98f, 1.0f});
    PruneHierarchySelection();

    if (ImGui::Button("Add Entity")) {
        pending_history_label_ = "Add entity";
        scene_.EditableLevel().entities.push_back({
            .id = "entity_" + std::to_string(scene_.EditableLevel().entities.size()),
            .archetype = "generic",
            .position = {64.0f, 64.0f},
            .size = {32.0f, 32.0f},
            .dynamic = false,
            .visible = true
        });
        SetHierarchySelectionSingle(SelectionKind::Entity, static_cast<int>(scene_.EditableLevel().entities.size() - 1));
        scene_.ResetSimulation(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Trigger")) {
        pending_history_label_ = "Add trigger";
        scene_.EditableLevel().triggers.push_back({
            .id = "trigger_" + std::to_string(scene_.EditableLevel().triggers.size()),
            .name = "trigger_" + std::to_string(scene_.EditableLevel().triggers.size()),
            .position = {256.0f, 256.0f},
            .size = {96.0f, 96.0f}
        });
        SetHierarchySelectionSingle(SelectionKind::Trigger, static_cast<int>(scene_.EditableLevel().triggers.size() - 1));
        scene_.ResetSimulation(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Parallax")) {
        pending_history_label_ = "Add parallax";
        scene_.EditableLevel().parallax_layers.push_back({
            .id = "parallax_" + std::to_string(scene_.EditableLevel().parallax_layers.size()),
            .name = "layer_" + std::to_string(scene_.EditableLevel().parallax_layers.size()),
            .texture = ""
        });
        SetHierarchySelectionSingle(SelectionKind::Parallax, static_cast<int>(scene_.EditableLevel().parallax_layers.size() - 1));
        scene_.ResetSimulation(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Light")) {
        pending_history_label_ = "Add light";
        scene_.EditableLevel().lights.push_back({
            .id = "light_" + std::to_string(scene_.EditableLevel().lights.size()),
            .name = "light_" + std::to_string(scene_.EditableLevel().lights.size()),
            .type = "point",
            .position = {320.0f, 220.0f},
        });
        SetHierarchySelectionSingle(SelectionKind::Light, static_cast<int>(scene_.EditableLevel().lights.size() - 1));
        scene_.ResetSimulation(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Flashlight")) {
        pending_history_label_ = "Add flashlight";
        scene_.EditableLevel().lights.push_back({
            .id = "flashlight_" + std::to_string(scene_.EditableLevel().lights.size()),
            .name = "flashlight_" + std::to_string(scene_.EditableLevel().lights.size()),
            .type = "flashlight",
            .position = {320.0f, 220.0f},
            .radius = 240.0f,
            .length = 620.0f,
            .source_radius = 10.0f,
            .scatter = 1.4f,
            .direction_degrees = -25.0f,
            .cone_angle = 38.0f,
            .cone_softness = 0.24f
        });
        SetHierarchySelectionSingle(SelectionKind::Light, static_cast<int>(scene_.EditableLevel().lights.size() - 1));
        scene_.ResetSimulation(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Audio")) {
        pending_history_label_ = "Add audio source";
        scene_.EditableLevel().audio_sources.push_back({
            .id = "audio_source_" + std::to_string(scene_.EditableLevel().audio_sources.size()),
            .name = "audio_source_" + std::to_string(scene_.EditableLevel().audio_sources.size()),
            .position = {360.0f, 240.0f}
        });
        SetHierarchySelectionSingle(SelectionKind::AudioSource, static_cast<int>(scene_.EditableLevel().audio_sources.size() - 1));
        scene_.ResetSimulation(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Audio Pak")) {
        pending_history_label_ = "Add audio pak";
        scene_.EditableLevel().audio_paks.push_back({
            .id = "audio_pak_" + std::to_string(scene_.EditableLevel().audio_paks.size()),
            .name = "audio_pak_" + std::to_string(scene_.EditableLevel().audio_paks.size()),
            .position = {420.0f, 260.0f}
        });
        SetHierarchySelectionSingle(SelectionKind::AudioPak, static_cast<int>(scene_.EditableLevel().audio_paks.size() - 1));
        scene_.ResetSimulation(false);
    }
    if (ImGui::Button("Add Camera")) {
        pending_history_label_ = "Add virtual camera";
        scene_.EditableLevel().virtual_cameras.push_back({
            .id = "vcam_" + std::to_string(scene_.EditableLevel().virtual_cameras.size()),
            .name = "vcam_" + std::to_string(scene_.EditableLevel().virtual_cameras.size()),
            .position = {0.0f, 0.0f},
            .size = {960.0f, 540.0f}
        });
        SetHierarchySelectionSingle(SelectionKind::VirtualCamera, static_cast<int>(scene_.EditableLevel().virtual_cameras.size() - 1));
        scene_.ResetSimulation(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Animation")) {
        pending_history_label_ = "Add scene animation";
        scene_.EditableLevel().animations.push_back({
            .id = "scene_animation_" + std::to_string(scene_.EditableLevel().animations.size()),
            .name = "scene_animation_" + std::to_string(scene_.EditableLevel().animations.size()),
            .target_entity = "player"
        });
        SetHierarchySelectionSingle(SelectionKind::SceneAnimation, static_cast<int>(scene_.EditableLevel().animations.size() - 1));
        scene_.ResetSimulation(false);
    }

    struct RenderItem {
        int index = -1;
        int flat_index = -1;
    };

    std::vector<HierarchySelectionItem> flat_items;
    std::vector<RenderItem> entity_items;
    std::vector<RenderItem> trigger_items;
    std::vector<RenderItem> parallax_items;
    std::vector<RenderItem> light_items;
    std::vector<RenderItem> audio_source_items;
    std::vector<RenderItem> audio_pak_items;
    std::vector<RenderItem> virtual_camera_items;
    std::vector<RenderItem> scene_animation_items;
    flat_items.reserve(scene_.EditableLevel().entities.size() +
                       scene_.EditableLevel().triggers.size() +
                       scene_.EditableLevel().parallax_layers.size() +
                       scene_.EditableLevel().lights.size() +
                       scene_.EditableLevel().audio_sources.size() +
                       scene_.EditableLevel().audio_paks.size() +
                       scene_.EditableLevel().virtual_cameras.size() +
                       scene_.EditableLevel().animations.size());

    int next_flat_index = 0;
    for (int i = 0; i < static_cast<int>(scene_.EditableLevel().entities.size()); ++i, ++next_flat_index) {
        flat_items.push_back({SelectionKind::Entity, i});
        entity_items.push_back({i, next_flat_index});
    }
    for (int i = 0; i < static_cast<int>(scene_.EditableLevel().triggers.size()); ++i, ++next_flat_index) {
        flat_items.push_back({SelectionKind::Trigger, i});
        trigger_items.push_back({i, next_flat_index});
    }
    for (int i = 0; i < static_cast<int>(scene_.EditableLevel().parallax_layers.size()); ++i, ++next_flat_index) {
        flat_items.push_back({SelectionKind::Parallax, i});
        parallax_items.push_back({i, next_flat_index});
    }
    for (int i = 0; i < static_cast<int>(scene_.EditableLevel().lights.size()); ++i, ++next_flat_index) {
        flat_items.push_back({SelectionKind::Light, i});
        light_items.push_back({i, next_flat_index});
    }
    for (int i = 0; i < static_cast<int>(scene_.EditableLevel().audio_sources.size()); ++i, ++next_flat_index) {
        flat_items.push_back({SelectionKind::AudioSource, i});
        audio_source_items.push_back({i, next_flat_index});
    }
    for (int i = 0; i < static_cast<int>(scene_.EditableLevel().audio_paks.size()); ++i, ++next_flat_index) {
        flat_items.push_back({SelectionKind::AudioPak, i});
        audio_pak_items.push_back({i, next_flat_index});
    }
    for (int i = 0; i < static_cast<int>(scene_.EditableLevel().virtual_cameras.size()); ++i, ++next_flat_index) {
        flat_items.push_back({SelectionKind::VirtualCamera, i});
        virtual_camera_items.push_back({i, next_flat_index});
    }
    for (int i = 0; i < static_cast<int>(scene_.EditableLevel().animations.size()); ++i, ++next_flat_index) {
        flat_items.push_back({SelectionKind::SceneAnimation, i});
        scene_animation_items.push_back({i, next_flat_index});
    }

    auto sync_primary_from_selection = [&]() {
        if (hierarchy_selection_.empty()) {
            selection_kind_ = SelectionKind::None;
            selection_index_ = -1;
            return;
        }
        const auto active = hierarchy_selection_.back();
        selection_kind_ = active.kind;
        selection_index_ = active.index;
        if (active.kind == SelectionKind::Entity &&
            active.index >= 0 &&
            active.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            current_script_relative_ = scene_.EditableLevel().entities[static_cast<std::size_t>(active.index)].script;
            current_code_relative_ = current_script_relative_;
            LoadCurrentScript();
        }
    };

    bool request_delete = false;
    bool request_duplicate = false;
    bool request_merge_to_texture = false;
    bool request_remove_gaps = false;

    auto handle_select = [&](SelectionKind kind, int index, int flat_index) {
        const bool additive = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
        if (additive) {
            const auto it = std::find_if(hierarchy_selection_.begin(), hierarchy_selection_.end(),
                [&](const HierarchySelectionItem& item) {
                    return item.kind == kind && item.index == index;
                });
            if (it == hierarchy_selection_.end()) {
                hierarchy_selection_.push_back({kind, index});
            } else {
                hierarchy_selection_.erase(it);
            }
        } else {
            SetHierarchySelectionSingle(kind, index);
        }
        hierarchy_anchor_flat_index_ = flat_index;
        sync_primary_from_selection();
    };

    auto open_context_for_item = [&](SelectionKind kind, int index, int flat_index) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !IsHierarchyItemSelected(kind, index)) {
            SetHierarchySelectionSingle(kind, index);
            hierarchy_anchor_flat_index_ = flat_index;
        }
        if (ImGui::BeginPopupContextItem("HierarchyItemContext")) {
            if (ImGui::MenuItem("Delete")) {
                request_delete = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Duplicate")) {
                request_duplicate = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Rename")) {
                if (hierarchy_selection_.size() == 1) {
                    hierarchy_name_buffer_ = HierarchyItemLabel(hierarchy_selection_.front().kind, hierarchy_selection_.front().index);
                    const auto group_pos = hierarchy_name_buffer_.find("  [");
                    if (group_pos != std::string::npos) {
                        hierarchy_name_buffer_.erase(group_pos);
                    }
                } else {
                    hierarchy_name_buffer_.clear();
                }
                open_hierarchy_rename_popup_ = true;
                ImGui::CloseCurrentPopup();
            }
            if (kind == SelectionKind::Trigger && ImGui::MenuItem("Open Graph Editor")) {
                show_trigger_panel_ = true;
                SetHierarchySelectionSingle(kind, index);
                focus_trigger_panel_ = true;
                status_message_ = "Opened trigger graph editor.";
                ImGui::CloseCurrentPopup();
            }
            if (kind == SelectionKind::SceneAnimation && ImGui::MenuItem("Open Animation Editor")) {
                show_animation_editor_ = true;
                SetHierarchySelectionSingle(kind, index);
                status_message_ = "Opened animation editor.";
                ImGui::CloseCurrentPopup();
            }
            const bool all_entities = !hierarchy_selection_.empty() &&
                std::all_of(hierarchy_selection_.begin(), hierarchy_selection_.end(), [](const HierarchySelectionItem& item) {
                    return item.kind == SelectionKind::Entity;
                });
            const bool can_combine = hierarchy_selection_.size() >= 2;
            if (ImGui::MenuItem("Group", nullptr, false, can_combine)) {
                hierarchy_name_buffer_ = "group_" + std::to_string(scene_.EditableLevel().entities.size() +
                    scene_.EditableLevel().triggers.size() +
                    scene_.EditableLevel().parallax_layers.size() +
                    scene_.EditableLevel().lights.size() +
                    scene_.EditableLevel().audio_sources.size() +
                    scene_.EditableLevel().audio_paks.size() +
                    scene_.EditableLevel().virtual_cameras.size() +
                    scene_.EditableLevel().animations.size());
                open_hierarchy_combine_popup_ = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Merge To Image", nullptr, false, all_entities && hierarchy_selection_.size() >= 2)) {
                request_merge_to_texture = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Remove Gaps", nullptr, false, all_entities && hierarchy_selection_.size() >= 2)) {
                request_remove_gaps = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    };

    const auto base_label = [&](SelectionKind kind, int index) {
        std::string label = HierarchyItemLabel(kind, index);
        const auto group_pos = label.find("  [");
        if (group_pos != std::string::npos) {
            label.erase(group_pos);
        }
        return label;
    };

    const auto render_entry = [&](SelectionKind kind, const RenderItem& item) {
        ImGui::PushID((std::to_string(static_cast<int>(kind)) + "_" + std::to_string(item.index)).c_str());
        if (ImGui::Selectable(base_label(kind, item.index).c_str(), IsHierarchyItemSelected(kind, item.index))) {
            handle_select(kind, item.index, item.flat_index);
        }
        open_context_for_item(kind, item.index, item.flat_index);
        ImGui::PopID();
    };

    const auto render_grouped_section = [&](const char* title, SelectionKind kind, const std::vector<RenderItem>& items, const auto& group_for_index) {
        ImGui::Separator();
        ImGui::TextUnformatted(title);

        std::vector<RenderItem> ungrouped;
        std::map<std::string, std::vector<RenderItem>> grouped;
        for (const auto& item : items) {
            const std::string group = group_for_index(item.index);
            if (group.empty()) {
                ungrouped.push_back(item);
            } else {
                grouped[group].push_back(item);
            }
        }

        for (const auto& item : ungrouped) {
            render_entry(kind, item);
        }

        for (const auto& [group_name, grouped_items] : grouped) {
            ImGui::PushID((std::string(title) + "_group_" + group_name).c_str());
            const bool open = ImGui::TreeNodeEx("folder", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "[Folder] %s", group_name.c_str());
            if (ImGui::BeginPopupContextItem("HierarchyFolderContext")) {
                if (ImGui::MenuItem("Select Group")) {
                    hierarchy_selection_.clear();
                    for (const auto& item : grouped_items) {
                        hierarchy_selection_.push_back({kind, item.index});
                    }
                    if (!grouped_items.empty()) {
                        hierarchy_anchor_flat_index_ = grouped_items.front().flat_index;
                        sync_primary_from_selection();
                    }
                }
                if (ImGui::MenuItem("Rename Folder")) {
                    hierarchy_name_buffer_ = group_name;
                    hierarchy_selection_.clear();
                    for (const auto& item : grouped_items) {
                        hierarchy_selection_.push_back({kind, item.index});
                    }
                    open_hierarchy_rename_popup_ = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (open) {
                for (const auto& item : grouped_items) {
                    render_entry(kind, item);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    };

    render_grouped_section("Entities", SelectionKind::Entity, entity_items, [&](int index) {
        return scene_.EditableLevel().entities[static_cast<std::size_t>(index)].editor_group;
    });
    render_grouped_section("Triggers", SelectionKind::Trigger, trigger_items, [&](int index) {
        return scene_.EditableLevel().triggers[static_cast<std::size_t>(index)].editor_group;
    });
    render_grouped_section("Parallax", SelectionKind::Parallax, parallax_items, [&](int index) {
        return scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(index)].editor_group;
    });
    render_grouped_section("Lights", SelectionKind::Light, light_items, [&](int index) {
        return scene_.EditableLevel().lights[static_cast<std::size_t>(index)].editor_group;
    });
    render_grouped_section("Audio Sources", SelectionKind::AudioSource, audio_source_items, [&](int index) {
        return scene_.EditableLevel().audio_sources[static_cast<std::size_t>(index)].editor_group;
    });
    render_grouped_section("Audio Paks", SelectionKind::AudioPak, audio_pak_items, [&](int index) {
        return scene_.EditableLevel().audio_paks[static_cast<std::size_t>(index)].editor_group;
    });
    render_grouped_section("Virtual Cameras", SelectionKind::VirtualCamera, virtual_camera_items, [&](int index) {
        return scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(index)].editor_group;
    });
    render_grouped_section("Scene Animations", SelectionKind::SceneAnimation, scene_animation_items, [&](int index) {
        return scene_.EditableLevel().animations[static_cast<std::size_t>(index)].editor_group;
    });

    if (open_hierarchy_rename_popup_) {
        ImGui::OpenPopup("HierarchyRenamePopup");
        open_hierarchy_rename_popup_ = false;
    }
    if (ImGui::BeginPopupModal("HierarchyRenamePopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditString("Name", hierarchy_name_buffer_);
        if (ImGui::Button("Apply", ImVec2(120.0f, 0.0f))) {
            RenameHierarchySelection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (open_hierarchy_combine_popup_) {
        ImGui::OpenPopup("HierarchyCombinePopup");
        open_hierarchy_combine_popup_ = false;
    }
    if (ImGui::BeginPopupModal("HierarchyCombinePopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditString("Group", hierarchy_name_buffer_);
        if (ImGui::Button("Apply", ImVec2(120.0f, 0.0f))) {
            CombineHierarchySelection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (request_delete) {
        DeleteHierarchySelection();
    }
    if (request_duplicate) {
        DuplicateHierarchySelection();
    }
    if (request_merge_to_texture) {
        MergeHierarchySelectionToTexture();
    }
    if (request_remove_gaps) {
        RemoveHierarchySelectionGaps();
    }

    ImGui::End();
}

void EditorApp::DrawPropertiesPanel() {
    if (!show_properties_panel_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("PRP", Tr("editor.window.properties", "Properties"), "PropertiesWindow").c_str(), &show_properties_panel_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.28f, 0.78f, 0.82f, 1.0f});

    if (selection_kind_ == SelectionKind::Entity && selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())) {
        auto& entity = scene_.EditableLevel().entities[selection_index_];
        bool changed = false;
        changed |= EditString("Id", entity.id);
        changed |= EditString("Group", entity.editor_group);
        changed |= EditString("Archetype", entity.archetype);
        changed |= EditString("Object Asset", entity.object_asset);
        changed |= EditString("Sprite Asset", entity.sprite_asset);
        changed |= EditString("Animation", entity.animation);
        changed |= ImGui::InputFloat2("Position", &entity.position.x);
        changed |= ImGui::InputFloat2("Size", &entity.size.x);
        changed |= ImGui::InputFloat2("Velocity", &entity.velocity.x);
        changed |= ImGui::ColorEdit4("Tint", &entity.tint.x);
        changed |= EditString("Texture", entity.texture);
        changed |= EditString("Sound", entity.sound);
        changed |= ImGui::InputFloat4("UV", &entity.uv.x);
        changed |= ImGui::InputInt("Layer", &entity.layer);
        changed |= ImGui::Checkbox("Dynamic", &entity.dynamic);
        changed |= ImGui::Checkbox("Collidable", &entity.collidable);
        changed |= ImGui::Checkbox("Visible", &entity.visible);
        changed |= ImGui::SliderFloat("Reflection", &entity.reflection, 0.0f, 1.5f, "%.2f");
        ImGui::Separator();
        ImGui::TextUnformatted("Surface Relief");
        changed |= ImGui::Checkbox("Bump Mapping", &entity.relief_enabled);
        changed |= EditString("Normal Map", entity.normal_map);
        changed |= EditString("Height Map", entity.height_map);
        changed |= EditString("Displacement Map", entity.displacement_map);
        if (ImGui::Button("Generate Texture Maps")) {
            std::string texture_path;
            if (selection_index_ >= 0 &&
                selection_index_ < static_cast<int>(scene_.RuntimeEntities().size())) {
                texture_path = scene_.RuntimeEntities()[static_cast<std::size_t>(selection_index_)].texture;
            }
            if (texture_path.empty()) {
                texture_path = entity.texture;
            }
            if (!texture_path.empty()) {
                const auto generated = GenerateReliefMapsFromTexture(project_root_, texture_path, entity.id.empty() ? "entity_relief" : entity.id);
                if (generated.has_value()) {
                    entity.normal_map = generated->normal_map;
                    entity.height_map = generated->height_map;
                    entity.displacement_map = generated->displacement_map;
                    entity.relief_enabled = generated->settings.enabled;
                    entity.bump_strength = generated->settings.bump_strength;
                    entity.relief_depth = generated->settings.relief_depth;
                    entity.parallax_depth = generated->settings.parallax_depth;
                    entity.relief_contrast = generated->settings.relief_contrast;
                    changed = true;
                    status_message_ = "Generated normal, height and displacement maps.";
                } else {
                    status_message_ = "Failed to generate texture maps from source texture.";
                }
            } else {
                status_message_ = "No source texture found for this entity.";
            }
        }
        if (entity.relief_enabled) {
            changed |= ImGui::SliderFloat("Bump Strength", &entity.bump_strength, 0.0f, 4.0f, "%.2f");
            changed |= ImGui::SliderFloat("Relief Depth", &entity.relief_depth, 0.0f, 0.12f, "%.3f");
            changed |= ImGui::SliderFloat("Texture Parallax", &entity.parallax_depth, 0.0f, 0.08f, "%.3f");
            changed |= ImGui::SliderFloat("Relief Contrast", &entity.relief_contrast, 0.2f, 4.0f, "%.2f");
        }
        changed |= ImGui::Checkbox("Pseudo 3D", &entity.pseudo_3d);
        changed |= ImGui::InputFloat2("Collider Offset", &entity.collider_offset.x);
        changed |= ImGui::InputFloat2("Collider Size", &entity.collider_size.x);
        changed |= ImGui::InputFloat("Pseudo 3D Height", &entity.pseudo_3d_height);
        entity.reflection = std::clamp(entity.reflection, 0.0f, 1.5f);
        entity.bump_strength = std::clamp(entity.bump_strength, 0.0f, 4.0f);
        entity.relief_depth = std::clamp(entity.relief_depth, 0.0f, 0.12f);
        entity.parallax_depth = std::clamp(entity.parallax_depth, 0.0f, 0.08f);
        entity.relief_contrast = std::clamp(entity.relief_contrast, 0.2f, 4.0f);
        entity.pseudo_3d_height = std::max(entity.pseudo_3d_height, 0.0f);
        changed |= ImGui::InputFloat("Animation Speed", &entity.animation_speed);
        if (EditString("Script", entity.script)) {
            changed = true;
            current_script_relative_ = entity.script;
            current_code_relative_ = current_script_relative_;
            LoadCurrentScript();
        }
        changed |= EditString("Trigger Hook", entity.on_trigger);
        if (ImGui::BeginCombo("Attach To", entity.attached_to.empty() ? "(none)" : entity.attached_to.c_str())) {
            if (ImGui::Selectable("(none)", entity.attached_to.empty())) {
                entity.attached_to.clear();
                changed = true;
            }
            for (int i = 0; i < static_cast<int>(scene_.EditableLevel().entities.size()); ++i) {
                if (i == selection_index_) {
                    continue;
                }
                const auto& candidate = scene_.EditableLevel().entities[static_cast<std::size_t>(i)];
                const std::string label = candidate.id.empty() ? ("entity_" + std::to_string(i)) : candidate.id;
                const bool selected = entity.attached_to == label;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    entity.attached_to = label;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        changed |= ImGui::InputFloat2("Attach Offset", &entity.attach_offset.x);

        if (IsParticleEmitterEntity(entity)) {
            if (!entity.properties.is_object()) {
                entity.properties = nlohmann::json::object();
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Particle Emitter");
            std::string particle_asset = entity.properties.value("particle_asset", std::string{});
            bool emitter_enabled = entity.properties.value("emitter_enabled", true);
            bool particle_autoplay = entity.properties.value("particle_autoplay", true);
            bool burst_on_start = entity.properties.value("burst_on_start", false);
            changed |= EditString("Particle Asset", particle_asset);
            changed |= ImGui::Checkbox("Emitter Enabled", &emitter_enabled);
            changed |= ImGui::Checkbox("Autoplay", &particle_autoplay);
            changed |= ImGui::Checkbox("Burst On Start", &burst_on_start);
            entity.properties["particle_emitter"] = true;
            entity.properties["particle_asset"] = particle_asset;
            entity.properties["emitter_enabled"] = emitter_enabled;
            entity.properties["particle_autoplay"] = particle_autoplay;
            entity.properties["burst_on_start"] = burst_on_start;
            if (browser_selection_kind_ == BrowserSelectionKind::Particle) {
                if (ImGui::Button("Use Selected Particle Effect")) {
                    entity.properties["particle_asset"] = browser_selection_relative_;
                    changed = true;
                }
            }
            if (ImGui::Button("Play Emitter")) {
                scene_.PlayParticleEmitter(entity.id, true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop Emitter")) {
                scene_.StopParticleEmitter(entity.id);
            }
            ImGui::SameLine();
            if (ImGui::Button("Burst")) {
                scene_.BurstParticleEmitter(entity.id);
            }
        }

        if (browser_selection_kind_ == BrowserSelectionKind::Object) {
            if (ImGui::Button("Use Selected Object Resource")) {
                ApplyObjectAssetToEntity(entity, selected_object_, browser_selection_relative_);
                changed = true;
            }
        }

        if (changed) {
            scene_.ResetSimulation(false);
        }
    } else if (selection_kind_ == SelectionKind::Trigger && selection_index_ >= 0 &&
               selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) {
        auto& trigger = scene_.EditableLevel().triggers[selection_index_];
        bool changed = false;
        changed |= EditString("Id", trigger.id);
        changed |= EditString("Name", trigger.name);
        changed |= EditString("Group", trigger.editor_group);
        changed |= EditString("Trigger Asset", trigger.asset);
        changed |= ImGui::InputFloat2("Position", &trigger.position.x);
        changed |= ImGui::InputFloat2("Size", &trigger.size.x);
        changed |= ImGui::ColorEdit4("Color", &trigger.color.x);
        changed |= ImGui::Checkbox("Once", &trigger.once);
        changed |= ImGui::Checkbox("Enabled", &trigger.enabled);
        if (browser_selection_kind_ == BrowserSelectionKind::Trigger) {
            if (ImGui::Button("Use Selected Trigger Resource")) {
                trigger.asset = browser_selection_relative_;
                trigger.size = selected_trigger_.default_size;
                trigger.color = selected_trigger_.color;
                trigger.once = selected_trigger_.once;
                trigger.enabled = selected_trigger_.enabled;
                trigger.conditions = selected_trigger_.conditions;
                trigger.actions = selected_trigger_.actions;
                changed = true;
            }
        }
        if (changed) {
            scene_.ResetSimulation(false);
        }
    } else if (selection_kind_ == SelectionKind::Parallax && selection_index_ >= 0 &&
               selection_index_ < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
        auto& layer = scene_.EditableLevel().parallax_layers[selection_index_];
        bool changed = false;
        changed |= EditString("Id", layer.id);
        changed |= EditString("Name", layer.name);
        changed |= EditString("Group", layer.editor_group);
        changed |= EditString("Texture", layer.texture);
        changed |= ImGui::InputFloat2("Speed", &layer.speed.x);
        float parallax_speed = layer.speed.x;
        if (ImGui::SliderFloat("Parallax Speed", &parallax_speed, -2.0f, 2.0f, "%.2f")) {
            layer.speed.x = parallax_speed;
            changed = true;
        }
        changed |= ImGui::InputFloat2("Scale", &layer.scale.x);
        changed |= ImGui::InputFloat2("Offset", &layer.offset.x);
        changed |= ImGui::ColorEdit4("Tint", &layer.tint.x);
        changed |= ImGui::InputFloat("Depth", &layer.depth);
        changed |= ImGui::InputFloat("Zoom Factor", &layer.zoom_factor);
        changed |= ImGui::Checkbox("Receives Lighting", &layer.receives_lighting);
        changed |= ImGui::SliderFloat("Lighting Response", &layer.lighting_response, 0.0f, 2.5f, "%.2f");
        changed |= ImGui::Checkbox("Artificial Light", &layer.artificial_light);
        changed |= ImGui::ColorEdit4("Artificial Light Color", &layer.artificial_light_color.x);
        changed |= ImGui::SliderFloat("Artificial Light Strength", &layer.artificial_light_strength, 0.0f, 3.0f, "%.2f");
        changed |= ImGui::Checkbox("Repeat", &layer.repeat);
        changed |= ImGui::Checkbox("Visible", &layer.visible);
        layer.lighting_response = std::clamp(layer.lighting_response, 0.0f, 2.5f);
        layer.artificial_light_strength = std::clamp(layer.artificial_light_strength, 0.0f, 3.0f);
        if (changed) {
            scene_.ResetSimulation(false);
        }
    } else if (selection_kind_ == SelectionKind::Light && selection_index_ >= 0 &&
               selection_index_ < static_cast<int>(scene_.EditableLevel().lights.size())) {
        auto& light = scene_.EditableLevel().lights[selection_index_];
        bool changed = false;
        auto attachment_preview = [&](std::string_view token) {
            if (token.empty()) {
                return std::string("(none)");
            }
            const std::string lowered = ToLowerCopy(std::string(token));
            if (lowered.rfind("parallax:", 0) == 0 || lowered.rfind("layer:", 0) == 0) {
                const std::string raw = lowered.rfind("parallax:", 0) == 0 ? std::string(token.substr(9)) : std::string(token.substr(6));
                for (const auto& candidate : scene_.EditableLevel().parallax_layers) {
                    if (ToLowerCopy(candidate.id) == ToLowerCopy(raw) || ToLowerCopy(candidate.name) == ToLowerCopy(raw)) {
                        const std::string label = !candidate.name.empty() ? candidate.name : (!candidate.id.empty() ? candidate.id : raw);
                        return std::string("Parallax: ") + label;
                    }
                }
            }
            return std::string(token);
        };
        changed |= EditString("Id", light.id);
        changed |= EditString("Name", light.name);
        changed |= EditString("Group", light.editor_group);
        static constexpr const char* light_type_labels[] = {"Point Lamp", "Flashlight"};
        int light_type_index = 0;
        const std::string light_type = ToLowerCopy(light.type);
        if (light_type == "flashlight" || light_type == "spot" || light_type == "spotlight" || light_type == "directional") {
            light_type_index = 1;
        }
        if (ImGui::BeginCombo("Light Type", light_type_labels[light_type_index])) {
            for (int index = 0; index < 2; ++index) {
                const bool selected = light_type_index == index;
                if (ImGui::Selectable(light_type_labels[index], selected)) {
                    light_type_index = index;
                    light.type = index == 0 ? "point" : "flashlight";
                    changed = true;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        changed |= ImGui::InputFloat2("Position", &light.position.x);
        changed |= ImGui::InputFloat("Radius", &light.radius);
        if (light_type_index == 1) {
            changed |= ImGui::InputFloat("Length", &light.length);
            changed |= ImGui::SliderFloat("Direction", &light.direction_degrees, -180.0f, 180.0f, "%.1f deg");
            changed |= ImGui::SliderFloat("Cone Angle", &light.cone_angle, 8.0f, 170.0f, "%.1f deg");
            changed |= ImGui::SliderFloat("Cone Softness", &light.cone_softness, 0.02f, 0.95f, "%.2f");
        }
        changed |= ImGui::InputFloat("Source Radius", &light.source_radius);
        changed |= ImGui::SliderFloat("Scatter", &light.scatter, 0.2f, 2.8f, "%.2f");
        changed |= ImGui::ColorEdit4("Color", &light.color.x);
        changed |= ImGui::InputFloat("Intensity", &light.intensity);
        changed |= ImGui::Checkbox("Enabled", &light.enabled);
        const std::string attach_preview = attachment_preview(light.attached_to);
        if (ImGui::BeginCombo("Attach To", attach_preview.c_str())) {
            if (ImGui::Selectable("(none)", light.attached_to.empty())) {
                light.attached_to.clear();
                changed = true;
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Entities");
            for (int i = 0; i < static_cast<int>(scene_.EditableLevel().entities.size()); ++i) {
                const auto& candidate = scene_.EditableLevel().entities[static_cast<std::size_t>(i)];
                const std::string label = candidate.id.empty() ? ("entity_" + std::to_string(i)) : candidate.id;
                const bool selected = light.attached_to == label;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    light.attached_to = label;
                    changed = true;
                }
            }
            if (!scene_.EditableLevel().parallax_layers.empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted("Parallax Layers");
                for (int i = 0; i < static_cast<int>(scene_.EditableLevel().parallax_layers.size()); ++i) {
                    const auto& candidate = scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(i)];
                    const std::string raw = !candidate.id.empty() ? candidate.id : (!candidate.name.empty() ? candidate.name : ("parallax_" + std::to_string(i)));
                    const std::string token = "parallax:" + raw;
                    const std::string label = "Parallax: " + (!candidate.name.empty() ? candidate.name : raw);
                    const bool selected = ToLowerCopy(light.attached_to) == ToLowerCopy(token);
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        light.attached_to = token;
                        changed = true;
                    }
                }
            }
            ImGui::EndCombo();
        }
        changed |= ImGui::InputFloat2("Attach Offset", &light.attach_offset.x);
        light.radius = std::max(light.radius, 8.0f);
        light.length = std::max(light.length, light.radius);
        light.source_radius = std::clamp(light.source_radius, 0.0f, std::max(light.radius, light.length));
        light.scatter = std::clamp(light.scatter, 0.2f, 2.8f);
        light.cone_angle = std::clamp(light.cone_angle, 8.0f, 170.0f);
        light.cone_softness = std::clamp(light.cone_softness, 0.02f, 0.95f);
        light.intensity = std::max(light.intensity, 0.0f);
        if (changed) {
            scene_.ResetSimulation(false);
        }
    } else if (selection_kind_ == SelectionKind::AudioSource && selection_index_ >= 0 &&
               selection_index_ < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
        auto& source = scene_.EditableLevel().audio_sources[selection_index_];
        bool changed = false;
        changed |= EditString("Id", source.id);
        changed |= EditString("Name", source.name);
        changed |= EditString("Group", source.editor_group);
        changed |= EditString("Audio", source.audio);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_AUDIO_PATH")) {
                source.audio = static_cast<const char*>(payload->Data);
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }
        if (browser_selection_kind_ == BrowserSelectionKind::File && IsAudioExtension(std::filesystem::path(browser_selection_relative_))) {
            if (ImGui::Button("Use Selected Audio File")) {
                source.audio = browser_selection_relative_;
                changed = true;
            }
        }
        changed |= ImGui::InputFloat2("Position", &source.position.x);
        changed |= ImGui::Checkbox("Enabled", &source.enabled);
        changed |= ImGui::Checkbox("Always", &source.always);
        changed |= ImGui::Checkbox("Loop", &source.loop);
        changed |= ImGui::InputFloat("Radius", &source.radius);
        changed |= ImGui::Checkbox("Stop On Exit", &source.stop_on_exit);
        changed |= ImGui::InputFloat("Distance", &source.distance);
        changed |= ImGui::SliderFloat("Volume", &source.volume, 0.0f, 1.0f, "%.2f");
        source.radius = std::max(source.radius, 1.0f);
        source.distance = std::max(source.distance, source.radius);
        source.volume = std::clamp(source.volume, 0.0f, 1.0f);
        if (changed) {
            scene_.ResetSimulation(false);
        }
    } else if (selection_kind_ == SelectionKind::AudioPak && selection_index_ >= 0 &&
               selection_index_ < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
        auto& pak = scene_.EditableLevel().audio_paks[selection_index_];
        bool changed = false;
        changed |= EditString("Id", pak.id);
        changed |= EditString("Name", pak.name);
        changed |= EditString("Group", pak.editor_group);
        changed |= ImGui::InputFloat2("Position", &pak.position.x);
        changed |= ImGui::Checkbox("Enabled", &pak.enabled);
        changed |= ImGui::Checkbox("Always", &pak.always);
        changed |= ImGui::Checkbox("Loop Current", &pak.loop);
        changed |= ImGui::InputFloat("Radius", &pak.radius);
        changed |= ImGui::Checkbox("Stop On Exit", &pak.stop_on_exit);
        changed |= ImGui::InputFloat("Distance", &pak.distance);
        changed |= ImGui::SliderFloat("Volume", &pak.volume, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::Checkbox("Shuffle", &pak.shuffle);
        changed |= ImGui::Checkbox("Repeat Playlist", &pak.repeat_playlist);
        pak.radius = std::max(pak.radius, 1.0f);
        pak.distance = std::max(pak.distance, pak.radius);
        pak.volume = std::clamp(pak.volume, 0.0f, 1.0f);

        if (ImGui::BeginChild("AudioPakTracks", ImVec2(0.0f, 180.0f), true)) {
            for (int i = 0; i < static_cast<int>(pak.tracks.size()); ++i) {
                ImGui::PushID(i);
                EditString("Track", pak.tracks[static_cast<std::size_t>(i)]);
                ImGui::SameLine();
                if (ImGui::Button("Remove")) {
                    pak.tracks.erase(pak.tracks.begin() + i);
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Add Track Slot")) {
                pak.tracks.push_back("");
                changed = true;
            }
            if (browser_selection_kind_ == BrowserSelectionKind::File && IsAudioExtension(std::filesystem::path(browser_selection_relative_))) {
                if (ImGui::Button("Add Selected Audio File")) {
                    pak.tracks.push_back(browser_selection_relative_);
                    changed = true;
                }
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_AUDIO_PATH")) {
                    const std::string track = static_cast<const char*>(payload->Data);
                    pak.tracks.push_back(track);
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::TextDisabled("Drop audio files here from the Content Browser.");
            ImGui::EndChild();
        }

        if (changed) {
            scene_.ResetSimulation(false);
        }
    } else if (selection_kind_ == SelectionKind::VirtualCamera && selection_index_ >= 0 &&
               selection_index_ < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
        auto& camera = scene_.EditableLevel().virtual_cameras[selection_index_];
        bool changed = false;
        changed |= EditString("Id", camera.id);
        changed |= EditString("Name", camera.name);
        changed |= EditString("Group", camera.editor_group);
        changed |= ImGui::InputFloat2("Position", &camera.position.x);
        changed |= ImGui::InputFloat2("Size", &camera.size.x);
        changed |= EditString("Follow Target", camera.follow_target);
        changed |= ImGui::InputFloat2("Follow Offset", &camera.follow_offset.x);
        changed |= ImGui::InputFloat2("Dead Zone", &camera.dead_zone.x);
        changed |= ImGui::InputFloat("Zoom", &camera.zoom);
        changed |= ImGui::InputFloat("Follow Lag", &camera.follow_lag);
        changed |= ImGui::InputFloat("Zoom Lag", &camera.zoom_lag);
        changed |= ImGui::Checkbox("Enabled", &camera.enabled);
        changed |= ImGui::Checkbox("Auto Activate", &camera.auto_activate);
        changed |= ImGui::Checkbox("Release On Exit", &camera.release_on_exit);
        changed |= ImGui::Checkbox("Override Mode", &camera.override_mode);
        int mode_index = camera.camera_mode == "isometric" ? 1 : 0;
        if (ImGui::Combo("Camera Mode", &mode_index, "side\0isometric\0")) {
            camera.camera_mode = mode_index == 1 ? "isometric" : "side";
            changed = true;
        }
        ImGui::SeparatorText("Pseudo 3D");
        changed |= ImGui::Checkbox("Pseudo 3D Enabled", &camera.pseudo_3d_enabled);
        changed |= ImGui::InputFloat2("Pseudo 3D Offset", &camera.pseudo_3d_offset.x);
        changed |= ImGui::SliderFloat("Pseudo 3D Top Tint", &camera.pseudo_3d_top_tint, 0.55f, 1.65f, "%.2f");
        changed |= ImGui::SliderFloat("Pseudo 3D Side Tint", &camera.pseudo_3d_side_tint, 0.08f, 1.15f, "%.2f");
        ImGui::TextDisabled("Only entities with `Pseudo 3D` enabled are extruded by this camera.");
        camera.size.x = std::max(camera.size.x, 64.0f);
        camera.size.y = std::max(camera.size.y, 64.0f);
        camera.dead_zone.x = std::max(camera.dead_zone.x, 0.0f);
        camera.dead_zone.y = std::max(camera.dead_zone.y, 0.0f);
        camera.zoom = std::max(camera.zoom, 0.05f);
        camera.follow_lag = std::max(camera.follow_lag, 0.0f);
        camera.zoom_lag = std::max(camera.zoom_lag, 0.0f);
        camera.pseudo_3d_top_tint = std::clamp(camera.pseudo_3d_top_tint, 0.55f, 1.65f);
        camera.pseudo_3d_side_tint = std::clamp(camera.pseudo_3d_side_tint, 0.08f, 1.15f);
        if (ImGui::Button("Preview This Camera")) {
            scene_.ActivateVirtualCamera(camera.id);
            status_message_ = "Virtual camera preview: " + camera.name;
        }
        ImGui::SameLine();
        if (ImGui::Button("Release Camera")) {
            scene_.ReleaseVirtualCamera();
            status_message_ = "Released virtual camera preview.";
        }
        if (changed) {
            scene_.ResetSimulation(false);
        }
    } else if (selection_kind_ == SelectionKind::SceneAnimation && selection_index_ >= 0 &&
               selection_index_ < static_cast<int>(scene_.EditableLevel().animations.size())) {
        auto& animation = scene_.EditableLevel().animations[selection_index_];
        bool changed = false;
        changed |= EditString("Id", animation.id);
        changed |= EditString("Name", animation.name);
        changed |= EditString("Group", animation.editor_group);
        changed |= EditString("Animation Asset", animation.asset);
        changed |= EditString("Target Entity", animation.target_entity);
        changed |= ImGui::Checkbox("Enabled", &animation.enabled);
        changed |= ImGui::Checkbox("Play On Start", &animation.play_on_start);
        changed |= ImGui::Checkbox("Loop", &animation.loop);
        changed |= ImGui::InputFloat("Speed", &animation.speed);
        animation.speed = std::max(animation.speed, 0.01f);
        if (browser_selection_kind_ == BrowserSelectionKind::Animation) {
            if (ImGui::Button("Use Selected Animation Resource")) {
                animation.asset = browser_selection_relative_;
                changed = true;
            }
        }
        if (ImGui::Button("Play Animation")) {
            scene_.PlaySceneAnimation(animation.id, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop Animation")) {
            scene_.StopSceneAnimation(animation.id, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Animation Editor")) {
            show_animation_editor_ = true;
            if (!animation.asset.empty()) {
                SelectBrowserResource(BrowserSelectionKind::Animation, animation.asset);
            }
        }
        if (changed) {
            scene_.ResetSimulation(false);
        }
    } else {
        auto& level = scene_.EditableLevel();
        bool changed = false;
        ImGui::TextUnformatted("Scene Settings");
        changed |= EditString("Level Name", level.name);
        changed |= ImGui::InputFloat2("Player Spawn", &level.player_spawn.x);
        changed |= ImGui::ColorEdit4("Background Color", &level.clear_color.x);
        changed |= ImGui::Checkbox("Lighting Enabled", &level.lighting.enabled);
        changed |= ImGui::Checkbox("RT Lights", &level.lighting.rt_enabled);
        if (level.lighting.rt_enabled) {
            level.lighting.enabled = true;
        }
        changed |= ImGui::ColorEdit4("Ambient Color", &level.lighting.ambient_color.x);
        changed |= ImGui::InputFloat("Ambient Intensity", &level.lighting.ambient_intensity);
        changed |= ImGui::SliderFloat("Shadow Strength", &level.lighting.shadow_strength, 0.05f, 2.4f, "%.2f");
        changed |= ImGui::SliderFloat("Shadow Softness", &level.lighting.shadow_softness, 0.15f, 3.5f, "%.2f");
        changed |= ImGui::SliderFloat("Shadow Diffusion", &level.lighting.shadow_diffusion, 0.0f, 2.8f, "%.2f");
        changed |= ImGui::SliderInt("Shadow Samples", &level.lighting.shadow_samples, 1, 12);
        level.lighting.ambient_intensity = std::clamp(level.lighting.ambient_intensity, 0.0f, 1.5f);
        level.lighting.shadow_strength = std::clamp(level.lighting.shadow_strength, 0.05f, 2.4f);
        level.lighting.shadow_softness = std::clamp(level.lighting.shadow_softness, 0.15f, 3.5f);
        level.lighting.shadow_diffusion = std::clamp(level.lighting.shadow_diffusion, 0.0f, 2.8f);
        level.lighting.shadow_samples = std::clamp(level.lighting.shadow_samples, 1, 12);

        ImGui::SeparatorText("Player Camera");
        auto& player_camera = level.player_camera;
        changed |= ImGui::Checkbox("Player Camera Enabled", &player_camera.enabled);
        changed |= EditString("Player Camera Target", player_camera.follow_target);
        changed |= ImGui::InputFloat2("Player Camera Offset", &player_camera.follow_offset.x);
        changed |= ImGui::InputFloat2("Player Camera Dead Zone", &player_camera.dead_zone.x);
        changed |= ImGui::InputFloat("Player Camera Zoom", &player_camera.zoom);
        changed |= ImGui::InputFloat("Player Camera Follow Lag", &player_camera.follow_lag);
        changed |= ImGui::InputFloat("Player Camera Zoom Lag", &player_camera.zoom_lag);
        changed |= ImGui::Checkbox("Player Camera Clamp To World", &player_camera.clamp_to_world);
        changed |= ImGui::Checkbox("Player Camera Override Mode", &player_camera.override_mode);
        int player_camera_mode_index = player_camera.camera_mode == "isometric" ? 1 : 0;
        if (ImGui::Combo("Player Camera Mode", &player_camera_mode_index, "side isometric ")) {
            player_camera.camera_mode = player_camera_mode_index == 1 ? "isometric" : "side";
            changed = true;
        }
        changed |= ImGui::Checkbox("Player Camera Pseudo 3D", &player_camera.pseudo_3d_enabled);
        changed |= ImGui::InputFloat2("Player Camera 3D Offset", &player_camera.pseudo_3d_offset.x);
        changed |= ImGui::SliderFloat("Player Camera Top Tint", &player_camera.pseudo_3d_top_tint, 0.55f, 1.65f, "%.2f");
        changed |= ImGui::SliderFloat("Player Camera Side Tint", &player_camera.pseudo_3d_side_tint, 0.08f, 1.15f, "%.2f");
        player_camera.dead_zone.x = std::max(player_camera.dead_zone.x, 0.0f);
        player_camera.dead_zone.y = std::max(player_camera.dead_zone.y, 0.0f);
        player_camera.zoom = std::max(player_camera.zoom, 0.05f);
        player_camera.follow_lag = std::max(player_camera.follow_lag, 0.0f);
        player_camera.zoom_lag = std::max(player_camera.zoom_lag, 0.0f);
        player_camera.pseudo_3d_top_tint = std::clamp(player_camera.pseudo_3d_top_tint, 0.55f, 1.65f);
        player_camera.pseudo_3d_side_tint = std::clamp(player_camera.pseudo_3d_side_tint, 0.08f, 1.15f);

        if (ImGui::Button("Add Scene Light")) {
            level.lights.push_back({
                .id = "light_" + std::to_string(level.lights.size()),
                .name = "light_" + std::to_string(level.lights.size()),
                .type = "point",
                .position = level.player_spawn,
            });
            SetHierarchySelectionSingle(SelectionKind::Light, static_cast<int>(level.lights.size() - 1));
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Flashlight")) {
            level.lights.push_back({
                .id = "flashlight_" + std::to_string(level.lights.size()),
                .name = "flashlight_" + std::to_string(level.lights.size()),
                .type = "flashlight",
                .position = level.player_spawn,
                .radius = 220.0f,
                .length = 620.0f,
                .source_radius = 10.0f,
                .scatter = 1.45f,
                .direction_degrees = -25.0f,
                .cone_angle = 38.0f,
                .cone_softness = 0.22f
            });
            SetHierarchySelectionSingle(SelectionKind::Light, static_cast<int>(level.lights.size() - 1));
            changed = true;
        }
        if (changed) {
            scene_.ResetSimulation(false);
        }
    }

    ImGui::End();
}

void EditorApp::DrawTriggerPanel() {
    if (!show_trigger_panel_) {
        return;
    }
    if (focus_trigger_panel_) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(StableWindowLabel("TRG", Tr("editor.window.trigger_editor", "Trigger Editor"), "TriggerWindow").c_str(), &show_trigger_panel_)) {
        focus_trigger_panel_ = false;
        ImGui::End();
        return;
    }
    focus_trigger_panel_ = false;
    DecorateEditorWindow({0.80f, 0.62f, 0.22f, 1.0f});
    if (selection_kind_ == SelectionKind::Trigger && selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) {
        bool changed = false;
        auto& trigger = scene_.EditableLevel().triggers[selection_index_];

        ImGui::TextUnformatted("Visual Script");
        const ImVec2 canvas_size{std::max(ImGui::GetContentRegionAvail().x, 320.0f), 340.0f};
        const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("TriggerGraphCanvas", canvas_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const ImVec2 canvas_max{canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y};
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(canvas_min, canvas_max, IM_COL32(18, 22, 30, 245), 8.0f);
        for (float x = canvas_min.x; x < canvas_max.x; x += 32.0f) {
            draw_list->AddLine({x, canvas_min.y}, {x, canvas_max.y}, IM_COL32(255, 255, 255, 12));
        }
        for (float y = canvas_min.y; y < canvas_max.y; y += 32.0f) {
            draw_list->AddLine({canvas_min.x, y}, {canvas_max.x, y}, IM_COL32(255, 255, 255, 12));
        }

        struct NodeHit {
            GraphNodeSelectionKind kind = GraphNodeSelectionKind::None;
            int index = -1;
            ImVec2 min{};
            ImVec2 max{};
        };
        const auto rect_center = [](ImVec2 min, ImVec2 max) {
            return ImVec2{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
        };
        const auto rect_contains = [](ImVec2 min, ImVec2 max, ImVec2 point) {
            return point.x >= min.x && point.y >= min.y && point.x <= max.x && point.y <= max.y;
        };

        const glm::vec2 mouse_local{
            ImGui::GetIO().MousePos.x - canvas_min.x,
            ImGui::GetIO().MousePos.y - canvas_min.y
        };
        const ImVec2 start_pos{canvas_min.x + 36.0f, canvas_min.y + canvas_size.y * 0.5f - 34.0f};
        const ImVec2 start_size{150.0f, 68.0f};
        const ImVec2 start_rect_min = start_pos;
        const ImVec2 start_rect_max{start_pos.x + start_size.x, start_pos.y + start_size.y};
        draw_list->AddRectFilled(start_rect_min, start_rect_max, IM_COL32(120, 36, 38, 255), 8.0f);
        draw_list->AddRect(start_rect_min, start_rect_max, IM_COL32(230, 90, 90, 255), 8.0f, 0, 2.0f);
        draw_list->AddText({start_pos.x + 14.0f, start_pos.y + 12.0f}, IM_COL32(255, 255, 255, 255), "Trigger Enter");
        draw_list->AddText({start_pos.x + 14.0f, start_pos.y + 34.0f}, IM_COL32(255, 210, 210, 255), trigger.name.c_str());

        auto ensure_condition_pos = [&](assets::TriggerCondition& condition, int index) {
            if (condition.editor_position.x <= 0.0f && condition.editor_position.y <= 0.0f) {
                condition.editor_position = {260.0f, 44.0f + index * 92.0f};
                changed = true;
            }
        };
        auto ensure_action_pos = [&](assets::TriggerAction& action, int index) {
            if (action.editor_position.x <= 0.0f && action.editor_position.y <= 0.0f) {
                action.editor_position = {620.0f, 44.0f + index * 92.0f};
                changed = true;
            }
        };

        std::vector<NodeHit> hits;
        hits.reserve(trigger.conditions.size() + trigger.actions.size());
        for (int i = 0; i < static_cast<int>(trigger.conditions.size()); ++i) {
            auto& condition = trigger.conditions[static_cast<std::size_t>(i)];
            ensure_condition_pos(condition, i);
            ImVec2 pos{canvas_min.x + condition.editor_position.x, canvas_min.y + condition.editor_position.y};
            ImVec2 size{190.0f, 72.0f};
            ImVec2 rect_min = pos;
            ImVec2 rect_max{pos.x + size.x, pos.y + size.y};
            const bool selected = trigger_graph_selection_kind_ == GraphNodeSelectionKind::Condition && trigger_graph_selection_index_ == i;
            draw_list->AddBezierCubic(
                {start_rect_max.x, rect_center(start_rect_min, start_rect_max).y},
                {start_rect_max.x + 70.0f, rect_center(start_rect_min, start_rect_max).y},
                {rect_min.x - 70.0f, rect_center(rect_min, rect_max).y},
                {rect_min.x, rect_center(rect_min, rect_max).y},
                IM_COL32(120, 180, 255, 180),
                2.0f);
            draw_list->AddRectFilled(rect_min, rect_max, selected ? IM_COL32(46, 82, 124, 255) : IM_COL32(28, 58, 94, 255), 8.0f);
            draw_list->AddRect(rect_min, rect_max, selected ? IM_COL32(255, 214, 94, 255) : IM_COL32(100, 170, 255, 255), 8.0f, 0, 2.0f);
            draw_list->AddText({rect_min.x + 12.0f, rect_min.y + 10.0f}, IM_COL32(255, 255, 255, 255), ("Condition " + std::to_string(i + 1)).c_str());
            draw_list->AddText({rect_min.x + 12.0f, rect_min.y + 34.0f}, IM_COL32(210, 232, 255, 255), condition.function.c_str());
            hits.push_back(NodeHit{GraphNodeSelectionKind::Condition, i, rect_min, rect_max});
        }
        for (int i = 0; i < static_cast<int>(trigger.actions.size()); ++i) {
            auto& action = trigger.actions[static_cast<std::size_t>(i)];
            ensure_action_pos(action, i);
            ImVec2 pos{canvas_min.x + action.editor_position.x, canvas_min.y + action.editor_position.y};
            ImVec2 size{210.0f, 72.0f};
            ImVec2 rect_min = pos;
            ImVec2 rect_max{pos.x + size.x, pos.y + size.y};
            const bool selected = trigger_graph_selection_kind_ == GraphNodeSelectionKind::Action && trigger_graph_selection_index_ == i;
            const ImVec2 source = trigger.conditions.empty()
                ? ImVec2{start_rect_max.x, rect_center(start_rect_min, start_rect_max).y}
                : ImVec2{canvas_min.x + trigger.conditions.front().editor_position.x + 190.0f,
                         canvas_min.y + trigger.conditions.front().editor_position.y + 36.0f};
            draw_list->AddBezierCubic(
                source,
                {source.x + 80.0f, source.y},
                {rect_min.x - 80.0f, rect_center(rect_min, rect_max).y},
                {rect_min.x, rect_center(rect_min, rect_max).y},
                IM_COL32(140, 255, 170, 180),
                2.0f);
            draw_list->AddRectFilled(rect_min, rect_max, selected ? IM_COL32(38, 104, 68, 255) : IM_COL32(26, 74, 48, 255), 8.0f);
            draw_list->AddRect(rect_min, rect_max, selected ? IM_COL32(255, 214, 94, 255) : IM_COL32(116, 228, 150, 255), 8.0f, 0, 2.0f);
            draw_list->AddText({rect_min.x + 12.0f, rect_min.y + 10.0f}, IM_COL32(255, 255, 255, 255), ("Action " + std::to_string(i + 1)).c_str());
            draw_list->AddText({rect_min.x + 12.0f, rect_min.y + 34.0f}, IM_COL32(220, 255, 232, 255), action.function.c_str());
            hits.push_back(NodeHit{GraphNodeSelectionKind::Action, i, rect_min, rect_max});
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragging_trigger_graph_node_ = false;
            trigger_graph_selection_kind_ = GraphNodeSelectionKind::None;
            trigger_graph_selection_index_ = -1;
            for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
                if (rect_contains(it->min, it->max, ImGui::GetIO().MousePos)) {
                    trigger_graph_selection_kind_ = it->kind;
                    trigger_graph_selection_index_ = it->index;
                    dragging_trigger_graph_node_ = true;
                    trigger_graph_drag_offset_ = {
                        mouse_local.x - (it->min.x - canvas_min.x),
                        mouse_local.y - (it->min.y - canvas_min.y)
                    };
                    break;
                }
            }
        }

        if (dragging_trigger_graph_node_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (trigger_graph_selection_kind_ == GraphNodeSelectionKind::Condition &&
                    trigger_graph_selection_index_ >= 0 &&
                    trigger_graph_selection_index_ < static_cast<int>(trigger.conditions.size())) {
                    trigger.conditions[static_cast<std::size_t>(trigger_graph_selection_index_)].editor_position = {
                        std::clamp(mouse_local.x - trigger_graph_drag_offset_.x, 8.0f, canvas_size.x - 210.0f),
                        std::clamp(mouse_local.y - trigger_graph_drag_offset_.y, 8.0f, canvas_size.y - 84.0f)
                    };
                    changed = true;
                } else if (trigger_graph_selection_kind_ == GraphNodeSelectionKind::Action &&
                           trigger_graph_selection_index_ >= 0 &&
                           trigger_graph_selection_index_ < static_cast<int>(trigger.actions.size())) {
                    trigger.actions[static_cast<std::size_t>(trigger_graph_selection_index_)].editor_position = {
                        std::clamp(mouse_local.x - trigger_graph_drag_offset_.x, 8.0f, canvas_size.x - 226.0f),
                        std::clamp(mouse_local.y - trigger_graph_drag_offset_.y, 8.0f, canvas_size.y - 84.0f)
                    };
                    changed = true;
                }
            } else {
                dragging_trigger_graph_node_ = false;
            }
        }

        if (ImGui::BeginPopupContextItem("TriggerGraphContext")) {
            if (ImGui::MenuItem("Add Condition Node")) {
                assets::TriggerCondition condition{"builtin", "always_true", "{}"};
                condition.editor_position = {mouse_local.x, mouse_local.y};
                trigger.conditions.push_back(std::move(condition));
                trigger_graph_selection_kind_ = GraphNodeSelectionKind::Condition;
                trigger_graph_selection_index_ = static_cast<int>(trigger.conditions.size() - 1);
                changed = true;
            }
            if (ImGui::MenuItem("Add Action Node")) {
                assets::TriggerAction action{"builtin", "log_message", "{\"message\":\"Triggered\"}"};
                action.editor_position = {mouse_local.x, mouse_local.y};
                trigger.actions.push_back(std::move(action));
                trigger_graph_selection_kind_ = GraphNodeSelectionKind::Action;
                trigger_graph_selection_index_ = static_cast<int>(trigger.actions.size() - 1);
                changed = true;
            }
            ImGui::EndPopup();
        }

        ImGui::Separator();
        if (trigger_graph_selection_kind_ == GraphNodeSelectionKind::Condition &&
            trigger_graph_selection_index_ >= 0 &&
            trigger_graph_selection_index_ < static_cast<int>(trigger.conditions.size())) {
            ImGui::TextUnformatted("Selected Condition");
            auto& condition = trigger.conditions[static_cast<std::size_t>(trigger_graph_selection_index_)];
            bool custom = !IsBuiltinTriggerScript(condition.script);
            if (ImGui::Checkbox("Custom Script", &custom)) {
                condition.script = custom ? "scripts/custom_trigger.py" : "builtin";
                if (!custom && condition.function.empty()) {
                    condition.function = "always_true";
                }
                changed = true;
            }
            if (custom) {
                changed |= EditString("Script", condition.script);
                changed |= EditString("Function", condition.function);
                changed |= EditJsonString("Args", condition.args_json);
            } else {
                changed |= DrawBuiltinConditionEditor(condition);
            }
            if (ImGui::Button("Remove Selected Condition")) {
                trigger.conditions.erase(trigger.conditions.begin() + trigger_graph_selection_index_);
                trigger_graph_selection_kind_ = GraphNodeSelectionKind::None;
                trigger_graph_selection_index_ = -1;
                changed = true;
            }
        } else if (trigger_graph_selection_kind_ == GraphNodeSelectionKind::Action &&
                   trigger_graph_selection_index_ >= 0 &&
                   trigger_graph_selection_index_ < static_cast<int>(trigger.actions.size())) {
            ImGui::TextUnformatted("Selected Action");
            auto& action = trigger.actions[static_cast<std::size_t>(trigger_graph_selection_index_)];
            bool custom = !IsBuiltinTriggerScript(action.script);
            if (ImGui::Checkbox("Custom Script", &custom)) {
                action.script = custom ? "scripts/custom_trigger.py" : "builtin";
                if (!custom && action.function.empty()) {
                    action.function = "log_message";
                }
                changed = true;
            }
            if (custom) {
                changed |= EditString("Script", action.script);
                changed |= EditString("Function", action.function);
                changed |= EditJsonString("Args", action.args_json);
            } else {
                changed |= DrawBuiltinActionEditor(action);
            }
            if (ImGui::Button("Remove Selected Action")) {
                trigger.actions.erase(trigger.actions.begin() + trigger_graph_selection_index_);
                trigger_graph_selection_kind_ = GraphNodeSelectionKind::None;
                trigger_graph_selection_index_ = -1;
                changed = true;
            }
        } else {
            ImGui::TextWrapped("Right click in the graph to add nodes. Drag nodes with the mouse. Select a node to edit its parameters below.");
        }

        if (ImGui::CollapsingHeader("Advanced List View")) {
            DrawTriggerLogicEditor(trigger.conditions, trigger.actions, changed);
        }
        ImGui::Separator();
        if (ImGui::Button("Save Trigger To Level")) {
            scene_.SaveLevelToDisk();
            status_message_ = "Trigger saved to level.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Save All")) {
            SaveAll();
            status_message_ = "Trigger and project saved.";
        }
        ImGui::TextDisabled("Trigger graph changes are auto-saved into the current .niso level.");
        if (changed) {
            pending_history_label_ = "Edit trigger graph";
            scene_.ResetSimulation(false);
            scene_.SaveLevelToDisk();
            status_message_ = "Trigger graph auto-saved.";
        }
    } else {
        ImGui::TextUnformatted("Select a trigger instance to edit its conditions and actions.");
    }
    ImGui::End();
}

void EditorApp::DrawLayersPanel() {
    if (!show_layers_panel_) {
        return;
    }
    if (!ImGui::Begin("[LAY] Layers###LayersWindow", &show_layers_panel_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.72f, 0.58f, 0.92f, 1.0f});

    auto& level = scene_.EditableLevel();
    int layers_flat_index = 0;

    auto sync_primary_from_selection = [&]() {
        if (hierarchy_selection_.empty()) {
            selection_kind_ = SelectionKind::None;
            selection_index_ = -1;
            return;
        }
        const auto active = hierarchy_selection_.back();
        selection_kind_ = active.kind;
        selection_index_ = active.index;
        if (active.kind == SelectionKind::Entity &&
            active.index >= 0 &&
            active.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            current_script_relative_ = scene_.EditableLevel().entities[static_cast<std::size_t>(active.index)].script;
            current_code_relative_ = current_script_relative_;
            LoadCurrentScript();
        }
    };

    auto select_layer_item = [&](SelectionKind kind, int index, int flat_index) {
        const bool additive = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
        if (additive) {
            const auto it = std::find_if(hierarchy_selection_.begin(), hierarchy_selection_.end(),
                [&](const HierarchySelectionItem& item) {
                    return item.kind == kind && item.index == index;
                });
            if (it == hierarchy_selection_.end()) {
                hierarchy_selection_.push_back({kind, index});
            } else {
                hierarchy_selection_.erase(it);
            }
            PruneHierarchySelection();
        } else {
            SetHierarchySelectionSingle(kind, index);
        }
        hierarchy_anchor_flat_index_ = flat_index;
        sync_primary_from_selection();
    };

    auto select_layer_group = [&](const std::vector<HierarchySelectionItem>& items, int flat_index) {
        if (items.empty()) {
            return;
        }
        const bool additive = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
        if (additive) {
            for (const auto& item : items) {
                if (!IsHierarchyItemSelected(item.kind, item.index)) {
                    hierarchy_selection_.push_back(item);
                }
            }
            PruneHierarchySelection();
        } else {
            ApplyHierarchySelection(items);
        }
        hierarchy_anchor_flat_index_ = flat_index;
        sync_primary_from_selection();
    };

    if (ImGui::TreeNodeEx("BackgroundLayers", ImGuiTreeNodeFlags_DefaultOpen, "Background / Parallax")) {
        for (int i = static_cast<int>(level.parallax_layers.size()) - 1; i >= 0; --i) {
            auto& layer = level.parallax_layers[static_cast<std::size_t>(i)];
            const bool selected = IsHierarchyItemSelected(SelectionKind::Parallax, i);
            ImGui::PushID(("parallax_layer_" + std::to_string(i)).c_str());
            if (ImGui::Selectable((layer.name + "##parallax").c_str(), selected)) {
                select_layer_item(SelectionKind::Parallax, i, layers_flat_index);
            }
            ++layers_flat_index;
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("NOVAISO_PARALLAX_LAYER_INDEX", &i, sizeof(i));
                ImGui::TextUnformatted(layer.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_PARALLAX_LAYER_INDEX")) {
                    const int source_index = *static_cast<const int*>(payload->Data);
                    if (source_index >= 0 && source_index < static_cast<int>(level.parallax_layers.size()) && source_index != i) {
                        auto moved = level.parallax_layers[static_cast<std::size_t>(source_index)];
                        level.parallax_layers.erase(level.parallax_layers.begin() + source_index);
                        int target_index = i;
                        if (source_index < i) {
                            --target_index;
                        }
                        level.parallax_layers.insert(level.parallax_layers.begin() + target_index, std::move(moved));
                        SetHierarchySelectionSingle(SelectionKind::Parallax, target_index);
                        pending_history_label_ = "Reorder parallax layer";
                        scene_.ResetSimulation(false);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    active_tile_layer_index_ = std::clamp(active_tile_layer_index_, 0, std::max(static_cast<int>(level.tile_layers.size()) - 1, 0));
    if (ImGui::TreeNodeEx("TileLayers", ImGuiTreeNodeFlags_DefaultOpen, "Tileset / Tile Layers")) {
        bool changed = false;
        changed |= EditString("Tileset Texture", level.tileset.texture);
        changed |= ImGui::InputInt("Tile Width", &level.tile_width);
        changed |= ImGui::InputInt("Tile Height", &level.tile_height);
        level.tile_width = std::max(level.tile_width, 1);
        level.tile_height = std::max(level.tile_height, 1);
        level.tileset.tile_width = level.tile_width;
        level.tileset.tile_height = level.tile_height;

        bool has_tileset_texture = !level.tileset.texture.empty();
        glm::ivec2 tileset_texture_size{0, 0};
        if (has_tileset_texture) {
            renderer::Texture2D& tileset_texture = asset_manager_.LoadTexture(level.tileset.texture);
            tileset_texture_size = tileset_texture.Size();
            const int previous_columns = level.tileset.columns;
            const int previous_rows = level.tileset.rows;
            SyncLevelTilesetLayout(level, tileset_texture_size);
            changed = changed || previous_columns != level.tileset.columns || previous_rows != level.tileset.rows;
            if (ImGui::Button("Auto Detect Grid")) {
                SyncLevelTilesetLayout(level, tileset_texture_size);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%d x %d px -> %d x %d tiles", tileset_texture_size.x, tileset_texture_size.y, level.tileset.columns, level.tileset.rows);
        }
        if (browser_selection_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(browser_selection_relative_))) {
            if (ImGui::Button("Use Selected Image As Tileset")) {
                level.tileset.texture = browser_selection_relative_;
                renderer::Texture2D& tileset_texture = asset_manager_.LoadTexture(level.tileset.texture);
                SyncLevelTilesetLayout(level, tileset_texture.Size());
                if (level.tile_layers.empty()) {
                    level.tile_layers.push_back({
                        .name = "Tiles",
                        .width = 64,
                        .height = 24,
                        .depth = 0.0f,
                        .visible = true,
                        .collidable = true,
                        .tiles = std::vector<int>(64 * 24, 0)
                    });
                }
                EnsureTileLayerStorage(level.tile_layers.front());
                active_tile_layer_index_ = 0;
                tile_paint_mode_ = true;
                selected_tileset_index_ = 0;
                changed = true;
                status_message_ = "Tileset assigned: " + level.tileset.texture;
            }
        }

        if (ImGui::Button("Add Tile Layer")) {
            const int default_width = !level.tile_layers.empty() ? level.tile_layers.front().width : 64;
            const int default_height = !level.tile_layers.empty() ? level.tile_layers.front().height : 24;
            assets::TileLayer layer;
            layer.name = "TileLayer_" + std::to_string(level.tile_layers.size());
            layer.width = std::max(default_width, 1);
            layer.height = std::max(default_height, 1);
            layer.depth = static_cast<float>(level.tile_layers.size()) * 0.05f;
            layer.visible = true;
            layer.collidable = level.tile_layers.empty();
            layer.tiles.assign(static_cast<std::size_t>(layer.width) * static_cast<std::size_t>(layer.height), 0);
            level.tile_layers.push_back(std::move(layer));
            active_tile_layer_index_ = static_cast<int>(level.tile_layers.size() - 1);
            tile_paint_mode_ = true;
            changed = true;
        }
        if (!level.tile_layers.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Remove Active Tile Layer")) {
                level.tile_layers.erase(level.tile_layers.begin() + active_tile_layer_index_);
                active_tile_layer_index_ = std::clamp(active_tile_layer_index_, 0, std::max(static_cast<int>(level.tile_layers.size()) - 1, 0));
                changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("Tile Paint Mode", &tile_paint_mode_);
        ImGui::SameLine();
        ImGui::TextDisabled(selected_tileset_index_ >= 0 ? "Tile #%d" : "Eraser", selected_tileset_index_ + 1);

        for (std::size_t i = 0; i < level.tile_layers.size(); ++i) {
            auto& tile_layer = level.tile_layers[i];
            EnsureTileLayerStorage(tile_layer);
            ImGui::PushID(static_cast<int>(i));
            const bool active = active_tile_layer_index_ == static_cast<int>(i);
            if (ImGui::Selectable(tile_layer.name.c_str(), active)) {
                active_tile_layer_index_ = static_cast<int>(i);
            }
            if (active) {
                changed |= EditString("Layer Name", tile_layer.name);
                changed |= ImGui::InputInt("Layer Width", &tile_layer.width);
                changed |= ImGui::InputInt("Layer Height", &tile_layer.height);
                changed |= ImGui::InputFloat("Layer Depth", &tile_layer.depth);
                changed |= ImGui::Checkbox("Layer Visible", &tile_layer.visible);
                changed |= ImGui::Checkbox("Layer Collidable", &tile_layer.collidable);
                if (ImGui::Button("Clear Active Layer")) {
                    std::fill(tile_layer.tiles.begin(), tile_layer.tiles.end(), 0);
                    changed = true;
                }
                EnsureTileLayerStorage(tile_layer);
            }
            ImGui::PopID();
        }

        if (has_tileset_texture) {
            renderer::Texture2D& tileset_texture = asset_manager_.LoadTexture(level.tileset.texture);
            const int total_tiles = TilesetTileCount(level);
            const float tile_button = 36.0f;
            const int palette_columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (tile_button + 6.0f)));
            ImGui::SeparatorText("Tileset Palette");
            if (ImGui::BeginChild("TilesetPalette", ImVec2(0.0f, 220.0f), true)) {
                ImGui::PushID("tile_eraser");
                if (ImGui::Selectable("Eraser", selected_tileset_index_ < 0, 0, ImVec2(tile_button * 1.5f, tile_button))) {
                    selected_tileset_index_ = -1;
                    tile_paint_mode_ = true;
                }
                ImGui::PopID();
                for (int tile_index = 0; tile_index < total_tiles; ++tile_index) {
                    if (tile_index % palette_columns != 0) {
                        ImGui::SameLine();
                    }
                    const glm::vec4 uv = TilesetTileUv(level, tile_index);
                    ImGui::PushID(tile_index);
                    const bool clicked = ImGui::ImageButton("##tile", reinterpret_cast<void*>(static_cast<intptr_t>(tileset_texture.Id())), ImVec2(tile_button, tile_button), ImVec2(uv.x, uv.w), ImVec2(uv.z, uv.y));
                    if (clicked) {
                        selected_tileset_index_ = tile_index;
                        tile_paint_mode_ = true;
                    }
                    if (selected_tileset_index_ == tile_index) {
                        const ImVec2 min = ImGui::GetItemRectMin();
                        const ImVec2 max = ImGui::GetItemRectMax();
                        ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(255, 214, 92, 255), 4.0f, 0, 2.0f);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Tile %d", tile_index + 1);
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
        } else {
            ImGui::TextDisabled("Assign a tileset image to start tile painting.");
        }

        if (changed) {
            pending_history_label_ = "Update tileset";
            scene_.ResetSimulation(false);
        }
        ImGui::TreePop();
    }

    std::vector<int> layer_values;
    layer_values.reserve(level.entities.size());
    for (const auto& entity : level.entities) {
        if (std::find(layer_values.begin(), layer_values.end(), entity.layer) == layer_values.end()) {
            layer_values.push_back(entity.layer);
        }
    }
    std::sort(layer_values.begin(), layer_values.end(), std::greater<int>());

    if (ImGui::TreeNodeEx("GameplayLayers", ImGuiTreeNodeFlags_DefaultOpen, "Gameplay Layers")) {
        if (layer_values.empty()) {
            ImGui::TextDisabled("No entity layers yet.");
        }

        for (int layer_value : layer_values) {
            ImGui::PushID(("entity_layer_bucket_" + std::to_string(layer_value)).c_str());
            const std::string label = "Layer " + std::to_string(layer_value);
            const bool open = ImGui::TreeNodeEx("layer_bucket", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label.c_str());
            const int bucket_flat_index = layers_flat_index++;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                std::vector<HierarchySelectionItem> bucket_items;
                for (int i = 0; i < static_cast<int>(level.entities.size()); ++i) {
                    if (level.entities[static_cast<std::size_t>(i)].layer == layer_value) {
                        bucket_items.push_back({SelectionKind::Entity, i});
                    }
                }
                select_layer_group(bucket_items, bucket_flat_index);
            }

            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("NOVAISO_ENTITY_LAYER_BUCKET", &layer_value, sizeof(layer_value));
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_ENTITY_LAYER_BUCKET")) {
                    const int source_layer = *static_cast<const int*>(payload->Data);
                    if (source_layer != layer_value) {
                        for (auto& entity : level.entities) {
                            if (entity.layer == source_layer) {
                                entity.layer = layer_value;
                            } else if (entity.layer == layer_value) {
                                entity.layer = source_layer;
                            }
                        }
                        pending_history_label_ = "Reorder gameplay layers";
                        scene_.ResetSimulation(false);
                    }
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_ENTITY_LAYER_ITEM")) {
                    const int entity_index = *static_cast<const int*>(payload->Data);
                    if (entity_index >= 0 && entity_index < static_cast<int>(level.entities.size())) {
                        level.entities[static_cast<std::size_t>(entity_index)].layer = layer_value;
                        SetHierarchySelectionSingle(SelectionKind::Entity, entity_index);
                        pending_history_label_ = "Move entity to layer";
                        scene_.ResetSimulation(false);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (open) {
                for (int i = 0; i < static_cast<int>(level.entities.size()); ++i) {
                    const auto& entity = level.entities[static_cast<std::size_t>(i)];
                    if (entity.layer != layer_value) {
                        continue;
                    }
                    const bool selected = IsHierarchyItemSelected(SelectionKind::Entity, i);
                    ImGui::PushID(("layer_entity_" + std::to_string(i)).c_str());
                    if (ImGui::Selectable((entity.id.empty() ? ("entity_" + std::to_string(i)) : entity.id).c_str(), selected)) {
                        select_layer_item(SelectionKind::Entity, i, layers_flat_index);
                    }
                    ++layers_flat_index;
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("NOVAISO_ENTITY_LAYER_ITEM", &i, sizeof(i));
                        ImGui::TextUnformatted(entity.id.c_str());
                        ImGui::EndDragDropSource();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("OverlayFolders", ImGuiTreeNodeFlags_DefaultOpen, "Lights / Audio")) {
        for (int i = 0; i < static_cast<int>(level.lights.size()); ++i) {
            const bool selected = IsHierarchyItemSelected(SelectionKind::Light, i);
            if (ImGui::Selectable((level.lights[static_cast<std::size_t>(i)].name + "##light").c_str(), selected)) {
                select_layer_item(SelectionKind::Light, i, layers_flat_index);
            }
            ++layers_flat_index;
        }
        for (int i = 0; i < static_cast<int>(level.audio_sources.size()); ++i) {
            const bool selected = IsHierarchyItemSelected(SelectionKind::AudioSource, i);
            if (ImGui::Selectable((level.audio_sources[static_cast<std::size_t>(i)].name + "##audio_source").c_str(), selected)) {
                select_layer_item(SelectionKind::AudioSource, i, layers_flat_index);
            }
            ++layers_flat_index;
        }
        for (int i = 0; i < static_cast<int>(level.audio_paks.size()); ++i) {
            const bool selected = IsHierarchyItemSelected(SelectionKind::AudioPak, i);
            if (ImGui::Selectable((level.audio_paks[static_cast<std::size_t>(i)].name + "##audio_pak").c_str(), selected)) {
                select_layer_item(SelectionKind::AudioPak, i, layers_flat_index);
            }
            ++layers_flat_index;
        }
        ImGui::TreePop();
    }

    ImGui::End();
}

void EditorApp::DrawAssetBrowser() {
    if (!show_asset_browser_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("BOX", Tr("editor.window.browser", "Content Browser"), "BrowserWindow").c_str(), &show_asset_browser_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.34f, 0.68f, 0.96f, 1.0f});

    auto import_from_disk = [&](const std::string& title) {
        const std::filesystem::path chosen = BrowseForFile(title);
        if (chosen.empty()) {
            return;
        }
        const auto imported = asset_manager_.ImportFile(chosen);
        InvalidateAssetBrowserCache();
        status_message_ = "Imported: " + imported.generic_string();
        SelectBrowserResource(BrowserSelectionKind::File, imported.generic_string());
    };

    if (ImGui::Button("+", {32.0f, 32.0f})) {
        ImGui::OpenPopup("BrowserAddMenu");
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        InvalidateAssetBrowserCache();
        status_message_ = "Content Browser refreshed.";
    }
    if (ImGui::BeginPopup("BrowserAddMenu")) {
        if (ImGui::MenuItem("Import Image")) {
            import_from_disk("Import image into NovaIso project");
        }
        if (ImGui::MenuItem("Import Audio")) {
            import_from_disk("Import audio into NovaIso project");
        }
        if (ImGui::MenuItem("Import Script")) {
            import_from_disk("Import script into NovaIso project");
        }
        if (ImGui::MenuItem("Import Shader")) {
            import_from_disk("Import shader into NovaIso project");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("editor.browser.new_sprite", "New Sprite").c_str())) {
            CreateSpriteResource();
        }
        if (ImGui::MenuItem(Tr("editor.browser.new_object", "New Object").c_str())) {
            CreateObjectResource();
        }
        if (ImGui::MenuItem(Tr("editor.browser.new_trigger", "New Trigger").c_str())) {
            CreateTriggerResource();
        }
        if (ImGui::MenuItem("New Animation")) {
            CreateAnimationResource();
        }
        if (ImGui::MenuItem("New Particle Effect")) {
            CreateParticleResource();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Add / Import");

    ImGui::Separator();
    ImGui::TextWrapped("%s", Tr("editor.browser.help", "Double-click or drag textures, objects and triggers into the viewport.").c_str());
    if (!placement_relative_.empty()) {
        ImGui::TextWrapped("%s %s", Tr("editor.browser.placement", "Placement mode:").c_str(), placement_relative_.c_str());
        ImGui::SameLine();
        if (ImGui::Button(Tr("editor.browser.clear_placement", "Clear Placement").c_str())) {
            placement_kind_ = BrowserSelectionKind::None;
            placement_relative_.clear();
        }
    }

    if (ImGui::CollapsingHeader("Built-In Tools", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.60f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.70f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.94f, 0.76f, 0.28f, 1.0f));
        if (ImGui::Button("LAMP", {72.0f, 72.0f})) {
            placement_kind_ = BrowserSelectionKind::Light;
            placement_relative_ = "__builtin_light__";
            status_message_ = "Placement mode: Lamp";
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            placement_kind_ = BrowserSelectionKind::Light;
            placement_relative_ = "__builtin_light__";
            status_message_ = "Placement mode: Lamp";
        }
        if (ImGui::BeginDragDropSource()) {
            static constexpr char builtin_light[] = "__builtin_light__";
            ImGui::TextUnformatted("Lamp");
            ImGui::SetDragDropPayload("NOVAISO_LIGHT_BUILTIN", builtin_light, sizeof(builtin_light));
            ImGui::EndDragDropSource();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextWrapped("Point lamp with radius, source size and scatter.");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.98f, 0.56f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.64f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.70f, 0.36f, 1.0f));
        if (ImGui::Button("FLASH", {72.0f, 72.0f})) {
            placement_kind_ = BrowserSelectionKind::Light;
            placement_relative_ = "__builtin_flashlight__";
            status_message_ = "Placement mode: Flashlight";
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            placement_kind_ = BrowserSelectionKind::Light;
            placement_relative_ = "__builtin_flashlight__";
            status_message_ = "Placement mode: Flashlight";
        }
        if (ImGui::BeginDragDropSource()) {
            static constexpr char builtin_flashlight[] = "__builtin_flashlight__";
            ImGui::TextUnformatted("Flashlight");
            ImGui::SetDragDropPayload("NOVAISO_LIGHT_BUILTIN", builtin_flashlight, sizeof(builtin_flashlight));
            ImGui::EndDragDropSource();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextWrapped("Directional RT flashlight with aim, cone angle and reach.");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.56f, 0.42f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.64f, 0.50f, 0.98f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.56f, 1.0f, 1.0f));
        if (ImGui::Button("AUDIO PAK", {92.0f, 72.0f})) {
            placement_kind_ = BrowserSelectionKind::AudioPak;
            placement_relative_ = "__builtin_audio_pak__";
            status_message_ = "Placement mode: Audio Pak";
        }
        if (ImGui::BeginDragDropSource()) {
            static constexpr char builtin_audio_pak[] = "__builtin_audio_pak__";
            ImGui::TextUnformatted("Audio Pak");
            ImGui::SetDragDropPayload("NOVAISO_AUDIO_PAK_BUILTIN", builtin_audio_pak, sizeof(builtin_audio_pak));
            ImGui::EndDragDropSource();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextWrapped("Playlist emitter with shuffle, sequential and repeat rules.");
    }

    auto draw_file_section = [&](const char* label_key, const char* label_fallback, const std::vector<std::filesystem::path>& files, BrowserSelectionKind selection_kind) {
        const std::string header = Tr(label_key, label_fallback);
        const bool open = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem((header + "SectionContext").c_str())) {
            if (label_key == std::string("editor.browser.images") && ImGui::MenuItem("Import Image")) {
                import_from_disk("Import image into NovaIso project");
            } else if (label_key == std::string("editor.browser.audio") && ImGui::MenuItem("Import Audio")) {
                import_from_disk("Import audio into NovaIso project");
            } else if (label_key == std::string("editor.browser.scripts") && ImGui::MenuItem("Import Script")) {
                import_from_disk("Import script into NovaIso project");
            } else if (label_key == std::string("editor.browser.shaders") && ImGui::MenuItem("Import Shader")) {
                import_from_disk("Import shader into NovaIso project");
            } else if (label_key == std::string("editor.browser.levels") && ImGui::MenuItem("Import Level")) {
                import_from_disk("Import level into NovaIso project");
            } else if (label_key == std::string("editor.browser.sprites") && ImGui::MenuItem("New Sprite Resource")) {
                CreateSpriteResource();
            } else if (label_key == std::string("editor.browser.objects") && ImGui::MenuItem("New Object Resource")) {
                CreateObjectResource();
            } else if (label_key == std::string("editor.browser.triggers") && ImGui::MenuItem("New Trigger Resource")) {
                CreateTriggerResource();
            } else if (label_key == std::string("editor.browser.animations") && ImGui::MenuItem("New Animation Resource")) {
                CreateAnimationResource();
            } else if (label_key == std::string("editor.browser.particles") && ImGui::MenuItem("New Particle Resource")) {
                CreateParticleResource();
            }
            ImGui::EndPopup();
        }
        if (!open) {
            return;
        }

        if (files.empty()) {
            ImGui::TextDisabled("No files.");
            return;
        }

        const float card_width = 112.0f;
        const ImVec2 icon_size{72.0f, 72.0f};
        const float card_height = icon_size.y + ImGui::GetTextLineHeightWithSpacing() * 3.0f + 12.0f;
        const float available = std::max(ImGui::GetContentRegionAvail().x, card_width);
        const int columns = std::max(1, static_cast<int>(available / card_width));
        if (!ImGui::BeginTable(header.c_str(), columns, ImGuiTableFlags_SizingFixedFit)) {
            return;
        }

        ImGuiListClipper clipper;
        const int row_count = static_cast<int>((files.size() + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
        clipper.Begin(row_count, card_height);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, card_height);
                for (int column = 0; column < columns; ++column) {
                    const std::size_t item_index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column);
                    ImGui::TableSetColumnIndex(column);
                    if (item_index >= files.size()) {
                        continue;
                    }

                    const auto& relative_path = files[item_index];
                    const std::string relative = relative_path.generic_string();
                    const bool selected = browser_selection_kind_ == selection_kind && browser_selection_relative_ == relative;
                    const bool image_preview = selection_kind == BrowserSelectionKind::File && IsImageExtension(relative_path);
                    const bool placeable = (selection_kind == BrowserSelectionKind::Object || selection_kind == BrowserSelectionKind::Trigger ||
                        selection_kind == BrowserSelectionKind::Animation || selection_kind == BrowserSelectionKind::Particle ||
                        (selection_kind == BrowserSelectionKind::File && (IsImageExtension(relative_path) || IsAudioExtension(relative_path))));

                    ImGui::PushID(relative.c_str());
                    ImGui::BeginGroup();

                    bool clicked = false;
                    if (image_preview) {
                        renderer::Texture2D& preview = asset_manager_.LoadTexture(relative);
                        clicked = ImGui::ImageButton("thumb", reinterpret_cast<void*>(static_cast<intptr_t>(preview.Id())), icon_size, ImVec2(0, 1), ImVec2(1, 0));
                    } else {
                        ImVec4 color = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
                        const char* icon_text = "FILE";
                        if (selection_kind == BrowserSelectionKind::Sprite) {
                            color = ImVec4(0.16f, 0.36f, 0.68f, 1.0f);
                            icon_text = "SPR";
                        } else if (selection_kind == BrowserSelectionKind::Object) {
                            color = ImVec4(0.18f, 0.55f, 0.34f, 1.0f);
                            icon_text = "OBJ";
                        } else if (selection_kind == BrowserSelectionKind::Trigger) {
                            color = ImVec4(0.72f, 0.48f, 0.12f, 1.0f);
                            icon_text = "TRG";
                        } else if (selection_kind == BrowserSelectionKind::Animation) {
                            color = ImVec4(0.18f, 0.66f, 0.66f, 1.0f);
                            icon_text = "ANM";
                        } else if (selection_kind == BrowserSelectionKind::Particle) {
                            color = ImVec4(0.98f, 0.50f, 0.18f, 1.0f);
                            icon_text = "PRT";
                        } else if (selection_kind == BrowserSelectionKind::File && IsAudioExtension(relative_path)) {
                            color = ImVec4(0.55f, 0.22f, 0.22f, 1.0f);
                            icon_text = "SND";
                        } else if (selection_kind == BrowserSelectionKind::File && IsScriptExtension(relative_path)) {
                            color = ImVec4(0.45f, 0.24f, 0.60f, 1.0f);
                            icon_text = "PY";
                        } else if (selection_kind == BrowserSelectionKind::File &&
                                   (relative_path.extension() == ".frag" || relative_path.extension() == ".vert" ||
                                    relative_path.extension() == ".glsl" || relative_path.extension() == ".shader" ||
                                    relative_path.extension() == ".nshader")) {
                            color = ImVec4(0.18f, 0.52f, 0.76f, 1.0f);
                            icon_text = "FX";
                        } else if (selection_kind == BrowserSelectionKind::File && relative_path.extension() == ".niso") {
                            color = ImVec4(0.22f, 0.48f, 0.62f, 1.0f);
                            icon_text = "LVL";
                        }

                        ImGui::PushStyleColor(ImGuiCol_Button, color);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x + 0.08f, color.y + 0.08f, color.z + 0.08f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x + 0.12f, color.y + 0.12f, color.z + 0.12f, 1.0f));
                        clicked = ImGui::Button(icon_text, icon_size);
                        ImGui::PopStyleColor(3);
                    }

                    if (selected) {
                        ImGui::GetWindowDrawList()->AddRect(
                            ImGui::GetItemRectMin(),
                            ImGui::GetItemRectMax(),
                            IM_COL32(255, 210, 90, 255),
                            6.0f,
                            0,
                            2.0f
                        );
                    }

                    if (clicked) {
                        SelectBrowserResource(selection_kind, relative);
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && placeable) {
                            placement_kind_ = selection_kind;
                            placement_relative_ = relative;
                            status_message_ = "Placement mode: " + relative;
                        }
                    }

                    if (ImGui::BeginPopupContextItem("BrowserItemContext")) {
                        if ((selection_kind == BrowserSelectionKind::File ||
                             selection_kind == BrowserSelectionKind::Sprite ||
                             selection_kind == BrowserSelectionKind::Object ||
                             selection_kind == BrowserSelectionKind::Trigger ||
                             selection_kind == BrowserSelectionKind::Animation ||
                             selection_kind == BrowserSelectionKind::Particle) &&
                            IsEditableTextFile(relative_path)) {
                            if (ImGui::MenuItem("Open In Code Editor")) {
                                OpenCodeEditorFile(relative_path);
                            }
                        }
                        if (placeable && ImGui::MenuItem("Place In Viewport")) {
                            placement_kind_ = selection_kind;
                            placement_relative_ = relative;
                            status_message_ = "Placement mode: " + relative;
                        }
                        if (ImGui::MenuItem("Delete File")) {
                            DeleteBrowserResource(selection_kind, relative);
                        }
                        ImGui::EndPopup();
                    }

                    if (ImGui::BeginDragDropSource()) {
                        ImGui::TextUnformatted(relative.c_str());
                        ImGui::SetDragDropPayload("NOVAISO_GENERIC_PATH", relative.c_str(), relative.size() + 1);
                        const char* payload_type = nullptr;
                        if (selection_kind == BrowserSelectionKind::Object) {
                            payload_type = "NOVAISO_OBJECT_PATH";
                        } else if (selection_kind == BrowserSelectionKind::Trigger) {
                            payload_type = "NOVAISO_TRIGGER_PATH";
                        } else if (selection_kind == BrowserSelectionKind::Animation) {
                            payload_type = "NOVAISO_ANIMATION_PATH";
                        } else if (selection_kind == BrowserSelectionKind::Particle) {
                            payload_type = "NOVAISO_PARTICLE_PATH";
                        } else if (selection_kind == BrowserSelectionKind::File && IsImageExtension(relative_path)) {
                            payload_type = "NOVAISO_TEXTURE_PATH";
                        } else if (selection_kind == BrowserSelectionKind::File && IsAudioExtension(relative_path)) {
                            payload_type = "NOVAISO_AUDIO_PATH";
                        }

                        if (payload_type != nullptr) {
                            ImGui::SetDragDropPayload(payload_type, relative.c_str(), relative.size() + 1);
                        }
                        ImGui::EndDragDropSource();
                    }

                    ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + 88.0f);
                    ImGui::TextUnformatted(relative_path.filename().string().c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndGroup();
                    ImGui::PopID();
                }
            }
        }

        ImGui::EndTable();
    };

    const auto project_files = std::filesystem::exists(project_root_ / "project.json")
        ? std::vector<std::filesystem::path>{std::filesystem::path("project.json")}
        : std::vector<std::filesystem::path>{};
    draw_file_section("editor.browser.images", "Images", EnumerateRelativeFiles("assets/images"), BrowserSelectionKind::File);
    draw_file_section("editor.browser.audio", "Audio", EnumerateRelativeFiles("assets/audio"), BrowserSelectionKind::File);
    draw_file_section("editor.browser.scripts", "Scripts", EnumerateRelativeFiles("scripts", ".py"), BrowserSelectionKind::File);
    draw_file_section("editor.browser.levels", "Levels", EnumerateRelativeFiles("levels", ".niso"), BrowserSelectionKind::File);
    draw_file_section("editor.browser.project_files", "Project Files",
        project_files,
        BrowserSelectionKind::File);
    draw_file_section("editor.browser.localization", "Localization", EnumerateRelativeFiles("localization", ".json"), BrowserSelectionKind::File);
    draw_file_section("editor.browser.shaders", "Shaders", EnumerateRelativeFiles("shaders"), BrowserSelectionKind::File);
    draw_file_section("editor.browser.ui", "HTML UI", EnumerateRelativeFiles("ui"), BrowserSelectionKind::File);
    draw_file_section("editor.browser.sprites", "Sprite Resources", EnumerateRelativeFiles("resources/sprites", ".nsprite"), BrowserSelectionKind::Sprite);
    draw_file_section("editor.browser.objects", "Object Resources", EnumerateRelativeFiles("resources/objects", ".nobject"), BrowserSelectionKind::Object);
    draw_file_section("editor.browser.triggers", "Trigger Resources", EnumerateRelativeFiles("resources/triggers", ".ntrigger"), BrowserSelectionKind::Trigger);
    draw_file_section("editor.browser.animations", "Animation Resources", EnumerateRelativeFiles("resources/animations", ".nanim"), BrowserSelectionKind::Animation);
    draw_file_section("editor.browser.particles", "Particle Resources", EnumerateRelativeFiles("resources/particles", ".nparticle"), BrowserSelectionKind::Particle);

    ImGui::End();
}

void EditorApp::DrawResourceInspector() {
    if (!show_resource_inspector_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("RES", Tr("editor.window.inspector", "Resource Inspector"), "InspectorWindow").c_str(), &show_resource_inspector_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.26f, 0.76f, 0.66f, 1.0f});

    if (browser_selection_relative_.empty()) {
        ImGui::TextUnformatted(Tr("editor.inspector.empty", "Select a file or engine resource from the Content Browser.").c_str());
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(browser_selection_relative_.c_str());
    ImGui::Separator();

    if (browser_selection_kind_ == BrowserSelectionKind::File) {
        const std::filesystem::path selected_path(browser_selection_relative_);
        if (IsImageExtension(selected_path)) {
            renderer::Texture2D& texture = asset_manager_.LoadTexture(browser_selection_relative_);
            const glm::ivec2 size = texture.Size();
            ImGui::Text("Image: %d x %d", size.x, size.y);
            ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(texture.Id())), ImVec2(192.0f, 192.0f), ImVec2(0, 1), ImVec2(1, 0));
            if (ImGui::Button(Tr("editor.inspector.create_sprite", "Create Sprite Resource").c_str())) {
                CreateSpriteFromTexture(browser_selection_relative_);
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr("editor.inspector.create_object", "Create Object Resource").c_str())) {
                CreateObjectFromTexture(browser_selection_relative_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Create Particle Resource")) {
                CreateParticleResource();
            }
            if (ImGui::Button(Tr("editor.inspector.place_image", "Place In Viewport").c_str())) {
                placement_kind_ = BrowserSelectionKind::File;
                placement_relative_ = browser_selection_relative_;
            }
            ImGui::SameLine();
            if (ImGui::Button("Use As Tileset")) {
                auto& level = scene_.EditableLevel();
                level.tileset.texture = browser_selection_relative_;
                SyncLevelTilesetLayout(level, texture.Size());
                if (level.tile_layers.empty()) {
                    level.tile_layers.push_back({
                        .name = "Tiles",
                        .width = 64,
                        .height = 24,
                        .depth = 0.0f,
                        .visible = true,
                        .collidable = true,
                        .tiles = std::vector<int>(64 * 24, 0)
                    });
                }
                EnsureTileLayerStorage(level.tile_layers.front());
                active_tile_layer_index_ = 0;
                selected_tileset_index_ = 0;
                tile_paint_mode_ = true;
                scene_.ResetSimulation(false);
                status_message_ = "Tileset assigned: " + browser_selection_relative_;
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Tile Palette")) {
                show_layers_panel_ = true;
                tile_paint_mode_ = true;
            }
        } else if (IsAudioExtension(selected_path)) {
            ImGui::TextUnformatted(Tr("editor.inspector.audio_info", "Audio asset. Attach this path to objects or play it from Python.").c_str());
            const std::string extension = selected_path.extension().string();
            const bool music_like = extension == ".ogg" || extension == ".mp3";
            if (music_like) {
                static bool loop_preview = true;
                if (ImGui::Button("Play")) {
                    asset_manager_.PlayMusic(browser_selection_relative_, false);
                }
                ImGui::SameLine();
                if (ImGui::Button("Play Loop")) {
                    asset_manager_.PlayMusic(browser_selection_relative_, loop_preview);
                }
                ImGui::SameLine();
                ImGui::Checkbox("Loop", &loop_preview);
            } else {
                if (ImGui::Button("Play")) {
                    asset_manager_.PlaySound(browser_selection_relative_);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                asset_manager_.StopAllAudio();
            }
            if (ImGui::Button("Place Audio Source")) {
                placement_kind_ = BrowserSelectionKind::File;
                placement_relative_ = browser_selection_relative_;
            }
        } else if (selected_path.extension() == ".nshader") {
            ImGui::TextUnformatted("Shader designer preset. Load it from the Shader Stack panel to regenerate or tweak the effect.");
            if (ImGui::Button("Open Shader Stack")) {
                show_shader_panel_ = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Open In Code Editor")) {
                OpenCodeEditorFile(browser_selection_relative_);
            }
        } else if (selected_path.extension() == ".frag" || selected_path.extension() == ".vert" ||
                   selected_path.extension() == ".glsl" || selected_path.extension() == ".shader") {
            ImGui::TextUnformatted("Shader source file. Add the fragment shader name to the Shader Stack window to preview it.");
            if (ImGui::Button("Open Shader Stack")) {
                show_shader_panel_ = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Open In Code Editor")) {
                OpenCodeEditorFile(browser_selection_relative_);
            }
        } else if (IsScriptExtension(selected_path)) {
            ImGui::TextUnformatted(Tr("editor.inspector.script_info", "Python script. Attach it to objects or entities.").c_str());
        } else {
            ImGui::TextUnformatted(Tr("editor.inspector.generic_info", "Generic project file.").c_str());
        }
    } else if (browser_selection_kind_ == BrowserSelectionKind::Sprite) {
        bool changed = false;
        changed |= EditString("Name", selected_sprite_.name);
        changed |= EditString("Default Animation", selected_sprite_.default_animation);
        changed |= ImGui::InputInt2("Canvas Size", &selected_sprite_.canvas_size.x);
        selected_sprite_.canvas_size.x = std::max(selected_sprite_.canvas_size.x, 1);
        selected_sprite_.canvas_size.y = std::max(selected_sprite_.canvas_size.y, 1);
        changed |= ImGui::InputFloat2("Pivot", &selected_sprite_.pivot.x);
        changed |= ImGui::ColorEdit4("Tint", &selected_sprite_.tint.x);
        if (ImGui::Button(Tr("editor.inspector.open_sprite_editor", "Open Sprite Editor").c_str())) {
            status_message_ = "Sprite Editor: " + browser_selection_relative_;
        }
        resource_dirty_ = resource_dirty_ || changed;
    } else if (browser_selection_kind_ == BrowserSelectionKind::Object) {
        bool changed = false;
        changed |= EditString("Name", selected_object_.name);
        selected_object_.preset = NormalizeObjectPreset(selected_object_.preset);
        int preset_index = ObjectPresetIndex(selected_object_.preset);
        if (ImGui::BeginCombo("Preset", kObjectPresetOptions[static_cast<std::size_t>(preset_index)].label)) {
            for (int option_index = 0; option_index < static_cast<int>(kObjectPresetOptions.size()); ++option_index) {
                const auto& option = kObjectPresetOptions[static_cast<std::size_t>(option_index)];
                const bool selected = preset_index == option_index;
                if (ImGui::Selectable(option.label, selected)) {
                    selected_object_.preset = option.value;
                    ApplyObjectPresetDefaults(selected_object_);
                    changed = true;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        changed |= EditString("Sprite Resource", selected_object_.sprite);
        changed |= EditString("Default Animation", selected_object_.default_animation);
        changed |= ImGui::InputFloat2("Size", &selected_object_.size.x);
        changed |= ImGui::ColorEdit4("Tint", &selected_object_.tint.x);
        changed |= ImGui::InputInt("Layer", &selected_object_.layer);
        changed |= ImGui::Checkbox("Dynamic", &selected_object_.dynamic);
        changed |= ImGui::Checkbox("Collidable", &selected_object_.collidable);
        changed |= ImGui::SliderFloat("Reflection", &selected_object_.reflection, 0.0f, 1.5f, "%.2f");
        ImGui::Separator();
        ImGui::TextUnformatted("Surface Relief");
        changed |= ImGui::Checkbox("Bump Mapping", &selected_object_.relief_enabled);
        changed |= EditString("Normal Map", selected_object_.normal_map);
        changed |= EditString("Height Map", selected_object_.height_map);
        changed |= EditString("Displacement Map", selected_object_.displacement_map);
        if (ImGui::Button("Generate Texture Maps")) {
            const auto preview_texture = ResolvePreviewTextureForObjectAsset(project_root_, selected_object_);
            if (preview_texture.has_value()) {
                const auto generated = GenerateReliefMapsFromTexture(project_root_, *preview_texture, selected_object_.name.empty() ? "object_relief" : selected_object_.name);
                if (generated.has_value()) {
                    selected_object_.normal_map = generated->normal_map;
                    selected_object_.height_map = generated->height_map;
                    selected_object_.displacement_map = generated->displacement_map;
                    selected_object_.relief_enabled = generated->settings.enabled;
                    selected_object_.bump_strength = generated->settings.bump_strength;
                    selected_object_.relief_depth = generated->settings.relief_depth;
                    selected_object_.parallax_depth = generated->settings.parallax_depth;
                    selected_object_.relief_contrast = generated->settings.relief_contrast;
                    changed = true;
                    status_message_ = "Generated texture maps for object resource.";
                } else {
                    status_message_ = "Failed to generate maps for object resource.";
                }
            } else {
                status_message_ = "Object resource has no preview texture to generate maps from.";
            }
        }
        if (selected_object_.relief_enabled) {
            changed |= ImGui::SliderFloat("Bump Strength", &selected_object_.bump_strength, 0.0f, 4.0f, "%.2f");
            changed |= ImGui::SliderFloat("Relief Depth", &selected_object_.relief_depth, 0.0f, 0.12f, "%.3f");
            changed |= ImGui::SliderFloat("Texture Parallax", &selected_object_.parallax_depth, 0.0f, 0.08f, "%.3f");
            changed |= ImGui::SliderFloat("Relief Contrast", &selected_object_.relief_contrast, 0.2f, 4.0f, "%.2f");
        }
        changed |= ImGui::Checkbox("Pseudo 3D", &selected_object_.pseudo_3d);
        changed |= ImGui::InputFloat2("Collider Offset", &selected_object_.collider_offset.x);
        changed |= ImGui::InputFloat2("Collider Size", &selected_object_.collider_size.x);
        changed |= ImGui::InputFloat("Pseudo 3D Height", &selected_object_.pseudo_3d_height);
        selected_object_.reflection = std::clamp(selected_object_.reflection, 0.0f, 1.5f);
        selected_object_.bump_strength = std::clamp(selected_object_.bump_strength, 0.0f, 4.0f);
        selected_object_.relief_depth = std::clamp(selected_object_.relief_depth, 0.0f, 0.12f);
        selected_object_.parallax_depth = std::clamp(selected_object_.parallax_depth, 0.0f, 0.08f);
        selected_object_.relief_contrast = std::clamp(selected_object_.relief_contrast, 0.2f, 4.0f);
        selected_object_.pseudo_3d_height = std::max(selected_object_.pseudo_3d_height, 0.0f);
        changed |= EditString("Sound", selected_object_.sound);
        changed |= EditString("Script", selected_object_.script);
        if (selected_object_.preset != "none") {
            ImGui::TextDisabled("Preset writes hidden gameplay properties into this object automatically.");
        }
        if (ImGui::Button(Tr("editor.inspector.place_object", "Place In Viewport").c_str())) {
            placement_kind_ = BrowserSelectionKind::Object;
            placement_relative_ = browser_selection_relative_;
        }
        resource_dirty_ = resource_dirty_ || changed;
    } else if (browser_selection_kind_ == BrowserSelectionKind::Trigger) {
        bool changed = false;
        changed |= EditString("Name", selected_trigger_.name);
        changed |= ImGui::InputFloat2("Default Size", &selected_trigger_.default_size.x);
        changed |= ImGui::ColorEdit4("Color", &selected_trigger_.color.x);
        changed |= ImGui::Checkbox("Once", &selected_trigger_.once);
        changed |= ImGui::Checkbox("Enabled", &selected_trigger_.enabled);
        DrawTriggerLogicEditor(selected_trigger_.conditions, selected_trigger_.actions, changed);
        if (ImGui::Button(Tr("editor.inspector.place_trigger", "Place In Viewport").c_str())) {
            placement_kind_ = BrowserSelectionKind::Trigger;
            placement_relative_ = browser_selection_relative_;
        }
        resource_dirty_ = resource_dirty_ || changed;
    } else if (browser_selection_kind_ == BrowserSelectionKind::Animation) {
        bool changed = false;
        changed |= EditString("Name", selected_animation_.name);
        changed |= ImGui::InputFloat("Duration", &selected_animation_.duration);
        changed |= EditString("Preview Texture", selected_animation_.preview_texture);
        changed |= ImGui::Checkbox("Affect Position", &selected_animation_.affect_position);
        changed |= ImGui::Checkbox("Affect Opacity", &selected_animation_.affect_opacity);
        changed |= ImGui::Checkbox("Affect Scale", &selected_animation_.affect_scale);
        changed |= ImGui::Checkbox("Affect Rotation", &selected_animation_.affect_rotation);
        changed |= ImGui::Checkbox("Affect Skew", &selected_animation_.affect_skew);
        selected_animation_.duration = std::max(selected_animation_.duration, 0.05f);
        if (browser_selection_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(browser_selection_relative_))) {
            if (ImGui::Button("Use Selected Image As Preview")) {
                selected_animation_.preview_texture = browser_selection_relative_;
                changed = true;
            }
        }
        if (ImGui::Button("Open Animation Editor")) {
            show_animation_editor_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Place On Scene")) {
            placement_kind_ = BrowserSelectionKind::Animation;
            placement_relative_ = browser_selection_relative_;
        }
        resource_dirty_ = resource_dirty_ || changed;
    } else if (browser_selection_kind_ == BrowserSelectionKind::Particle) {
        bool changed = false;
        changed |= EditString("Name", selected_particle_.name);
        selected_particle_.preset = NormalizeParticlePreset(selected_particle_.preset);
        int preset_index = ParticlePresetIndex(selected_particle_.preset);
        if (ImGui::BeginCombo("Preset", kParticlePresetOptions[static_cast<std::size_t>(preset_index)].label)) {
            for (int option_index = 0; option_index < static_cast<int>(kParticlePresetOptions.size()); ++option_index) {
                const auto& option = kParticlePresetOptions[static_cast<std::size_t>(option_index)];
                const bool selected = preset_index == option_index;
                if (ImGui::Selectable(option.label, selected)) {
                    selected_particle_.preset = option.value;
                    ApplyParticlePresetDefaults(selected_particle_);
                    changed = true;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        changed |= EditString("Texture", selected_particle_.texture);
        changed |= EditString("Sound", selected_particle_.sound);
        changed |= ImGui::ColorEdit4("Start Color", &selected_particle_.start_color.x);
        changed |= ImGui::ColorEdit4("End Color", &selected_particle_.end_color.x);
        changed |= ImGui::InputFloat2("Acceleration", &selected_particle_.acceleration.x);
        changed |= ImGui::InputFloat2("Start Size", &selected_particle_.start_size.x);
        changed |= ImGui::InputFloat2("End Size", &selected_particle_.end_size.x);
        changed |= ImGui::SliderFloat("Angle Min", &selected_particle_.velocity_angle_min, -180.0f, 180.0f, "%.1f");
        changed |= ImGui::SliderFloat("Angle Max", &selected_particle_.velocity_angle_max, -180.0f, 180.0f, "%.1f");
        changed |= ImGui::InputFloat("Speed Min", &selected_particle_.speed_min);
        changed |= ImGui::InputFloat("Speed Max", &selected_particle_.speed_max);
        changed |= ImGui::InputFloat("Lifetime Min", &selected_particle_.lifetime_min);
        changed |= ImGui::InputFloat("Lifetime Max", &selected_particle_.lifetime_max);
        changed |= ImGui::InputFloat("Spawn Rate", &selected_particle_.spawn_rate);
        changed |= ImGui::InputInt("Burst Count", &selected_particle_.burst_count);
        changed |= ImGui::InputFloat("Emission Radius", &selected_particle_.emission_radius);
        changed |= ImGui::InputFloat("Drag", &selected_particle_.drag);
        changed |= ImGui::InputFloat("Angular Velocity Min", &selected_particle_.angular_velocity_min);
        changed |= ImGui::InputFloat("Angular Velocity Max", &selected_particle_.angular_velocity_max);
        changed |= ImGui::Checkbox("Loop", &selected_particle_.loop);
        changed |= ImGui::Checkbox("Additive", &selected_particle_.additive);
        changed |= ImGui::Checkbox("Align To Velocity", &selected_particle_.align_to_velocity);
        selected_particle_.speed_min = std::max(selected_particle_.speed_min, 0.0f);
        selected_particle_.speed_max = std::max(selected_particle_.speed_max, selected_particle_.speed_min);
        selected_particle_.lifetime_min = std::max(selected_particle_.lifetime_min, 0.02f);
        selected_particle_.lifetime_max = std::max(selected_particle_.lifetime_max, selected_particle_.lifetime_min);
        selected_particle_.spawn_rate = std::max(selected_particle_.spawn_rate, 0.0f);
        selected_particle_.burst_count = std::max(selected_particle_.burst_count, 0);
        selected_particle_.emission_radius = std::max(selected_particle_.emission_radius, 0.0f);
        selected_particle_.drag = std::max(selected_particle_.drag, 0.0f);
        if (browser_selection_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(browser_selection_relative_))) {
            if (ImGui::Button("Use Selected Image As Particle Texture")) {
                selected_particle_.texture = browser_selection_relative_;
                selected_particle_.preset = "custom";
                changed = true;
            }
        }
        if (ImGui::Button("Apply Preset Defaults")) {
            ApplyParticlePresetDefaults(selected_particle_);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Place Emitter In Viewport")) {
            placement_kind_ = BrowserSelectionKind::Particle;
            placement_relative_ = browser_selection_relative_;
        }
        resource_dirty_ = resource_dirty_ || changed;
    }

    if ((browser_selection_kind_ == BrowserSelectionKind::Sprite ||
         browser_selection_kind_ == BrowserSelectionKind::Object ||
         browser_selection_kind_ == BrowserSelectionKind::Trigger ||
         browser_selection_kind_ == BrowserSelectionKind::Animation ||
         browser_selection_kind_ == BrowserSelectionKind::Particle) &&
        ImGui::Button(Tr("editor.resource.save", "Save Resource").c_str())) {
        resource_dirty_ = true;
        SaveSelectedResource();
        status_message_ = "Saved resource: " + browser_selection_relative_;
    }
    if ((browser_selection_kind_ == BrowserSelectionKind::Sprite ||
         browser_selection_kind_ == BrowserSelectionKind::Object ||
         browser_selection_kind_ == BrowserSelectionKind::Trigger ||
         browser_selection_kind_ == BrowserSelectionKind::Animation ||
         browser_selection_kind_ == BrowserSelectionKind::Particle) &&
        ImGui::Button(Tr("editor.resource.rename_file", "Rename File From Resource Name").c_str())) {
        if (RenameSelectedResourceFile()) {
            status_message_ = "Renamed resource file to match the resource name.";
        }
    }

    ImGui::End();
}

void EditorApp::DrawProjectSettingsPanel() {
    if (!show_project_settings_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("CFG", Tr("editor.window.project", "Project Settings"), "ProjectSettingsWindow").c_str(), &show_project_settings_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.62f, 0.55f, 0.98f, 1.0f});

    bool changed = false;
    changed |= EditString("Name", project_.name);
    changed |= EditString("Developer", project_.developer);
    changed |= EditString("Version", project_.version);
    changed |= EditString("Icon", project_.icon);
    changed |= EditString("Preview Image", project_.preview_image);
    changed |= ImGui::InputInt2("Game Viewport", &project_.game_viewport_size.x);
    project_.game_viewport_size.x = std::max(project_.game_viewport_size.x, 320);
    project_.game_viewport_size.y = std::max(project_.game_viewport_size.y, 180);
    ImGui::TextDisabled("Logical gameplay viewport. It controls world framing and camera scale.");
    changed |= ImGui::Checkbox("Splash Enabled", &project_.splash_enabled);
    changed |= EditString("Splash Image", project_.splash_image);
    changed |= EditString("Export Directory", project_.export_directory);
    int backend_index = project_.renderer_backend == "vulkan" ? 1 : 0;
    if (ImGui::Combo("Renderer Backend", &backend_index, "OpenGL\0Vulkan\0")) {
        project_.renderer_backend = backend_index == 1 ? "vulkan" : "opengl";
        changed = true;
    }
    ImGui::TextDisabled("Current renderer in this build is still OpenGL. Vulkan runtime: %s",
        core::VulkanBootstrap::RuntimeAvailable() ? core::VulkanBootstrap::RuntimeApiVersionString().c_str() : "not found");
    changed |= ImGui::Checkbox("Multithreaded Scene Prepass", &project_.multithreading_enabled);
    changed |= ImGui::InputInt("Worker Threads (0 = auto)", &project_.worker_threads);
    project_.worker_threads = std::max(project_.worker_threads, 0);
    ImGui::TextDisabled("Configured worker threads: %d", static_cast<int>(ResolveProjectWorkerCount(project_)));
    changed |= ImGui::Checkbox("Encrypt Archive", &project_.encrypt_archive);
    if (project_.encrypt_archive) {
        changed |= EditPasswordString("Archive Password", project_.archive_password, 256);
        if (ImGui::Button("Generate Password")) {
            project_.archive_password = GeneratePasswordString();
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Password length: %d", static_cast<int>(project_.archive_password.size()));
    }

    if (browser_selection_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(browser_selection_relative_))) {
        if (ImGui::Button(Tr("editor.project.use_selected_icon", "Use Selected Image As Icon").c_str())) {
            project_.icon = browser_selection_relative_;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Selected Image As Preview")) {
            project_.preview_image = browser_selection_relative_;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Selected Image As Splash")) {
            project_.splash_image = browser_selection_relative_;
            changed = true;
        }
    }

    if (ImGui::BeginCombo("Default Language", project_.default_language.c_str())) {
        for (const auto& language : project_.supported_languages) {
            const bool selected = project_.default_language == language;
            if (ImGui::Selectable(language.c_str(), selected)) {
                project_.default_language = language;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::BeginCombo("Editor Language", project_.editor_language.c_str())) {
        for (const auto& language : project_.supported_languages) {
            const bool selected = project_.editor_language == language;
            if (ImGui::Selectable(language.c_str(), selected)) {
                project_.editor_language = language;
                LoadEditorLocalization();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::TextWrapped("%s", Tr("editor.project.localization_info", "Localization files are stored in localization/editor_en.json and localization/editor_ru.json.").c_str());
    if (changed) {
        const float aspect = static_cast<float>(std::max(project_.game_viewport_size.x, 1)) /
                             static_cast<float>(std::max(project_.game_viewport_size.y, 1));
        scene_render_size_.x = std::max(scene_render_size_.x, 320);
        scene_render_size_.y = std::max(static_cast<int>(std::round(static_cast<float>(scene_render_size_.x) / aspect)), 180);
        EnsureProjectLayout();
        EnsureLocalizationFiles();
        ApplyProjectPresentation();
        core::ThreadPool::Shared().Configure(ResolveProjectWorkerCount(project_));
    }

    if (ImGui::Button(Tr("editor.project.save", "Save Project Settings").c_str())) {
        SaveAll();
        status_message_ = "Project settings saved.";
    }

    ImGui::End();
}

void EditorApp::DrawMenuDesignerPanel() {
    if (!show_menu_designer_) {
        return;
    }

    if (!ImGui::Begin("[MENU] Menu Designer###MenuDesignerWindow", &show_menu_designer_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.90f, 0.56f, 0.28f, 1.0f});

    int target = menu_edit_target_ == MenuEditTarget::Main ? 0 : 1;
    if (ImGui::Combo("Target", &target, "Main Menu\0Pause Menu\0")) {
        menu_edit_target_ = target == 0 ? MenuEditTarget::Main : MenuEditTarget::Pause;
    }

    auto& menu = EditedMenuDefinition();
    bool changed = false;
    changed |= ImGui::Checkbox("Enabled", &menu.enabled);
    changed |= EditString("Title", menu.title);
    changed |= EditString("Subtitle", menu.subtitle);
    changed |= ImGui::InputFloat2("Screen Position", &menu.position.x);
    changed |= ImGui::InputFloat2("Panel Size", &menu.panel_size.x);
    changed |= ImGui::ColorEdit4("Background", &menu.background.x);
    changed |= ImGui::ColorEdit4("Accent", &menu.accent.x);
    changed |= ImGui::ColorEdit4("Title Color", &menu.title_color.x);
    changed |= ImGui::ColorEdit4("Subtitle Color", &menu.subtitle_color.x);
    changed |= ImGui::InputFloat("Roundness", &menu.roundness);
    changed |= ImGui::InputFloat("Spacing", &menu.spacing);
    changed |= EditString("Background Music", menu.background_music);
    changed |= ImGui::Checkbox("Music Loop", &menu.music_loop);

    if (ImGui::Button("Add Button")) {
        menu.buttons.push_back({});
        changed = true;
    }

    static const std::array<const char*, 7> actions{
        "start_game", "resume_game", "restart_level", "open_main_menu", "open_settings", "toggle_camera_mode", "quit_game"
    };

    for (int i = 0; i < static_cast<int>(menu.buttons.size()); ++i) {
        ImGui::PushID(i);
        auto& button = menu.buttons[static_cast<std::size_t>(i)];
        ImGui::Separator();
        changed |= EditString("Label", button.label);
        int action_index = 0;
        for (int k = 0; k < static_cast<int>(actions.size()); ++k) {
            if (button.action == actions[static_cast<std::size_t>(k)]) {
                action_index = k;
                break;
            }
        }
        if (ImGui::BeginCombo("Action", actions[static_cast<std::size_t>(action_index)])) {
            for (int k = 0; k < static_cast<int>(actions.size()); ++k) {
                const bool selected = action_index == k;
                if (ImGui::Selectable(actions[static_cast<std::size_t>(k)], selected)) {
                    button.action = actions[static_cast<std::size_t>(k)];
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        changed |= EditString("Target", button.target);
        changed |= ImGui::InputFloat2("Size", &button.size.x);
        changed |= ImGui::ColorEdit4("Color", &button.color.x);
        changed |= ImGui::ColorEdit4("Hover", &button.hover_color.x);
        changed |= ImGui::ColorEdit4("Text Color", &button.text_color.x);
        changed |= ImGui::Checkbox("Enabled", &button.enabled);
        if (ImGui::Button("Remove Button")) {
            menu.buttons.erase(menu.buttons.begin() + i);
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    if (changed) {
        status_message_ = "Menu designer updated.";
    }
    ImGui::TextWrapped("Runtime uses Main Menu on startup when enabled, and Pause Menu on ESC when pause is enabled.");
    ImGui::End();
}

void EditorApp::DrawUiEditorPanel() {
    if (!show_ui_editor_) {
        return;
    }

    if (!ImGui::Begin(StableWindowLabel("UI", Tr("editor.window.ui", "UI Editor"), "UiEditorWindow").c_str(), &show_ui_editor_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.22f, 0.78f, 0.70f, 1.0f});

    const std::string html_enabled_text = Tr("editor.ui.html_enabled", "HTML UI Enabled");
    const std::string html_file_text = Tr("editor.ui.html_file", "HTML File");
    const std::string css_file_text = Tr("editor.ui.css_file", "CSS File");
    const std::string js_file_text = Tr("editor.ui.js_file", "JS File");
    const std::string create_template_text = Tr("editor.ui.create_template", "Create HTML UI Template");
    const std::string open_html_text = Tr("editor.ui.open_html", "Open HTML");
    const std::string open_css_text = Tr("editor.ui.open_css", "Open CSS");
    const std::string open_js_text = Tr("editor.ui.open_js", "Open JS");
    const std::string runtime_help_text = Tr(
        "editor.ui.runtime_help",
        "HTML runtime screens: `main-menu`, `pause-menu`, `settings-menu`, `hud`. Supported bindings: `${ui.some_key}`, `{project}`, `{version}`, `{level}`, `{camera_mode}`, `{last_message}`, `{setting_resolution}`, `{setting_fullscreen}`, `{setting_master_volume}`, `{setting_music_volume}`, `{setting_sound_volume}`, `{entity:player:coins}`.");
    const std::string legacy_overlay_text = Tr("editor.ui.legacy_overlay", "Legacy Widget Overlay");
    const std::string add_widget_text = Tr("editor.ui.add_widget", "Add Widget");
    const std::string legacy_help_text = Tr(
        "editor.ui.legacy_help",
        "These widgets are used when HTML UI is disabled or the `hud` screen is missing.");

    ImGui::Checkbox(html_enabled_text.c_str(), &project_.html_ui.enabled);
    EditString(html_file_text.c_str(), project_.html_ui.html_path, 512);
    EditString(css_file_text.c_str(), project_.html_ui.css_path, 512);
    EditString(js_file_text.c_str(), project_.html_ui.script_path, 512);

    if (ImGui::Button(create_template_text.c_str())) {
        CreateDefaultHtmlUiFiles();
        project_.html_ui.enabled = true;
        status_message_ = "Created default HTML UI files.";
    }
    ImGui::SameLine();
    if (ImGui::Button(open_html_text.c_str())) {
        OpenCodeEditorFile(project_.html_ui.html_path);
    }
    ImGui::SameLine();
    if (ImGui::Button(open_css_text.c_str())) {
        OpenCodeEditorFile(project_.html_ui.css_path);
    }
    ImGui::SameLine();
    if (ImGui::Button(open_js_text.c_str())) {
        OpenCodeEditorFile(project_.html_ui.script_path);
    }

    ImGui::TextWrapped("%s", runtime_help_text.c_str());

    if (ImGui::CollapsingHeader(legacy_overlay_text.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button(add_widget_text.c_str())) {
            project_.ui_elements.push_back({});
        }
        ImGui::SameLine();
        ImGui::TextWrapped("%s", legacy_help_text.c_str());

        for (int i = 0; i < static_cast<int>(project_.ui_elements.size()); ++i) {
            ImGui::PushID(i);
            auto& widget = project_.ui_elements[static_cast<std::size_t>(i)];
            ImGui::Separator();
            EditString("Name", widget.name);
            EditString("Text", widget.text, 1024);
            ImGui::InputFloat2("Anchor", &widget.anchor.x);
            ImGui::InputFloat2("Size", &widget.size.x);
            ImGui::InputFloat2("Padding", &widget.padding.x);
            ImGui::InputFloat("Scale", &widget.scale);
            ImGui::ColorEdit4("Background", &widget.background.x);
            ImGui::ColorEdit4("Accent", &widget.accent.x);
            ImGui::ColorEdit4("Text Color", &widget.text_color.x);
            ImGui::Checkbox("Enabled", &widget.enabled);
            widget.scale = std::max(widget.scale, 0.4f);
            if (ImGui::Button("Remove Widget")) {
                project_.ui_elements.erase(project_.ui_elements.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    ImGui::End();
}

void EditorApp::DrawShaderPanel() {
    if (!show_shader_panel_) {
        return;
    }

    if (!ImGui::Begin("[FX] Shader Stack###ShaderStackWindow", &show_shader_panel_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.78f, 0.42f, 0.96f, 1.0f});

    static ShaderDesignerState designer_state{};
    static std::string loaded_preset_relative;

    auto normalized_effect_name = [&](std::string name) {
        name = SanitizeName(std::move(name));
        if (name == "copy" || name == "sprite" || name == "light" || name == "post") {
            name += "_fx";
        }
        return name;
    };

    auto is_internal_shader = [&](std::string_view name) {
        return name == "copy" ||
               name == "sprite" ||
               name == "light" ||
               name == "post" ||
               name == "wet_reflection";
    };

    auto effect_enabled = [&](const std::string& name) {
        const auto& effects = scene_.EditableLevel().post_effects;
        return std::find(effects.begin(), effects.end(), name) != effects.end();
    };

    auto set_effect_enabled = [&](const std::string& name, bool enabled) {
        auto& effects = scene_.EditableLevel().post_effects;
        if (enabled) {
            if (std::find(effects.begin(), effects.end(), name) == effects.end()) {
                effects.push_back(name);
            }
        } else {
            effects.erase(std::remove(effects.begin(), effects.end(), name), effects.end());
        }
    };

    auto load_shader_preset = [&](const std::filesystem::path& relative_path) {
        const std::filesystem::path full_path = project_root_ / relative_path;
        if (!core::FileIO::Exists(full_path)) {
            status_message_ = "Shader preset not found: " + relative_path.generic_string();
            return;
        }
        const auto parsed = nlohmann::json::parse(core::FileIO::ReadText(full_path), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            status_message_ = "Failed to parse shader preset: " + relative_path.generic_string();
            return;
        }
        designer_state = {};
        ShaderDesignerFromJson(parsed, designer_state);
        designer_state.effect_name = normalized_effect_name(designer_state.effect_name);
        loaded_preset_relative = relative_path.generic_string();
        status_message_ = "Loaded shader preset: " + loaded_preset_relative;
    };

    ImGui::TextWrapped("Enabled post-process shaders are previewed live in the viewport.");
    if (ImGui::Checkbox("RT Enable", &scene_.EditableLevel().lighting.rt_enabled)) {
        if (scene_.EditableLevel().lighting.rt_enabled) {
            scene_.EditableLevel().lighting.enabled = true;
        }
        pending_history_label_ = "Toggle RT lighting";
    }
    ImGui::TextDisabled("RT here means real-time shader lighting for lamps, not ray tracing.");
    ImGui::SliderFloat("Shadow Strength", &scene_.EditableLevel().lighting.shadow_strength, 0.05f, 2.4f, "%.2f");
    ImGui::SliderFloat("Shadow Softness", &scene_.EditableLevel().lighting.shadow_softness, 0.15f, 3.5f, "%.2f");
    ImGui::SliderFloat("Shadow Diffusion", &scene_.EditableLevel().lighting.shadow_diffusion, 0.0f, 2.8f, "%.2f");
    ImGui::SliderInt("Shadow Samples", &scene_.EditableLevel().lighting.shadow_samples, 1, 12);
    scene_.EditableLevel().lighting.shadow_strength = std::clamp(scene_.EditableLevel().lighting.shadow_strength, 0.05f, 2.4f);
    scene_.EditableLevel().lighting.shadow_softness = std::clamp(scene_.EditableLevel().lighting.shadow_softness, 0.15f, 3.5f);
    scene_.EditableLevel().lighting.shadow_diffusion = std::clamp(scene_.EditableLevel().lighting.shadow_diffusion, 0.0f, 2.8f);
    scene_.EditableLevel().lighting.shadow_samples = std::clamp(scene_.EditableLevel().lighting.shadow_samples, 1, 12);
    const std::filesystem::path shader_root = ActiveShaderRoot();
    if (std::filesystem::exists(shader_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(shader_root)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".frag") {
                continue;
            }
            const std::string name = entry.path().stem().string();
            if (is_internal_shader(name)) {
                continue;
            }
            bool enabled = std::find(scene_.EditableLevel().post_effects.begin(), scene_.EditableLevel().post_effects.end(), name) != scene_.EditableLevel().post_effects.end();
            if (ImGui::Checkbox(name.c_str(), &enabled)) {
                auto& effects = scene_.EditableLevel().post_effects;
                if (enabled) {
                    if (std::find(effects.begin(), effects.end(), name) == effects.end()) {
                        effects.push_back(name);
                    }
                } else {
                    effects.erase(std::remove(effects.begin(), effects.end(), name), effects.end());
                }
            }
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Current Order");
    for (const auto& effect : scene_.EditableLevel().post_effects) {
        if (is_internal_shader(effect)) {
            continue;
        }
        ImGui::BulletText("%s", effect.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Shader Designer");
    ImGui::TextWrapped("Build reusable post-process shaders directly in the editor, save them as presets, then generate GLSL fragment files for the runtime.");

    const bool selected_preset_available =
        browser_selection_kind_ == BrowserSelectionKind::File &&
        std::filesystem::path(browser_selection_relative_).extension() == ".nshader";
    if (ImGui::Button("New Preset")) {
        designer_state = {};
        loaded_preset_relative.clear();
        status_message_ = "Shader designer reset.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Selected Preset")) {
        if (selected_preset_available) {
            load_shader_preset(browser_selection_relative_);
        } else {
            status_message_ = "Select a .nshader preset in the Content Browser first.";
        }
    }
    ImGui::SameLine();
    if (!loaded_preset_relative.empty()) {
        ImGui::TextDisabled("%s", loaded_preset_relative.c_str());
    } else {
        ImGui::TextDisabled("No preset loaded");
    }

    EditString("Effect Name", designer_state.effect_name);
    designer_state.effect_name = normalized_effect_name(designer_state.effect_name);
    ImGui::Checkbox("Grayscale", &designer_state.grayscale);
    if (designer_state.grayscale) {
        ImGui::SliderFloat("Grayscale Amount", &designer_state.grayscale_amount, 0.0f, 1.0f, "%.2f");
    }
    ImGui::SliderFloat("Brightness", &designer_state.brightness, 0.2f, 2.5f, "%.2f");
    ImGui::SliderFloat("Contrast", &designer_state.contrast, 0.2f, 2.5f, "%.2f");
    ImGui::SliderFloat("Saturation", &designer_state.saturation, 0.0f, 2.5f, "%.2f");
    ImGui::ColorEdit3("Tint", &designer_state.tint.x);
    ImGui::SliderFloat("Vignette", &designer_state.vignette, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("Film Grain", &designer_state.grain, 0.0f, 0.25f, "%.3f");
    ImGui::SliderFloat("Scanlines", &designer_state.scanlines, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Chromatic Aberration", &designer_state.chromatic_aberration, 0.0f, 0.02f, "%.4f");
    ImGui::SliderFloat("Wave Distortion", &designer_state.wave, 0.0f, 0.06f, "%.4f");
    if (designer_state.wave > 0.0001f) {
        ImGui::SliderFloat("Wave Frequency", &designer_state.wave_frequency, 1.0f, 40.0f, "%.2f");
    }
    ImGui::SliderFloat("Sharpen", &designer_state.sharpen, 0.0f, 1.25f, "%.2f");
    ImGui::SliderInt("Posterize Steps", &designer_state.posterize_steps, 0, 16);

    const std::string effect_name = normalized_effect_name(designer_state.effect_name);
    designer_state.effect_name = effect_name;
    const std::filesystem::path preset_relative = std::filesystem::path("shaders/designer") / (effect_name + ".nshader");
    const std::filesystem::path fragment_relative = std::filesystem::path("shaders") / (effect_name + ".frag");
    const std::string generated_fragment = BuildShaderDesignerFragment(designer_state);

    ImGui::TextDisabled("Fragment: %s", fragment_relative.generic_string().c_str());
    ImGui::TextDisabled("Preset: %s", preset_relative.generic_string().c_str());

    if (ImGui::Button("Save Preset")) {
        core::FileIO::EnsureDirectory(project_root_ / "shaders/designer");
        core::FileIO::WriteText(project_root_ / preset_relative, ShaderDesignerToJson(designer_state).dump(2));
        loaded_preset_relative = preset_relative.generic_string();
        SelectBrowserResource(BrowserSelectionKind::File, loaded_preset_relative);
        pending_history_label_ = "Save shader preset";
        status_message_ = "Saved shader preset: " + loaded_preset_relative;
    }
    ImGui::SameLine();
    if (ImGui::Button("Generate Shader")) {
        core::FileIO::EnsureDirectory(project_root_ / "shaders");
        core::FileIO::WriteText(project_root_ / fragment_relative, generated_fragment);
        set_effect_enabled(effect_name, true);
        SelectBrowserResource(BrowserSelectionKind::File, fragment_relative.generic_string());
        pending_history_label_ = "Generate shader";
        status_message_ = "Generated shader: " + fragment_relative.generic_string();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Generated File")) {
        OpenCodeEditorFile(fragment_relative);
    }

    bool enabled_in_level = effect_enabled(effect_name);
    if (ImGui::Checkbox("Enabled In Current Level", &enabled_in_level)) {
        set_effect_enabled(effect_name, enabled_in_level);
        pending_history_label_ = enabled_in_level ? "Enable post effect" : "Disable post effect";
    }

    ImGui::BeginChild("ShaderDesignerPreview", ImVec2(0.0f, 220.0f), true);
    ImGui::TextUnformatted(generated_fragment.c_str());
    ImGui::EndChild();

    ImGui::End();
}

void EditorApp::DrawAudioMixerPanel() {
    if (!show_audio_mixer_) {
        return;
    }

    if (!ImGui::Begin("[SND] Audio Mixer###AudioMixerWindow", &show_audio_mixer_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.26f, 0.74f, 0.92f, 1.0f});

    float master = asset_manager_.MasterVolume();
    float music = asset_manager_.MusicVolume();
    float sound = asset_manager_.SoundVolume();
    if (ImGui::SliderFloat("Master", &master, 0.0f, 1.0f, "%.2f")) {
        asset_manager_.SetMasterVolume(master);
    }
    if (ImGui::SliderFloat("Music", &music, 0.0f, 1.0f, "%.2f")) {
        asset_manager_.SetMusicVolume(music);
    }
    if (ImGui::SliderFloat("Sound FX", &sound, 0.0f, 1.0f, "%.2f")) {
        asset_manager_.SetSoundVolume(sound);
    }
    ImGui::Separator();
    ImGui::TextWrapped("Use Resource Inspector to preview the selected audio file. Mixer changes affect editor playback immediately.");
    if (ImGui::Button("Stop All Audio")) {
        asset_manager_.StopAllAudio();
    }

    ImGui::End();
}

void EditorApp::DrawScriptPanel() {
    if (!show_code_editor_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("PY", Tr("editor.window.code", "Code Editor"), "CodeEditorWindow").c_str(), &show_code_editor_)) {
        script_panel_hovered_ = false;
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.42f, 0.62f, 0.98f, 1.0f});
    script_panel_hovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    EditString("File Path", current_code_relative_, 1024);
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        LoadCurrentScript();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save File")) {
        SaveCurrentScript();
    }
    if (!current_code_relative_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", LanguageDefinitionForPath(current_code_relative_).mName.c_str());
    }

    python_editor_.Render("PythonEditor", ImVec2(-1.0f, -1.0f), false);
    if (python_editor_.IsTextChanged()) {
        script_dirty_ = true;
    }
    if (ImGui::BeginDragDropTarget()) {
        auto insert_payload = [&](const char* type) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type)) {
                const char* text = static_cast<const char*>(payload->Data);
                python_editor_.InsertText(std::string("\"") + text + "\"");
                script_dirty_ = true;
            }
        };
        insert_payload("NOVAISO_TEXTURE_PATH");
        insert_payload("NOVAISO_OBJECT_PATH");
        insert_payload("NOVAISO_TRIGGER_PATH");
        insert_payload("NOVAISO_GENERIC_PATH");
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
}

void EditorApp::DrawViewport() {
    if (!show_viewport_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("VIEW", Tr("editor.window.viewport", "Viewport"), "ViewportWindow").c_str(), &show_viewport_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.30f, 0.58f, 0.98f, 1.0f});

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("ViewportCanvas", available, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    const float aspect = static_cast<float>(scene_render_size_.x) / static_cast<float>(scene_render_size_.y);
    ImVec2 image_size = available;
    if (image_size.y <= 1.0f) {
        image_size.y = 1.0f;
    }
    if ((image_size.x / image_size.y) > aspect) {
        image_size.x = image_size.y * aspect;
    } else {
        image_size.y = image_size.x / aspect;
    }

    const ImVec2 image_min{
        canvas_min.x + (available.x - image_size.x) * 0.5f,
        canvas_min.y + (available.y - image_size.y) * 0.5f
    };
    const ImVec2 image_max{image_min.x + image_size.x, image_min.y + image_size.y};
    const glm::vec2 camera_viewport = scene_.Camera().Viewport();
    viewport_image_min_ = {image_min.x, image_min.y};
    viewport_image_size_ = {image_size.x, image_size.y};

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(canvas_min, {canvas_min.x + available.x, canvas_min.y + available.y}, IM_COL32(18, 18, 22, 255));
    draw_list->AddImage(reinterpret_cast<void*>(static_cast<intptr_t>(viewport_texture_)), image_min, image_max, ImVec2(0, 1), ImVec2(1, 0));
    draw_list->AddRect(image_min, image_max, IM_COL32(90, 90, 104, 255), 0.0f, 0, 1.0f);
    draw_list->AddText(
        {image_min.x + 10.0f, image_min.y + 10.0f},
        IM_COL32(230, 230, 235, 255),
        Tr("editor.viewport.help", "Middle mouse pans the camera. Left mouse selects or places assets.").c_str()
    );
    const std::string animation_counter = Tr("editor.viewport.animations", "Animations") + ": " +
        std::to_string(scene_.ActiveAnimatedEntityCount()) + "/" + std::to_string(scene_.AnimatedEntityCount()) +
        "  Clips: " + std::to_string(scene_.PlayingSceneAnimationCount()) + "/" + std::to_string(scene_.SceneAnimationCount()) +
        "  Particles: " + std::to_string(scene_.ActiveParticleCount());
    const ImVec2 animation_counter_size = ImGui::CalcTextSize(animation_counter.c_str());
    draw_list->AddText(
        {image_max.x - animation_counter_size.x - 10.0f, image_min.y + 10.0f},
        IM_COL32(120, 220, 255, 255),
        animation_counter.c_str()
    );
    if (!placement_relative_.empty()) {
        draw_list->AddText(
            {image_min.x + 10.0f, image_min.y + 30.0f},
            IM_COL32(255, 220, 120, 255),
            (Tr("editor.browser.placement", "Placement mode:") + std::string(" ") + placement_relative_).c_str()
        );
    }

    const auto world_to_viewport = [&](glm::vec2 world) {
        const glm::vec2 screen = scene_.Camera().WorldToScreen(world);
        return ImVec2{
            image_min.x + (screen.x / std::max(camera_viewport.x, 1.0f)) * image_size.x,
            image_min.y + (screen.y / std::max(camera_viewport.y, 1.0f)) * image_size.y
        };
    };

    const auto draw_world_rect = [&](glm::vec2 position, glm::vec2 size, ImU32 color, float thickness) {
        const ImVec2 a = world_to_viewport(position);
        const ImVec2 b = world_to_viewport(position + size);
        draw_list->AddRect(a, b, color, 0.0f, 0, thickness);
    };
    const auto draw_badge = [&](ImVec2 center, ImU32 color, const char* text) {
        const ImVec2 half_size{14.0f, 9.0f};
        draw_list->AddRectFilled({center.x - half_size.x, center.y - half_size.y},
                                 {center.x + half_size.x, center.y + half_size.y},
                                 IM_COL32(16, 20, 28, 220), 4.0f);
        draw_list->AddRect({center.x - half_size.x, center.y - half_size.y},
                           {center.x + half_size.x, center.y + half_size.y},
                           color, 4.0f, 0, 1.3f);
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        draw_list->AddText({center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f}, color, text);
    };

    if (viewport_mode_ == ViewportMode::Edit) {
        const int grid_w = std::max(scene_.Level().tile_width, 1);
        const int grid_h = std::max(scene_.Level().tile_height, 1);
        const glm::vec2 top_left_world = scene_.Camera().ScreenToWorld({0.0f, 0.0f});
        const glm::vec2 bottom_right_world = scene_.Camera().ScreenToWorld(camera_viewport);
        const int start_x = static_cast<int>(std::floor(top_left_world.x / static_cast<float>(grid_w))) - 1;
        const int end_x = static_cast<int>(std::ceil(bottom_right_world.x / static_cast<float>(grid_w))) + 1;
        const int start_y = static_cast<int>(std::floor(top_left_world.y / static_cast<float>(grid_h))) - 1;
        const int end_y = static_cast<int>(std::ceil(bottom_right_world.y / static_cast<float>(grid_h))) + 1;

        for (int x = start_x; x <= end_x; ++x) {
            const float world_x = x * static_cast<float>(grid_w);
            const glm::vec2 a = scene_.Camera().WorldToScreen({world_x, top_left_world.y});
            const glm::vec2 b = scene_.Camera().WorldToScreen({world_x, bottom_right_world.y});
            draw_list->AddLine(
                {image_min.x + (a.x / std::max(camera_viewport.x, 1.0f)) * image_size.x, image_min.y + (a.y / std::max(camera_viewport.y, 1.0f)) * image_size.y},
                {image_min.x + (b.x / std::max(camera_viewport.x, 1.0f)) * image_size.x, image_min.y + (b.y / std::max(camera_viewport.y, 1.0f)) * image_size.y},
                IM_COL32(255, 255, 255, 22)
            );
        }
        for (int y = start_y; y <= end_y; ++y) {
            const float world_y = y * static_cast<float>(grid_h);
            const glm::vec2 a = scene_.Camera().WorldToScreen({top_left_world.x, world_y});
            const glm::vec2 b = scene_.Camera().WorldToScreen({bottom_right_world.x, world_y});
            draw_list->AddLine(
                {image_min.x + (a.x / std::max(camera_viewport.x, 1.0f)) * image_size.x, image_min.y + (a.y / std::max(camera_viewport.y, 1.0f)) * image_size.y},
                {image_min.x + (b.x / std::max(camera_viewport.x, 1.0f)) * image_size.x, image_min.y + (b.y / std::max(camera_viewport.y, 1.0f)) * image_size.y},
                IM_COL32(255, 255, 255, 22)
            );
        }

        for (int i = 0; i < static_cast<int>(scene_.EditableLevel().parallax_layers.size()); ++i) {
            const auto& layer = scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(i)];
            const ImVec2 handle = world_to_viewport(layer.offset);
            if (handle.x < image_min.x - 24.0f || handle.x > image_max.x + 24.0f ||
                handle.y < image_min.y - 24.0f || handle.y > image_max.y + 24.0f) {
                continue;
            }

            const bool selected = selection_kind_ == SelectionKind::Parallax && selection_index_ == i;
            const ImU32 color = selected ? IM_COL32(255, 196, 84, 255) : IM_COL32(90, 212, 255, 235);
            draw_list->AddCircleFilled(handle, selected ? 6.5f : 5.0f, color);
            draw_list->AddCircle(handle, selected ? 9.0f : 7.5f, IM_COL32(16, 20, 28, 220), 0, 2.0f);
            draw_list->AddText({handle.x + 10.0f, handle.y - 8.0f}, color, layer.name.c_str());
        }

        for (int i = 0; i < static_cast<int>(scene_.EditableLevel().lights.size()); ++i) {
            const auto& light = scene_.EditableLevel().lights[static_cast<std::size_t>(i)];
            const ImVec2 handle = world_to_viewport(light.position);
            if (handle.x < image_min.x - 24.0f || handle.x > image_max.x + 24.0f ||
                handle.y < image_min.y - 24.0f || handle.y > image_max.y + 24.0f) {
                continue;
            }

            const bool selected = selection_kind_ == SelectionKind::Light && selection_index_ == i;
            const ImU32 color = selected ? IM_COL32(255, 221, 86, 255) : IM_COL32(255, 191, 76, 240);
            draw_list->AddCircleFilled(handle, selected ? 8.0f : 6.0f, color);
            draw_list->AddCircle(handle, selected ? 12.0f : 9.5f, IM_COL32(18, 18, 24, 230), 0, 2.0f);
            if (ToLowerCopy(light.type) == "flashlight" || ToLowerCopy(light.type) == "spot" ||
                ToLowerCopy(light.type) == "spotlight" || ToLowerCopy(light.type) == "directional") {
                const float radians = glm::radians(light.direction_degrees);
                const glm::vec2 direction{std::cos(radians), std::sin(radians)};
                const ImVec2 tip = world_to_viewport(light.position + direction * std::max(light.length, 96.0f) * 0.25f);
                draw_list->AddLine(handle, tip, color, selected ? 3.0f : 2.0f);
                draw_badge({handle.x - 14.0f, handle.y - 22.0f}, color, "FL");
            }
            draw_list->AddText({handle.x + 11.0f, handle.y - 7.0f}, color, light.name.c_str());
        }

        for (int i = 0; i < static_cast<int>(scene_.EditableLevel().triggers.size()); ++i) {
            const auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(i)];
            const bool selected = IsHierarchyItemSelected(SelectionKind::Trigger, i);
            const ImU32 color = selected ? IM_COL32(120, 255, 170, 255) : IM_COL32(94, 225, 138, 214);
            draw_world_rect(trigger.position, trigger.size, color, selected ? 2.0f : 1.2f);
            const ImVec2 badge = world_to_viewport(trigger.position + glm::vec2(18.0f, 14.0f));
            draw_badge(badge, color, "TRG");
            draw_list->AddText({badge.x + 18.0f, badge.y - 6.0f}, color, trigger.name.c_str());
        }

        for (int i = 0; i < static_cast<int>(scene_.EditableLevel().audio_sources.size()); ++i) {
            const auto& source = scene_.EditableLevel().audio_sources[static_cast<std::size_t>(i)];
            const ImVec2 handle = world_to_viewport(source.position);
            const bool selected = selection_kind_ == SelectionKind::AudioSource && selection_index_ == i;
            const ImU32 color = selected ? IM_COL32(120, 255, 168, 255) : IM_COL32(82, 216, 154, 238);
            draw_list->AddRectFilled({handle.x - 7.0f, handle.y - 7.0f}, {handle.x + 7.0f, handle.y + 7.0f}, color, 3.0f);
            draw_list->AddRect({handle.x - 9.0f, handle.y - 9.0f}, {handle.x + 9.0f, handle.y + 9.0f}, IM_COL32(18, 18, 24, 230), 3.0f, 0, 2.0f);
            draw_badge({handle.x, handle.y - 18.0f}, color, "SND");
            draw_list->AddText({handle.x + 12.0f, handle.y - 7.0f}, color, source.name.c_str());
        }

        for (int i = 0; i < static_cast<int>(scene_.EditableLevel().audio_paks.size()); ++i) {
            const auto& pak = scene_.EditableLevel().audio_paks[static_cast<std::size_t>(i)];
            const ImVec2 handle = world_to_viewport(pak.position);
            const bool selected = selection_kind_ == SelectionKind::AudioPak && selection_index_ == i;
            const ImU32 color = selected ? IM_COL32(188, 156, 255, 255) : IM_COL32(156, 132, 240, 238);
            draw_list->AddRectFilled({handle.x - 9.0f, handle.y - 9.0f}, {handle.x + 9.0f, handle.y + 9.0f}, color, 4.0f);
            draw_list->AddRect({handle.x - 12.0f, handle.y - 12.0f}, {handle.x + 12.0f, handle.y + 12.0f}, IM_COL32(18, 18, 24, 230), 4.0f, 0, 2.0f);
            draw_badge({handle.x, handle.y - 20.0f}, color, "PAK");
            draw_list->AddText({handle.x + 14.0f, handle.y - 7.0f}, color, pak.name.c_str());
        }

        for (int i = 0; i < static_cast<int>(scene_.EditableLevel().entities.size()); ++i) {
            const auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(i)];
            if (!IsParticleEmitterEntity(entity)) {
                continue;
            }
            const bool selected = IsHierarchyItemSelected(SelectionKind::Entity, i);
            const ImU32 color = selected ? IM_COL32(255, 162, 86, 255) : IM_COL32(255, 136, 64, 230);
            draw_world_rect(entity.position, entity.size, color, selected ? 2.0f : 1.2f);
            const ImVec2 center = world_to_viewport(entity.position + entity.size * 0.5f);
            draw_list->AddCircleFilled(center, selected ? 9.0f : 7.0f, color);
            draw_list->AddCircle(center, selected ? 13.0f : 10.0f, IM_COL32(18, 18, 24, 230), 0, 2.0f);
            draw_badge({center.x, center.y - 18.0f}, color, "FX");
            const std::string asset_name = std::filesystem::path(ParticleAssetPath(entity)).stem().string();
            draw_list->AddText({center.x + 12.0f, center.y - 7.0f}, color, asset_name.empty() ? entity.id.c_str() : asset_name.c_str());
        }

        for (int i = 0; i < static_cast<int>(scene_.EditableLevel().virtual_cameras.size()); ++i) {
            const auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(i)];
            const ImVec2 min = world_to_viewport(camera.position);
            const ImVec2 max = world_to_viewport(camera.position + camera.size);
            const ImVec2 center = world_to_viewport(camera.position + camera.size * 0.5f);
            const bool selected = IsHierarchyItemSelected(SelectionKind::VirtualCamera, i);
            const ImU32 color = selected ? IM_COL32(94, 224, 255, 255) : IM_COL32(74, 152, 244, 210);
            draw_list->AddRect(min, max, color, 0.0f, 0, selected ? 2.5f : 1.2f);
            draw_list->AddCircleFilled(center, selected ? 8.0f : 6.0f, color);
            draw_list->AddCircle(center, selected ? 12.0f : 9.0f, IM_COL32(18, 18, 24, 230), 0, 2.0f);
            draw_list->AddText({center.x + 12.0f, center.y - 10.0f}, color, camera.name.c_str());
            const ImVec2 dead_zone_min = world_to_viewport(camera.position + camera.size * 0.5f - camera.dead_zone * 0.5f);
            const ImVec2 dead_zone_max = world_to_viewport(camera.position + camera.size * 0.5f + camera.dead_zone * 0.5f);
            draw_list->AddRect(dead_zone_min, dead_zone_max, IM_COL32(96, 248, 224, selected ? 220 : 140), 0.0f, 0, 1.0f);
        }

        const auto& player_camera = scene_.EditableLevel().player_camera;
        if (player_camera.enabled && !scene_.HasActiveVirtualCamera()) {
            auto target_it = std::find_if(scene_.EditableLevel().entities.begin(), scene_.EditableLevel().entities.end(),
                [&](const assets::EntityDefinition& entity) {
                    if (!player_camera.follow_target.empty()) {
                        return entity.id == player_camera.follow_target;
                    }
                    return entity.id == "player" || entity.archetype == "player";
                });
            if (target_it != scene_.EditableLevel().entities.end()) {
                const glm::vec2 center_world = target_it->position + target_it->size * 0.5f + player_camera.follow_offset;
                const ImVec2 center = world_to_viewport(center_world);
                const ImVec2 dead_zone_min = world_to_viewport(center_world - player_camera.dead_zone * 0.5f);
                const ImVec2 dead_zone_max = world_to_viewport(center_world + player_camera.dead_zone * 0.5f);
                const ImU32 color = IM_COL32(255, 162, 92, 220);
                draw_list->AddCircle(center, 10.0f, color, 0, 2.0f);
                draw_badge({center.x, center.y - 18.0f}, color, "PCAM");
                draw_list->AddRect(dead_zone_min, dead_zone_max, color, 0.0f, 0, 1.0f);
            }
        }
    }

    std::optional<std::pair<glm::vec2, glm::vec2>> selected_entity_bounds;
    bool selection_contains_non_entities = false;
    bool selection_contains_trigger = false;
    bool selection_contains_camera = false;
    for (const auto& item : hierarchy_selection_) {
        if (item.kind == SelectionKind::Entity &&
            item.index >= 0 &&
            item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            const auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)];
            draw_world_rect(entity.position, entity.size, IM_COL32(255, 214, 92, 255), 2.0f);
            if (!selected_entity_bounds.has_value()) {
                selected_entity_bounds = std::make_pair(entity.position, entity.size);
            } else {
                auto& [bounds_position, bounds_size] = *selected_entity_bounds;
                const glm::vec2 min_corner{
                    std::min(bounds_position.x, entity.position.x),
                    std::min(bounds_position.y, entity.position.y)
                };
                const glm::vec2 max_corner{
                    std::max(bounds_position.x + bounds_size.x, entity.position.x + entity.size.x),
                    std::max(bounds_position.y + bounds_size.y, entity.position.y + entity.size.y)
                };
                bounds_position = min_corner;
                bounds_size = max_corner - min_corner;
            }
        } else if (item.kind == SelectionKind::Trigger &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().triggers.size())) {
            const auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(item.index)];
            draw_world_rect(trigger.position, trigger.size, IM_COL32(82, 255, 134, 255), 2.0f);
            selection_contains_trigger = true;
        } else if (item.kind == SelectionKind::Light &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().lights.size())) {
            const auto& light = scene_.EditableLevel().lights[static_cast<std::size_t>(item.index)];
            const bool flashlight = ToLowerCopy(light.type) == "flashlight" || ToLowerCopy(light.type) == "spot" ||
                                    ToLowerCopy(light.type) == "spotlight" || ToLowerCopy(light.type) == "directional";
            const float range = flashlight ? std::max(light.length, light.radius) : light.radius;
            draw_world_rect(
                light.position - glm::vec2(range, range),
                glm::vec2(range * 2.0f, range * 2.0f),
                IM_COL32(255, 214, 92, 255),
                2.0f
            );
            selection_contains_non_entities = true;
        } else if (item.kind == SelectionKind::AudioSource &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
            const auto& source = scene_.EditableLevel().audio_sources[static_cast<std::size_t>(item.index)];
            draw_world_rect(
                source.position - glm::vec2(source.radius, source.radius),
                glm::vec2(source.radius * 2.0f, source.radius * 2.0f),
                IM_COL32(92, 255, 180, 255),
                2.0f
            );
            selection_contains_non_entities = true;
        } else if (item.kind == SelectionKind::AudioPak &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
            const auto& pak = scene_.EditableLevel().audio_paks[static_cast<std::size_t>(item.index)];
            draw_world_rect(
                pak.position - glm::vec2(pak.radius, pak.radius),
                glm::vec2(pak.radius * 2.0f, pak.radius * 2.0f),
                IM_COL32(196, 164, 255, 255),
                2.0f
            );
            selection_contains_non_entities = true;
        } else if (item.kind == SelectionKind::VirtualCamera &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
            const auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(item.index)];
            draw_world_rect(camera.position, camera.size, IM_COL32(94, 224, 255, 255), 2.0f);
            selection_contains_camera = true;
        } else if (item.kind == SelectionKind::Parallax &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
            const auto& layer = scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(item.index)];
            draw_world_rect(layer.offset - glm::vec2(10.0f, 10.0f), {20.0f, 20.0f}, IM_COL32(90, 212, 255, 255), 2.0f);
            selection_contains_non_entities = true;
        }
    }

    const bool ctrl_down = GetInput().IsKeyDown(SDL_SCANCODE_LCTRL) || GetInput().IsKeyDown(SDL_SCANCODE_RCTRL);
    const bool alt_resize = GetInput().IsKeyDown(SDL_SCANCODE_LALT) || GetInput().IsKeyDown(SDL_SCANCODE_RALT);
    const bool constructor_mode = ctrl_down && alt_resize;
    const bool can_draw_entity_handles = alt_resize &&
        selected_entity_bounds.has_value() &&
        !selection_contains_trigger &&
        !selection_contains_camera &&
        !selection_contains_non_entities &&
        (constructor_mode || hierarchy_selection_.size() == 1);
    const bool can_draw_single_rect_handles = alt_resize && !can_draw_entity_handles &&
        ((selection_kind_ == SelectionKind::Trigger &&
          selection_index_ >= 0 &&
          selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) ||
         (selection_kind_ == SelectionKind::VirtualCamera &&
          selection_index_ >= 0 &&
          selection_index_ < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())));
    if (can_draw_entity_handles || can_draw_single_rect_handles) {
        glm::vec2 position{};
        glm::vec2 size{};
        if (can_draw_entity_handles) {
            position = selected_entity_bounds->first;
            size = selected_entity_bounds->second;
        } else if (selection_kind_ == SelectionKind::Trigger) {
            const auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(selection_index_)];
            position = trigger.position;
            size = trigger.size;
        } else {
            const auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(selection_index_)];
            position = camera.position;
            size = camera.size;
        }

        const std::array<glm::vec2, 4> handle_points{
            glm::vec2(position.x, position.y + size.y * 0.5f),
            glm::vec2(position.x + size.x, position.y + size.y * 0.5f),
            glm::vec2(position.x + size.x * 0.5f, position.y),
            glm::vec2(position.x + size.x * 0.5f, position.y + size.y)
        };
        for (int handle_index = 0; handle_index < static_cast<int>(handle_points.size()); ++handle_index) {
            const ImVec2 screen = world_to_viewport(handle_points[static_cast<std::size_t>(handle_index)]);
            if (constructor_mode && can_draw_entity_handles) {
                const ImU32 color = IM_COL32(255, 196, 84, 255);
                if (handle_index < 2) {
                    const float direction = handle_index == 0 ? -1.0f : 1.0f;
                    draw_list->AddTriangleFilled(
                        {screen.x + direction * 8.0f, screen.y},
                        {screen.x - direction * 4.0f, screen.y - 6.0f},
                        {screen.x - direction * 4.0f, screen.y + 6.0f},
                        color);
                } else {
                    const float direction = handle_index == 2 ? -1.0f : 1.0f;
                    draw_list->AddTriangleFilled(
                        {screen.x, screen.y + direction * 8.0f},
                        {screen.x - 6.0f, screen.y - direction * 4.0f},
                        {screen.x + 6.0f, screen.y - direction * 4.0f},
                        color);
                }
            } else {
                draw_list->AddRectFilled(
                    {screen.x - 5.0f, screen.y - 5.0f},
                    {screen.x + 5.0f, screen.y + 5.0f},
                    IM_COL32(114, 213, 255, 255),
                    2.0f);
                draw_list->AddRect(
                    {screen.x - 5.0f, screen.y - 5.0f},
                    {screen.x + 5.0f, screen.y + 5.0f},
                    IM_COL32(20, 26, 34, 255),
                    2.0f,
                    0,
                    1.0f);
            }
        }
    }

    if (marquee_selecting_) {
        const glm::vec2 min_world{
            std::min(marquee_start_world_.x, marquee_current_world_.x),
            std::min(marquee_start_world_.y, marquee_current_world_.y)
        };
        const glm::vec2 max_world{
            std::max(marquee_start_world_.x, marquee_current_world_.x),
            std::max(marquee_start_world_.y, marquee_current_world_.y)
        };
        const ImVec2 min_screen = world_to_viewport(min_world);
        const ImVec2 max_screen = world_to_viewport(max_world);
        draw_list->AddRectFilled(min_screen, max_screen, IM_COL32(90, 156, 255, 28));
        draw_list->AddRect(min_screen, max_screen, IM_COL32(90, 156, 255, 220), 0.0f, 0, 1.6f);
    }

    glm::vec2 mouse_world{};
    viewport_hovered_ = ViewportMouseToWorld(mouse_world);
    if (viewport_hovered_ && GetInput().WheelDelta() != 0.0f) {
        scene_.Camera().SetZoom(scene_.Camera().Zoom() + GetInput().WheelDelta() * 0.1f);
    }

    if (viewport_mode_ == ViewportMode::Edit && viewport_hovered_ && tile_paint_mode_ &&
        active_tile_layer_index_ >= 0 &&
        active_tile_layer_index_ < static_cast<int>(scene_.EditableLevel().tile_layers.size())) {
        const auto& level = scene_.EditableLevel();
        const float grid_w = static_cast<float>(std::max(level.tile_width, 1));
        const float grid_h = static_cast<float>(std::max(level.tile_height, 1));
        const int tile_x = static_cast<int>(std::floor(mouse_world.x / grid_w));
        const int tile_y = static_cast<int>(std::floor(mouse_world.y / grid_h));
        const glm::vec2 tile_position{tile_x * grid_w, tile_y * grid_h};
        const ImVec2 tile_min = world_to_viewport(tile_position);
        const ImVec2 tile_max = world_to_viewport(tile_position + glm::vec2(grid_w, grid_h));
        draw_list->AddRectFilled(tile_min, tile_max, IM_COL32(255, 214, 92, 24));
        draw_list->AddRect(tile_min, tile_max, IM_COL32(255, 214, 92, 255), 0.0f, 0, 2.0f);
        if (!level.tileset.texture.empty() && selected_tileset_index_ >= 0) {
            renderer::Texture2D& tileset_texture = asset_manager_.LoadTexture(level.tileset.texture);
            const glm::vec4 uv = TilesetTileUv(level, selected_tileset_index_);
            draw_list->AddImage(reinterpret_cast<void*>(static_cast<intptr_t>(tileset_texture.Id())), tile_min, tile_max, ImVec2(uv.x, uv.w), ImVec2(uv.z, uv.y), IM_COL32(255, 255, 255, 210));
        } else if (selected_tileset_index_ < 0) {
            draw_list->AddLine(tile_min, tile_max, IM_COL32(255, 96, 96, 220), 2.0f);
            draw_list->AddLine({tile_min.x, tile_max.y}, {tile_max.x, tile_min.y}, IM_COL32(255, 96, 96, 220), 2.0f);
        }
    }

    if (viewport_mode_ == ViewportMode::Edit && viewport_hovered_ && !placement_relative_.empty()) {
        glm::vec2 preview_size{64.0f, 64.0f};
        if (placement_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(placement_relative_))) {
            renderer::Texture2D& texture = asset_manager_.LoadTexture(placement_relative_);
            preview_size = {
                static_cast<float>(std::max(texture.Size().x, 16)),
                static_cast<float>(std::max(texture.Size().y, 16))
            };
        } else if (placement_kind_ == BrowserSelectionKind::Object && std::filesystem::exists(project_root_ / placement_relative_)) {
            preview_size = assets::LoadObjectAsset(project_root_ / placement_relative_).size;
        } else if (placement_kind_ == BrowserSelectionKind::Trigger && std::filesystem::exists(project_root_ / placement_relative_)) {
            preview_size = assets::LoadTriggerAsset(project_root_ / placement_relative_).default_size;
        } else if ((placement_kind_ == BrowserSelectionKind::File && IsAudioExtension(std::filesystem::path(placement_relative_))) ||
                   placement_kind_ == BrowserSelectionKind::AudioSource) {
            preview_size = {24.0f, 24.0f};
        } else if (placement_kind_ == BrowserSelectionKind::Animation) {
            preview_size = {48.0f, 48.0f};
        } else if (placement_kind_ == BrowserSelectionKind::Particle) {
            preview_size = {48.0f, 48.0f};
        } else if (placement_kind_ == BrowserSelectionKind::Light) {
            preview_size = {280.0f, 280.0f};
        } else if (placement_kind_ == BrowserSelectionKind::AudioPak) {
            preview_size = {48.0f, 48.0f};
        }

        glm::vec2 preview_center = mouse_world;
        if (snap_to_grid_) {
            const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
            const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
            preview_center.x = std::round(preview_center.x / grid_w) * grid_w;
            preview_center.y = std::round(preview_center.y / grid_h) * grid_h;
        }

        const glm::vec2 preview_position = preview_center - preview_size * 0.5f;
        draw_world_rect(preview_position, preview_size, IM_COL32(255, 220, 120, 255), 2.0f);
    }

    const bool viewport_context_menu_open = ImGui::IsPopupOpen("ViewportContextMenu");
    if (viewport_mode_ == ViewportMode::Edit && !viewport_context_menu_open) {
        HandleViewportSelectionAndPlacement(mouse_world);
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_TEXTURE_PATH")) {
            if (viewport_hovered_ && viewport_mode_ == ViewportMode::Edit) {
                PlaceTextureEntity(static_cast<const char*>(payload->Data), mouse_world);
                placement_kind_ = BrowserSelectionKind::None;
                placement_relative_.clear();
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_OBJECT_PATH")) {
            if (viewport_hovered_ && viewport_mode_ == ViewportMode::Edit) {
                PlaceObjectEntity(static_cast<const char*>(payload->Data), mouse_world);
                placement_kind_ = BrowserSelectionKind::None;
                placement_relative_.clear();
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_TRIGGER_PATH")) {
            if (viewport_hovered_ && viewport_mode_ == ViewportMode::Edit) {
                PlaceTriggerZone(static_cast<const char*>(payload->Data), mouse_world);
                placement_kind_ = BrowserSelectionKind::None;
                placement_relative_.clear();
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_AUDIO_PATH")) {
            if (viewport_hovered_ && viewport_mode_ == ViewportMode::Edit) {
                PlaceAudioSource(static_cast<const char*>(payload->Data), mouse_world);
                placement_kind_ = BrowserSelectionKind::None;
                placement_relative_.clear();
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_PARTICLE_PATH")) {
            if (viewport_hovered_ && viewport_mode_ == ViewportMode::Edit) {
                PlaceParticleEmitter(static_cast<const char*>(payload->Data), mouse_world);
                placement_kind_ = BrowserSelectionKind::None;
                placement_relative_.clear();
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_LIGHT_BUILTIN")) {
            if (viewport_hovered_ && viewport_mode_ == ViewportMode::Edit) {
                const std::string payload_value = static_cast<const char*>(payload->Data);
                PlaceLight(mouse_world, payload_value == "__builtin_flashlight__" ? "flashlight" : "point");
                placement_kind_ = BrowserSelectionKind::None;
                placement_relative_.clear();
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_AUDIO_PAK_BUILTIN")) {
            if (viewport_hovered_ && viewport_mode_ == ViewportMode::Edit) {
                (void)payload;
                PlaceAudioPak(mouse_world);
                placement_kind_ = BrowserSelectionKind::None;
                placement_relative_.clear();
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (viewport_hovered_ &&
        viewport_mode_ == ViewportMode::Edit &&
        !viewport_context_menu_open &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        auto set_primary_without_clearing = [&](SelectionKind kind, int index) {
            selection_kind_ = kind;
            selection_index_ = index;
            if (kind == SelectionKind::Entity &&
                index >= 0 &&
                index < static_cast<int>(scene_.EditableLevel().entities.size())) {
                current_script_relative_ = scene_.EditableLevel().entities[static_cast<std::size_t>(index)].script;
                current_code_relative_ = current_script_relative_;
                LoadCurrentScript();
            }
        };
        auto select_or_preserve = [&](SelectionKind kind, int index) {
            if (IsHierarchyItemSelected(kind, index)) {
                set_primary_without_clearing(kind, index);
            } else {
                SetHierarchySelectionSingle(kind, index);
            }
        };
        const auto point_in_rect = [&](glm::vec2 point, glm::vec2 position, glm::vec2 size) {
            return point.x >= position.x && point.y >= position.y &&
                   point.x <= position.x + size.x && point.y <= position.y + size.y;
        };
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const auto point_near_world = [&](glm::vec2 world, float radius) {
            const glm::vec2 screen = scene_.Camera().WorldToScreen(world);
            const ImVec2 handle{
                image_min.x + (screen.x / std::max(camera_viewport.x, 1.0f)) * image_size.x,
                image_min.y + (screen.y / std::max(camera_viewport.y, 1.0f)) * image_size.y
            };
            const float dx = mouse.x - handle.x;
            const float dy = mouse.y - handle.y;
            return (dx * dx + dy * dy) <= radius * radius;
        };

        bool picked = false;
        for (int i = static_cast<int>(scene_.EditableLevel().triggers.size()) - 1; i >= 0; --i) {
            const auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(i)];
            if (point_in_rect(mouse_world, trigger.position, trigger.size)) {
                select_or_preserve(SelectionKind::Trigger, i);
                picked = true;
                break;
            }
        }
        if (!picked) {
            for (int i = static_cast<int>(scene_.EditableLevel().lights.size()) - 1; i >= 0; --i) {
                if (point_near_world(scene_.EditableLevel().lights[static_cast<std::size_t>(i)].position, 12.0f)) {
                    select_or_preserve(SelectionKind::Light, i);
                    picked = true;
                    break;
                }
            }
        }
        if (!picked) {
            for (int i = static_cast<int>(scene_.EditableLevel().audio_sources.size()) - 1; i >= 0; --i) {
                if (point_near_world(scene_.EditableLevel().audio_sources[static_cast<std::size_t>(i)].position, 12.0f)) {
                    select_or_preserve(SelectionKind::AudioSource, i);
                    picked = true;
                    break;
                }
            }
        }
        if (!picked) {
            for (int i = static_cast<int>(scene_.EditableLevel().audio_paks.size()) - 1; i >= 0; --i) {
                if (point_near_world(scene_.EditableLevel().audio_paks[static_cast<std::size_t>(i)].position, 14.0f)) {
                    select_or_preserve(SelectionKind::AudioPak, i);
                    picked = true;
                    break;
                }
            }
        }
        if (!picked) {
            for (int i = static_cast<int>(scene_.EditableLevel().virtual_cameras.size()) - 1; i >= 0; --i) {
                const auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(i)];
                if (point_near_world(camera.position + camera.size * 0.5f, 14.0f)) {
                    select_or_preserve(SelectionKind::VirtualCamera, i);
                    picked = true;
                    break;
                }
            }
        }
        if (!picked) {
            for (int i = static_cast<int>(scene_.EditableLevel().parallax_layers.size()) - 1; i >= 0; --i) {
                if (point_near_world(scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(i)].offset, 10.0f)) {
                    select_or_preserve(SelectionKind::Parallax, i);
                    picked = true;
                    break;
                }
            }
        }
        if (!picked) {
            std::vector<int> entity_pick_order(scene_.EditableLevel().entities.size());
            std::iota(entity_pick_order.begin(), entity_pick_order.end(), 0);
            std::sort(entity_pick_order.begin(), entity_pick_order.end(), [&](int lhs, int rhs) {
                const auto& a = scene_.EditableLevel().entities[static_cast<std::size_t>(lhs)];
                const auto& b = scene_.EditableLevel().entities[static_cast<std::size_t>(rhs)];
                const bool a_particle = IsParticleEmitterEntity(a);
                const bool b_particle = IsParticleEmitterEntity(b);
                if (a_particle != b_particle) {
                    return a_particle;
                }
                if (a.layer != b.layer) {
                    return a.layer > b.layer;
                }
                return lhs > rhs;
            });
            for (int i : entity_pick_order) {
                const auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(i)];
                if (point_in_rect(mouse_world, entity.position, entity.size)) {
                    select_or_preserve(SelectionKind::Entity, i);
                    picked = true;
                    break;
                }
            }
        }
        if (!picked) {
            ClearHierarchySelection();
            selection_kind_ = SelectionKind::None;
            selection_index_ = -1;
        }
    }

    if (ImGui::BeginPopupContextWindow("ViewportContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        if (selection_kind_ != SelectionKind::None) {
            const bool all_entities = !hierarchy_selection_.empty() &&
                std::all_of(hierarchy_selection_.begin(), hierarchy_selection_.end(), [](const HierarchySelectionItem& item) {
                    return item.kind == SelectionKind::Entity;
                });
            if (ImGui::MenuItem("Delete")) {
                pending_history_label_ = "Delete selection";
                DeleteSelection();
            }
            if (ImGui::MenuItem("Duplicate")) {
                pending_history_label_ = "Duplicate selection";
                DuplicateHierarchySelection();
            }
            if (ImGui::MenuItem("Group", nullptr, false, hierarchy_selection_.size() >= 2)) {
                hierarchy_name_buffer_ = "group_" + std::to_string(scene_.EditableLevel().entities.size());
                open_hierarchy_combine_popup_ = true;
            }
            if (ImGui::MenuItem("Merge To Image", nullptr, false, all_entities && hierarchy_selection_.size() >= 2)) {
                MergeHierarchySelectionToTexture();
            }
            if (ImGui::MenuItem("Remove Gaps", nullptr, false, all_entities && hierarchy_selection_.size() >= 2)) {
                RemoveHierarchySelectionGaps();
            }
            if (ImGui::MenuItem("Properties")) {
                show_properties_panel_ = true;
            }
            if (selection_kind_ == SelectionKind::Trigger && ImGui::MenuItem("Open Graph Editor")) {
                show_trigger_panel_ = true;
                if (selection_index_ >= 0 &&
                    selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) {
                    SetHierarchySelectionSingle(SelectionKind::Trigger, selection_index_);
                }
                focus_trigger_panel_ = true;
                status_message_ = "Opened trigger graph editor.";
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (selection_kind_ == SelectionKind::Entity &&
                selection_index_ >= 0 &&
                selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())) {
                auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(selection_index_)];
                if (ImGui::MenuItem("Move Layer Up")) {
                    ++entity.layer;
                    pending_history_label_ = "Entity layer up";
                    scene_.ResetSimulation(false);
                }
                if (ImGui::MenuItem("Move Layer Down")) {
                    --entity.layer;
                    pending_history_label_ = "Entity layer down";
                    scene_.ResetSimulation(false);
                }
                if (ImGui::InputFloat2("Size", &entity.size.x)) {
                    pending_history_label_ = "Resize entity";
                    entity.size.x = std::max(entity.size.x, 1.0f);
                    entity.size.y = std::max(entity.size.y, 1.0f);
                    scene_.ResetSimulation(false);
                }
            } else if (selection_kind_ == SelectionKind::Trigger &&
                       selection_index_ >= 0 &&
                       selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) {
                auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(selection_index_)];
                if (ImGui::InputFloat2("Size", &trigger.size.x)) {
                    pending_history_label_ = "Resize trigger";
                    trigger.size.x = std::max(trigger.size.x, 1.0f);
                    trigger.size.y = std::max(trigger.size.y, 1.0f);
                    scene_.ResetSimulation(false);
                }
            } else if (selection_kind_ == SelectionKind::VirtualCamera &&
                       selection_index_ >= 0 &&
                       selection_index_ < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
                auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(selection_index_)];
                if (ImGui::InputFloat2("Size", &camera.size.x)) {
                    pending_history_label_ = "Resize virtual camera";
                    camera.size.x = std::max(camera.size.x, 64.0f);
                    camera.size.y = std::max(camera.size.y, 64.0f);
                    scene_.ResetSimulation(false);
                }
            } else if (selection_kind_ == SelectionKind::Parallax &&
                       selection_index_ >= 0 &&
                       selection_index_ < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
                auto& layer = scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(selection_index_)];
                const bool can_move_up = selection_index_ + 1 < static_cast<int>(scene_.EditableLevel().parallax_layers.size());
                const bool can_move_down = selection_index_ > 0;
                if (ImGui::MenuItem("Move Layer Up", nullptr, false, can_move_up)) {
                    std::swap(scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(selection_index_)],
                              scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(selection_index_ + 1)]);
                    ++selection_index_;
                    pending_history_label_ = "Parallax layer up";
                    scene_.ResetSimulation(false);
                }
                if (ImGui::MenuItem("Move Layer Down", nullptr, false, can_move_down)) {
                    std::swap(scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(selection_index_)],
                              scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(selection_index_ - 1)]);
                    --selection_index_;
                    pending_history_label_ = "Parallax layer down";
                    scene_.ResetSimulation(false);
                }
                float parallax_speed = layer.speed.x;
                if (ImGui::SliderFloat("Parallax Speed", &parallax_speed, -2.0f, 2.0f, "%.2f")) {
                    layer.speed = {parallax_speed, layer.speed.y};
                    pending_history_label_ = "Parallax speed";
                    scene_.ResetSimulation(false);
                }
                if (ImGui::InputFloat2("Scale", &layer.scale.x)) {
                    pending_history_label_ = "Resize parallax";
                    layer.scale.x = std::max(layer.scale.x, 0.05f);
                    layer.scale.y = std::max(layer.scale.y, 0.05f);
                    scene_.ResetSimulation(false);
                }
            } else if (selection_kind_ == SelectionKind::Light &&
                       selection_index_ >= 0 &&
                       selection_index_ < static_cast<int>(scene_.EditableLevel().lights.size())) {
                auto& light = scene_.EditableLevel().lights[static_cast<std::size_t>(selection_index_)];
                const bool flashlight = ToLowerCopy(light.type) == "flashlight" || ToLowerCopy(light.type) == "spot" ||
                                        ToLowerCopy(light.type) == "spotlight" || ToLowerCopy(light.type) == "directional";
                if (flashlight ? ImGui::InputFloat("Length", &light.length) : ImGui::InputFloat("Radius", &light.radius)) {
                    pending_history_label_ = flashlight ? "Resize flashlight" : "Resize light";
                    light.radius = std::max(light.radius, 8.0f);
                    light.length = std::max(light.length, light.radius);
                    scene_.ResetSimulation(false);
                }
            } else if (selection_kind_ == SelectionKind::AudioSource &&
                       selection_index_ >= 0 &&
                       selection_index_ < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
                auto& source = scene_.EditableLevel().audio_sources[static_cast<std::size_t>(selection_index_)];
                if (ImGui::InputFloat("Radius", &source.radius)) {
                    pending_history_label_ = "Resize audio source radius";
                    source.radius = std::max(source.radius, 1.0f);
                    scene_.ResetSimulation(false);
                }
            } else if (selection_kind_ == SelectionKind::AudioPak &&
                       selection_index_ >= 0 &&
                       selection_index_ < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
                auto& pak = scene_.EditableLevel().audio_paks[static_cast<std::size_t>(selection_index_)];
                if (ImGui::InputFloat("Radius", &pak.radius)) {
                    pending_history_label_ = "Resize audio pak radius";
                    pak.radius = std::max(pak.radius, 1.0f);
                    scene_.ResetSimulation(false);
                }
            }
        } else {
            if (ImGui::MenuItem("Scene Properties")) {
                show_properties_panel_ = true;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void EditorApp::DrawToolbar() {
    if (!show_toolbar_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("BAR", Tr("editor.window.toolbar", "Toolbar"), "ToolbarWindow").c_str(), &show_toolbar_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.26f, 0.66f, 0.96f, 1.0f});
    const std::string switch_to_side = Tr("editor.toolbar.switch_side", "Switch To Side");
    const std::string switch_to_iso = Tr("editor.toolbar.switch_iso", "Switch To Isometric");
    const std::string live_preview_text = Tr("editor.toggle.live_preview", "Live Preview");
    const std::string debug_draw_text = Tr("editor.toggle.debug_draw", "Debug Draw");
    const std::string audio_text = Tr("editor.toggle.editor_audio", "Editor Audio");
    const std::string rt_lights_text = Tr("editor.toggle.rt_lights", "RT Lights");
    const std::string follow_camera_text = Tr("editor.toolbar.follow_camera", "Follow Player");
    const std::string edit_mode_text = Tr("editor.toolbar.mode_edit", "Edit");
    const std::string preview_mode_text = Tr("editor.toolbar.mode_preview", "Preview");
    const std::string snap_grid_text = Tr("editor.toolbar.snap_grid", "Snap To Grid");
    const std::string save_all_text = Tr("editor.toolbar.save_all", "Save All");
    const std::string build_text = Tr("editor.action.build", "Build Package");
    const std::string build_run_text = Tr("editor.action.build_run", "Build And Run");
    const auto quick_button = [&](const char* label, const char* tooltip, const ImVec4& color, const auto& on_click) {
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x + 0.08f, color.y + 0.08f, color.z + 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x + 0.14f, color.y + 0.14f, color.z + 0.14f, 1.0f));
        if (ImGui::Button(label, {68.0f, 30.0f})) {
            on_click();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::PopStyleColor(3);
    };

    quick_button("PLAY", "Switch viewport to Preview mode", {0.20f, 0.72f, 0.48f, 1.0f}, [&] {
        viewport_mode_ = ViewportMode::Preview;
        preview_paused_ = false;
    });
    ImGui::SameLine();
    quick_button("PAUSE", "Pause preview simulation", {0.70f, 0.48f, 0.20f, 1.0f}, [&] {
        preview_paused_ = !preview_paused_;
    });
    ImGui::SameLine();
    quick_button("SAVE", save_all_text.c_str(), {0.26f, 0.60f, 0.94f, 1.0f}, [&] {
        SaveAll();
        status_message_ = "Project saved.";
    });
    ImGui::SameLine();
    quick_button("PKG", build_text.c_str(), {0.78f, 0.48f, 0.24f, 1.0f}, [&] {
        ExportGamePackage();
    });
    ImGui::SameLine();
    quick_button("RUN", build_run_text.c_str(), {0.88f, 0.34f, 0.34f, 1.0f}, [&] {
        BuildAndRunGamePackage();
    });
    ImGui::SameLine();
    quick_button("COPY", "Copy selected scene items", {0.46f, 0.42f, 0.92f, 1.0f}, [&] {
        CopyHierarchySelection();
    });
    ImGui::SameLine();
    quick_button("PASTE", "Paste copied scene items", {0.58f, 0.42f, 0.94f, 1.0f}, [&] {
        PasteHierarchySelection();
    });
    ImGui::SameLine();
    if (ImGui::Button(scene_.CameraModeName() == "isometric" ? switch_to_side.c_str() : switch_to_iso.c_str())) {
        scene_.ToggleCameraMode();
    }
    ImGui::SameLine();
    ImGui::Checkbox(live_preview_text.c_str(), &live_preview_);
    ImGui::SameLine();
    ImGui::Checkbox(debug_draw_text.c_str(), &draw_debug_);
    ImGui::SameLine();
    if (ImGui::Checkbox(audio_text.c_str(), &editor_audio_enabled_)) {
        asset_manager_.SetAudioEnabled(editor_audio_enabled_);
    }
    ImGui::SameLine();
    bool rt_lights = scene_.EditableLevel().lighting.rt_enabled;
    if (ImGui::Checkbox(rt_lights_text.c_str(), &rt_lights)) {
        scene_.EditableLevel().lighting.rt_enabled = rt_lights;
        if (rt_lights) {
            scene_.EditableLevel().lighting.enabled = true;
        }
        scene_.ResetSimulation(false);
    }
    ImGui::SameLine();
    bool follow_camera = scene_.CameraFollowEnabled();
    if (ImGui::Checkbox(follow_camera_text.c_str(), &follow_camera)) {
        scene_.SetCameraFollowEnabled(follow_camera);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Pause", &preview_paused_);
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        preview_paused_ = true;
        preview_step_requested_ = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Trace", &preview_trace_enabled_);
    ImGui::SameLine();
    int viewport_mode = viewport_mode_ == ViewportMode::Edit ? 0 : 1;
    if (ImGui::BeginCombo("Mode", viewport_mode == 0 ? edit_mode_text.c_str() : preview_mode_text.c_str())) {
        if (ImGui::Selectable(edit_mode_text.c_str(), viewport_mode == 0)) {
            viewport_mode_ = ViewportMode::Edit;
            viewport_mode = 0;
        }
        if (ImGui::Selectable(preview_mode_text.c_str(), viewport_mode == 1)) {
            viewport_mode_ = ViewportMode::Preview;
            viewport_mode = 1;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox(snap_grid_text.c_str(), &snap_to_grid_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::InputInt2("Render Size", &scene_render_size_.x)) {
        scene_render_size_.x = std::max(scene_render_size_.x, 320);
        scene_render_size_.y = std::max(scene_render_size_.y, 180);
        const float aspect = static_cast<float>(std::max(project_.game_viewport_size.x, 1)) /
                             static_cast<float>(std::max(project_.game_viewport_size.y, 1));
        scene_render_size_.y = std::max(static_cast<int>(std::round(static_cast<float>(scene_render_size_.x) / aspect)), 180);
    }
    ImGui::SameLine();
    if (ImGui::Button(save_all_text.c_str())) {
        SaveAll();
        status_message_ = "Project saved.";
    }
    ImGui::SameLine();
    if (ImGui::Button(build_text.c_str())) {
        ExportGamePackage();
    }
    ImGui::SameLine();
    if (ImGui::Button(build_run_text.c_str())) {
        BuildAndRunGamePackage();
    }
    ImGui::SameLine();
    if (ImGui::Button("AI Builder")) {
        show_ai_builder_ = true;
    }
    if (!ai_build_queue_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("AI %d/%d%s",
            static_cast<int>(std::min(ai_build_cursor_, ai_build_queue_.size())),
            static_cast<int>(ai_build_queue_.size()),
            ai_build_running_ ? " building" : "");
    }
    ImGui::End();
}

void EditorApp::DrawAiBuilderPanel() {
    if (!show_ai_builder_) {
        return;
    }
    if (!ImGui::Begin("[AI] AI Level Builder###AiLevelBuilderWindow", &show_ai_builder_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.20f, 0.74f, 0.48f, 1.0f});

    const int object_count = static_cast<int>(EnumerateRelativeFiles("resources/objects", ".nobject").size());
    const int image_count = static_cast<int>(EnumerateRelativeFiles("assets/images").size());
    const int audio_count = static_cast<int>(EnumerateRelativeFiles("assets/audio").size());

    ImGui::TextWrapped("Builds a draft level from the assets already inside the project. The builder analyzes file names, object resources and scene roles, then assembles the map step by step in the viewport.");
    EditString("Prompt", ai_builder_settings_.prompt, 256);
    ImGui::InputInt("Width Tiles", &ai_builder_settings_.width_tiles);
    ImGui::InputInt("Height Tiles", &ai_builder_settings_.height_tiles);
    ImGui::InputInt("Seed", &ai_builder_settings_.seed);
    ImGui::SliderInt("Zone Count", &ai_builder_settings_.zone_count, 1, 8);
    ImGui::SliderInt("Build Steps / Sec", &ai_builder_settings_.operations_per_second, 1, 120);
    ImGui::SliderFloat("Platform Density", &ai_builder_settings_.platform_density, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Prop Density", &ai_builder_settings_.prop_density, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Light Density", &ai_builder_settings_.light_density, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Richness", &ai_builder_settings_.richness, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Verticality", &ai_builder_settings_.verticality, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Atmosphere", &ai_builder_settings_.atmosphere, 0.0f, 1.0f, "%.2f");
    ImGui::Separator();
    ImGui::TextUnformatted("Lighting");
    ImGui::SliderFloat("Ambient Intensity", &ai_builder_settings_.ambient_intensity, 0.02f, 0.95f, "%.2f");
    ImGui::SliderFloat("Light Radius Scale", &ai_builder_settings_.light_radius_scale, 0.4f, 2.5f, "%.2f");
    ImGui::SliderFloat("Light Intensity Scale", &ai_builder_settings_.light_intensity_scale, 0.4f, 2.5f, "%.2f");
    ImGui::Separator();
    ImGui::TextUnformatted("Camera");
    ImGui::SliderFloat("Camera Zoom", &ai_builder_settings_.camera_zoom, 0.45f, 1.8f, "%.2f");
    ImGui::SliderFloat("Camera Follow Lag", &ai_builder_settings_.camera_follow_lag, 0.0f, 12.0f, "%.2f");
    ImGui::SliderFloat("Camera Zoom Lag", &ai_builder_settings_.camera_zoom_lag, 0.0f, 12.0f, "%.2f");
    ImGui::SliderFloat("Camera Dead Zone", &ai_builder_settings_.camera_dead_zone_scale, 0.05f, 0.45f, "%.2f");
    ImGui::SliderFloat("Camera Overlap", &ai_builder_settings_.camera_overlap, 0.0f, 0.45f, "%.2f");
    ImGui::Separator();
    ImGui::Checkbox("Include Lights", &ai_builder_settings_.include_lights);
    ImGui::SameLine();
    ImGui::Checkbox("Include Audio", &ai_builder_settings_.include_audio);
    ImGui::SameLine();
    ImGui::Checkbox("Include Cameras", &ai_builder_settings_.include_cameras);
    ImGui::Checkbox("Include Parallax", &ai_builder_settings_.include_parallax);
    ImGui::SameLine();
    ImGui::Checkbox("Auto Post Effects", &ai_builder_settings_.include_post_effects);
    ImGui::Checkbox("Use Raw Images", &ai_builder_settings_.include_raw_images);
    ImGui::SameLine();
    ImGui::Checkbox("Clear Previous AI Build", &ai_builder_settings_.clear_existing_ai);

    ai_builder_settings_.width_tiles = std::clamp(ai_builder_settings_.width_tiles, 12, 256);
    ai_builder_settings_.height_tiles = std::clamp(ai_builder_settings_.height_tiles, 8, 96);
    ai_builder_settings_.zone_count = std::clamp(ai_builder_settings_.zone_count, 1, 8);
    ai_builder_settings_.operations_per_second = std::clamp(ai_builder_settings_.operations_per_second, 1, 120);
    ai_builder_settings_.platform_density = std::clamp(ai_builder_settings_.platform_density, 0.0f, 1.0f);
    ai_builder_settings_.prop_density = std::clamp(ai_builder_settings_.prop_density, 0.0f, 1.0f);
    ai_builder_settings_.light_density = std::clamp(ai_builder_settings_.light_density, 0.0f, 1.0f);
    ai_builder_settings_.richness = std::clamp(ai_builder_settings_.richness, 0.0f, 1.0f);
    ai_builder_settings_.verticality = std::clamp(ai_builder_settings_.verticality, 0.0f, 1.0f);
    ai_builder_settings_.atmosphere = std::clamp(ai_builder_settings_.atmosphere, 0.0f, 1.0f);
    ai_builder_settings_.ambient_intensity = std::clamp(ai_builder_settings_.ambient_intensity, 0.02f, 0.95f);
    ai_builder_settings_.light_radius_scale = std::clamp(ai_builder_settings_.light_radius_scale, 0.4f, 2.5f);
    ai_builder_settings_.light_intensity_scale = std::clamp(ai_builder_settings_.light_intensity_scale, 0.4f, 2.5f);
    ai_builder_settings_.camera_zoom = std::clamp(ai_builder_settings_.camera_zoom, 0.45f, 1.8f);
    ai_builder_settings_.camera_follow_lag = std::clamp(ai_builder_settings_.camera_follow_lag, 0.0f, 12.0f);
    ai_builder_settings_.camera_zoom_lag = std::clamp(ai_builder_settings_.camera_zoom_lag, 0.0f, 12.0f);
    ai_builder_settings_.camera_dead_zone_scale = std::clamp(ai_builder_settings_.camera_dead_zone_scale, 0.05f, 0.45f);
    ai_builder_settings_.camera_overlap = std::clamp(ai_builder_settings_.camera_overlap, 0.0f, 0.45f);

    if (ImGui::Button(ai_build_running_ ? "Restart AI Build" : "Start AI Build")) {
        StartAiLevelBuild();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop Build")) {
        CancelAiLevelBuild();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear AI Generated")) {
        ClearAiGeneratedContent();
    }

    if (!ai_build_queue_.empty()) {
        const float progress = ai_build_queue_.empty()
            ? 1.0f
            : static_cast<float>(std::min(ai_build_cursor_, ai_build_queue_.size())) / static_cast<float>(ai_build_queue_.size());
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
        ImGui::Text("Queued Steps: %d / %d", static_cast<int>(std::min(ai_build_cursor_, ai_build_queue_.size())), static_cast<int>(ai_build_queue_.size()));
    } else {
        ImGui::TextDisabled("No active build plan.");
    }

    ImGui::Separator();
    ImGui::Text("Available Assets: objects %d, images %d, audio %d", object_count, image_count, audio_count);
    ImGui::TextDisabled("Generated content is grouped as 'AI Generated' so it can be cleared in one click.");
    ImGui::TextDisabled("The builder now composes the level by zones, landmarks, atmosphere, parallax, light rhythm and camera coverage.");

    ImGui::End();
}

void EditorApp::CancelAiLevelBuild() {
    ai_build_running_ = false;
    ai_build_accumulator_ = 0.0f;
    status_message_ = "AI builder stopped.";
}

void EditorApp::ClearAiGeneratedContent() {
    CancelAiLevelBuild();

    auto& level = scene_.EditableLevel();
    auto is_ai_group = [](const auto& item) {
        return item.editor_group == "AI Generated";
    };

    const auto erase_group = [&](auto& collection) {
        collection.erase(std::remove_if(collection.begin(), collection.end(), is_ai_group), collection.end());
    };

    erase_group(level.entities);
    erase_group(level.triggers);
    erase_group(level.parallax_layers);
    erase_group(level.lights);
    erase_group(level.audio_sources);
    erase_group(level.audio_paks);
    erase_group(level.virtual_cameras);
    for (const auto& effect : ai_generated_post_effects_) {
        level.post_effects.erase(std::remove(level.post_effects.begin(), level.post_effects.end(), effect), level.post_effects.end());
    }
    ai_generated_post_effects_.clear();

    ai_build_queue_.clear();
    ai_build_cursor_ = 0;
    PruneHierarchySelection();
    scene_.ResetSimulation(false);
    pending_history_label_ = "Clear AI generated";
    status_message_ = "Cleared AI generated content.";
}

void EditorApp::ApplyAiBuildOperation(const AiBuildOperation& operation) {
    auto& level = scene_.EditableLevel();

    switch (operation.kind) {
    case AiBuildOperationKind::PlaceObject: {
        if (!core::FileIO::Exists(project_root_ / operation.resource_path)) {
            return;
        }
        const assets::ObjectAsset object = assets::LoadObjectAsset(project_root_ / operation.resource_path);
        assets::EntityDefinition entity;
        entity.id = "ai_entity_" + std::to_string(level.entities.size());
        ApplyObjectAssetToEntity(entity, object, operation.resource_path);
        entity.position = operation.position - entity.size * 0.5f;
        entity.visible = true;
        entity.editor_group = "AI Generated";
        if (operation.label == "floor" || operation.label == "platform") {
            entity.collidable = true;
            entity.dynamic = false;
            entity.collider_offset = {0.0f, 0.0f};
            entity.collider_size = entity.size;
        }
        level.entities.push_back(std::move(entity));
        break;
    }
    case AiBuildOperationKind::PlaceTexture: {
        if (!core::FileIO::Exists(project_root_ / operation.resource_path)) {
            return;
        }
        renderer::Texture2D& texture = asset_manager_.LoadTexture(operation.resource_path);
        glm::vec2 size = operation.size;
        if (size.x <= 0.0f || size.y <= 0.0f) {
            size = {
                static_cast<float>(std::max(texture.Size().x, 16)),
                static_cast<float>(std::max(texture.Size().y, 16))
            };
        }

        assets::EntityDefinition entity;
        entity.id = "ai_entity_" + std::to_string(level.entities.size());
        entity.archetype = std::filesystem::path(operation.resource_path).stem().string();
        entity.position = operation.position - size * 0.5f;
        entity.size = size;
        entity.texture = operation.resource_path;
        entity.tint = operation.color;
        entity.visible = true;
        entity.editor_group = "AI Generated";
        if (operation.label == "floor" || operation.label == "platform") {
            entity.collidable = true;
            entity.collider_size = size;
        }
        level.entities.push_back(std::move(entity));
        break;
    }
    case AiBuildOperationKind::AddLight: {
        level.lights.push_back({
            .id = "ai_light_" + std::to_string(level.lights.size()),
            .name = operation.label.empty() ? "AI Light" : operation.label,
            .position = operation.position,
            .radius = std::max(operation.radius, 96.0f),
            .color = operation.color,
            .intensity = std::max(operation.intensity, 0.05f),
            .enabled = operation.enabled,
            .editor_group = "AI Generated"
        });
        break;
    }
    case AiBuildOperationKind::AddAudioSource: {
        level.audio_sources.push_back({
            .id = "ai_audio_" + std::to_string(level.audio_sources.size()),
            .name = operation.label.empty() ? std::filesystem::path(operation.resource_path).stem().string() : operation.label,
            .audio = operation.resource_path,
            .position = operation.position,
            .enabled = true,
            .always = false,
            .loop = true,
            .radius = std::max(operation.radius, 220.0f),
            .stop_on_exit = false,
            .distance = std::max(operation.radius * 2.5f, 620.0f),
            .volume = std::clamp(operation.volume, 0.0f, 1.0f),
            .editor_group = "AI Generated"
        });
        break;
    }
    case AiBuildOperationKind::AddVirtualCamera: {
        const glm::vec2 size{
            std::max(operation.size.x, 320.0f),
            std::max(operation.size.y, 180.0f)
        };
        const float dead_zone_scale = std::clamp(operation.dead_zone_scale, 0.05f, 0.45f);
        level.virtual_cameras.push_back({
            .id = "ai_vcam_" + std::to_string(level.virtual_cameras.size()),
            .name = operation.label.empty() ? "AI Camera" : operation.label,
            .position = operation.position,
            .size = size,
            .follow_target = operation.follow_target.empty() ? "player" : operation.follow_target,
            .follow_offset = {0.0f, 0.0f},
            .dead_zone = {size.x * dead_zone_scale, size.y * dead_zone_scale * 0.82f},
            .zoom = std::max(operation.zoom, 0.05f),
            .follow_lag = std::max(operation.follow_lag, 0.0f),
            .zoom_lag = std::max(operation.zoom_lag, 0.0f),
            .enabled = operation.enabled,
            .auto_activate = operation.auto_activate,
            .release_on_exit = operation.release_on_exit,
            .override_mode = false,
            .camera_mode = scene_.CameraModeName(),
            .editor_group = "AI Generated"
        });
        break;
    }
    case AiBuildOperationKind::AddParallax: {
        level.parallax_layers.push_back({
            .id = "ai_parallax_" + std::to_string(level.parallax_layers.size()),
            .name = operation.label.empty() ? "AI Parallax" : operation.label,
            .texture = operation.resource_path,
            .speed = operation.speed,
            .scale = operation.scale,
            .offset = operation.offset,
            .tint = operation.color,
            .depth = operation.depth,
            .zoom_factor = operation.zoom_factor,
            .repeat = operation.repeat,
            .visible = operation.enabled,
            .editor_group = "AI Generated"
        });
        break;
    }
    }
}

void EditorApp::StartAiLevelBuild() {
    CancelAiLevelBuild();

    ai_builder_settings_.width_tiles = std::clamp(ai_builder_settings_.width_tiles, 12, 256);
    ai_builder_settings_.height_tiles = std::clamp(ai_builder_settings_.height_tiles, 8, 96);
    ai_builder_settings_.zone_count = std::clamp(ai_builder_settings_.zone_count, 1, 8);
    ai_builder_settings_.operations_per_second = std::clamp(ai_builder_settings_.operations_per_second, 1, 120);
    ai_builder_settings_.platform_density = std::clamp(ai_builder_settings_.platform_density, 0.0f, 1.0f);
    ai_builder_settings_.prop_density = std::clamp(ai_builder_settings_.prop_density, 0.0f, 1.0f);
    ai_builder_settings_.light_density = std::clamp(ai_builder_settings_.light_density, 0.0f, 1.0f);
    ai_builder_settings_.richness = std::clamp(ai_builder_settings_.richness, 0.0f, 1.0f);
    ai_builder_settings_.verticality = std::clamp(ai_builder_settings_.verticality, 0.0f, 1.0f);
    ai_builder_settings_.atmosphere = std::clamp(ai_builder_settings_.atmosphere, 0.0f, 1.0f);
    ai_builder_settings_.ambient_intensity = std::clamp(ai_builder_settings_.ambient_intensity, 0.02f, 0.95f);
    ai_builder_settings_.light_radius_scale = std::clamp(ai_builder_settings_.light_radius_scale, 0.4f, 2.5f);
    ai_builder_settings_.light_intensity_scale = std::clamp(ai_builder_settings_.light_intensity_scale, 0.4f, 2.5f);
    ai_builder_settings_.camera_zoom = std::clamp(ai_builder_settings_.camera_zoom, 0.45f, 1.8f);
    ai_builder_settings_.camera_follow_lag = std::clamp(ai_builder_settings_.camera_follow_lag, 0.0f, 12.0f);
    ai_builder_settings_.camera_zoom_lag = std::clamp(ai_builder_settings_.camera_zoom_lag, 0.0f, 12.0f);
    ai_builder_settings_.camera_dead_zone_scale = std::clamp(ai_builder_settings_.camera_dead_zone_scale, 0.05f, 0.45f);
    ai_builder_settings_.camera_overlap = std::clamp(ai_builder_settings_.camera_overlap, 0.0f, 0.45f);

    auto& level = scene_.EditableLevel();
    for (const auto& effect : ai_generated_post_effects_) {
        level.post_effects.erase(std::remove(level.post_effects.begin(), level.post_effects.end(), effect), level.post_effects.end());
    }
    ai_generated_post_effects_.clear();

    if (ai_builder_settings_.clear_existing_ai) {
        ClearAiGeneratedContent();
    }

    std::mt19937 rng(static_cast<std::mt19937::result_type>(ai_builder_settings_.seed));
    const std::vector<std::string> prompt_tokens = TokenizeWords(ai_builder_settings_.prompt);
    const std::string prompt_text = ToLowerCopy(ai_builder_settings_.prompt);

    std::vector<AiObjectCandidate> object_candidates;
    for (const auto& relative_path : EnumerateRelativeFiles("resources/objects", ".nobject")) {
        const std::filesystem::path full_path = project_root_ / relative_path;
        if (!core::FileIO::Exists(full_path)) {
            continue;
        }
        AiObjectCandidate candidate;
        candidate.path = relative_path.generic_string();
        candidate.asset = assets::LoadObjectAsset(full_path);
        candidate.searchable = ToLowerCopy(candidate.path + " " + candidate.asset.name + " " + candidate.asset.preset + " " + candidate.asset.properties.dump());
        object_candidates.push_back(std::move(candidate));
    }

    std::vector<AiTextureCandidate> texture_candidates;
    if (ai_builder_settings_.include_raw_images) {
        for (const auto& relative_path : EnumerateRelativeFiles("assets/images")) {
            if (!IsImageExtension(relative_path)) {
                continue;
            }
            renderer::Texture2D& texture = asset_manager_.LoadTexture(relative_path.generic_string());
            texture_candidates.push_back({
                .path = relative_path.generic_string(),
                .size = {
                    static_cast<float>(std::max(texture.Size().x, 16)),
                    static_cast<float>(std::max(texture.Size().y, 16))
                },
                .searchable = ToLowerCopy(relative_path.generic_string())
            });
        }
    }

    std::vector<AiAudioCandidate> audio_candidates;
    if (ai_builder_settings_.include_audio) {
        for (const auto& relative_path : EnumerateRelativeFiles("assets/audio")) {
            if (!IsAudioExtension(relative_path)) {
                continue;
            }
            audio_candidates.push_back({
                .path = relative_path.generic_string(),
                .searchable = ToLowerCopy(relative_path.generic_string())
            });
        }
    }

    if (object_candidates.empty() && texture_candidates.empty()) {
        ai_build_queue_.clear();
        ai_build_cursor_ = 0;
        status_message_ = "AI builder: no object resources or image assets found in the project.";
        return;
    }

    auto pick_best_object = [&](const auto& scorer) -> const AiObjectCandidate* {
        const AiObjectCandidate* best = nullptr;
        float best_score = -100000.0f;
        for (const auto& candidate : object_candidates) {
            const float score = scorer(candidate);
            if (score > best_score) {
                best_score = score;
                best = &candidate;
            }
        }
        return best;
    };

    auto pick_best_texture = [&](const auto& scorer) -> const AiTextureCandidate* {
        const AiTextureCandidate* best = nullptr;
        float best_score = -100000.0f;
        for (const auto& candidate : texture_candidates) {
            const float score = scorer(candidate);
            if (score > best_score) {
                best_score = score;
                best = &candidate;
            }
        }
        return best;
    };

    auto pick_best_audio = [&](const auto& scorer) -> const AiAudioCandidate* {
        const AiAudioCandidate* best = nullptr;
        float best_score = -100000.0f;
        for (const auto& candidate : audio_candidates) {
            const float score = scorer(candidate);
            if (score > best_score) {
                best_score = score;
                best = &candidate;
            }
        }
        return best;
    };

    auto collect_objects = [&](const auto& scorer, float threshold) {
        std::vector<const AiObjectCandidate*> result;
        for (const auto& candidate : object_candidates) {
            if (scorer(candidate) >= threshold) {
                result.push_back(&candidate);
            }
        }
        return result;
    };

    auto collect_textures = [&](const auto& scorer, float threshold) {
        std::vector<const AiTextureCandidate*> result;
        for (const auto& candidate : texture_candidates) {
            if (scorer(candidate) >= threshold) {
                result.push_back(&candidate);
            }
        }
        return result;
    };

    auto score_floor_object = [&](const AiObjectCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        if (candidate.asset.collidable) {
            score += 3.0f;
        }
        if (candidate.asset.size.x >= candidate.asset.size.y * 1.2f) {
            score += 1.5f;
        }
        if (ContainsAnyToken(candidate.searchable, {"floor", "ground", "platform", "bridge", "road", "tile", "stone", "wall", "beam"})) {
            score += 4.0f;
        }
        if (ContainsAnyToken(candidate.searchable, {"lamp", "light", "torch", "tree", "bush", "door", "hero", "player", "enemy"})) {
            score -= 1.5f;
        }
        return score;
    };
    auto score_platform_object = [&](const AiObjectCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        if (candidate.asset.collidable) {
            score += 2.5f;
        }
        if (ContainsAnyToken(candidate.searchable, {"platform", "beam", "ledge", "bridge", "plank", "pipe", "wall"})) {
            score += 4.0f;
        }
        if (candidate.asset.size.x >= candidate.asset.size.y) {
            score += 1.0f;
        }
        return score;
    };
    auto score_decor_object = [&](const AiObjectCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        if (!candidate.asset.collidable) {
            score += 2.0f;
        }
        if (ContainsAnyToken(candidate.searchable, {"tree", "rock", "crate", "barrel", "bush", "grass", "plant", "pipe", "terminal", "sign", "panel", "machine", "box", "banner", "column", "door", "window", "lamp", "torch"})) {
            score += 3.0f;
        }
        return score;
    };
    auto score_light_object = [&](const AiObjectCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        if (ContainsAnyToken(candidate.searchable, {"lamp", "light", "torch", "lantern", "neon", "window"})) {
            score += 5.0f;
        }
        return score;
    };
    auto score_goal_object = [&](const AiObjectCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        if (ContainsAnyToken(candidate.searchable, {"door", "gate", "portal", "elevator", "exit"})) {
            score += 5.0f;
        }
        return score;
    };
    auto score_floor_texture = [&](const AiTextureCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        if (candidate.size.x >= candidate.size.y * 1.2f) {
            score += 1.5f;
        }
        if (ContainsAnyToken(candidate.searchable, {"floor", "ground", "platform", "road", "tile", "stone", "wall", "beam"})) {
            score += 3.0f;
        }
        return score;
    };
    auto score_decor_texture = [&](const AiTextureCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        if (ContainsAnyToken(candidate.searchable, {"tree", "rock", "crate", "barrel", "bush", "grass", "plant", "pipe", "sign", "panel", "machine", "box", "banner", "column", "lamp", "door"})) {
            score += 3.0f;
        }
        return score;
    };
    auto score_landmark_object = [&](const AiObjectCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        score += std::min((candidate.asset.size.x + candidate.asset.size.y) / 220.0f, 3.0f);
        if (ContainsAnyToken(candidate.searchable, {"door", "gate", "portal", "tower", "statue", "tree", "crystal", "altar", "machine", "window", "generator", "terminal", "elevator", "bridge", "arch"})) {
            score += 4.0f;
        }
        return score;
    };
    auto score_parallax_texture = [&](const AiTextureCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens) * 0.6f;
        if (ContainsAnyToken(candidate.searchable, {"bg", "background", "sky", "cloud", "mountain", "forest", "tree", "city", "building", "ruin", "cave", "fog", "mist", "stars"})) {
            score += 4.0f;
        }
        if (candidate.size.x >= candidate.size.y) {
            score += 1.5f;
        }
        return score;
    };

    const AiObjectCandidate* floor_object = pick_best_object(score_floor_object);
    const AiObjectCandidate* platform_object = pick_best_object(score_platform_object);
    const AiObjectCandidate* light_object = pick_best_object(score_light_object);
    const AiObjectCandidate* goal_object = pick_best_object(score_goal_object);
    const AiTextureCandidate* floor_texture = pick_best_texture(score_floor_texture);
    const AiAudioCandidate* ambient_audio = pick_best_audio([&](const AiAudioCandidate& candidate) {
        float score = ScorePromptTokens(candidate.searchable, prompt_tokens);
        if (ContainsAnyToken(candidate.searchable, {"ambient", "ambience", "wind", "rain", "drone", "hum", "loop", "music"})) {
            score += 3.5f;
        }
        return score;
    });

    std::vector<const AiObjectCandidate*> decor_objects = collect_objects(score_decor_object, 1.5f);
    if (decor_objects.empty()) {
        decor_objects = collect_objects(score_decor_object, -1000.0f);
    }
    std::vector<const AiTextureCandidate*> decor_textures = collect_textures(score_decor_texture, 1.0f);
    if (decor_textures.empty()) {
        decor_textures = collect_textures(score_decor_texture, -1000.0f);
    }
    std::vector<const AiObjectCandidate*> landmark_objects = collect_objects(score_landmark_object, 1.4f);
    if (landmark_objects.empty()) {
        landmark_objects = collect_objects(score_landmark_object, -1000.0f);
    }
    std::vector<const AiTextureCandidate*> parallax_textures = collect_textures(score_parallax_texture, 1.5f);
    if (parallax_textures.empty()) {
        parallax_textures = collect_textures(score_parallax_texture, -1000.0f);
    }

    auto mix_color = [](glm::vec4 a, glm::vec4 b, float t) {
        return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
    };
    auto push_effect_if_available = [&](const char* name) {
        if (!core::FileIO::Exists(ActiveShaderRoot() / (std::string(name) + ".frag"))) {
            return;
        }
        if (std::find(level.post_effects.begin(), level.post_effects.end(), name) == level.post_effects.end()) {
            level.post_effects.push_back(name);
        }
        ai_generated_post_effects_.push_back(name);
    };
    const float tile_w = static_cast<float>(std::max(level.tile_width, 1));
    const float tile_h = static_cast<float>(std::max(level.tile_height, 1));
    const float world_width = static_cast<float>(ai_builder_settings_.width_tiles) * tile_w;
    const float world_height = static_cast<float>(ai_builder_settings_.height_tiles) * tile_h;
    const float richness = ai_builder_settings_.richness;
    const float verticality = ai_builder_settings_.verticality;
    const float atmosphere = ai_builder_settings_.atmosphere;
    const float ground_y = std::max(world_height - tile_h * (1.7f - richness * 0.35f), tile_h * 6.0f);

    glm::vec4 clear_base{0.08f, 0.09f, 0.11f, 1.0f};
    glm::vec4 ambient_base{0.14f, 0.16f, 0.20f, 1.0f};
    glm::vec4 light_primary{1.0f, 0.84f, 0.58f, 0.92f};
    glm::vec4 light_secondary{0.56f, 0.78f, 1.0f, 0.88f};
    glm::vec4 parallax_tint{0.72f, 0.78f, 0.88f, 0.58f};
    bool moody_theme = false;

    if (ContainsAnyToken(prompt_text, {"forest", "jungle", "swamp"})) {
        clear_base = {0.05f, 0.11f, 0.09f, 1.0f};
        ambient_base = {0.13f, 0.20f, 0.16f, 1.0f};
        light_primary = {0.98f, 0.90f, 0.62f, 0.92f};
        light_secondary = {0.55f, 0.86f, 0.68f, 0.86f};
        parallax_tint = {0.74f, 0.90f, 0.76f, 0.55f};
        moody_theme = true;
    } else if (ContainsAnyToken(prompt_text, {"desert", "sand", "canyon"})) {
        clear_base = {0.18f, 0.13f, 0.09f, 1.0f};
        ambient_base = {0.26f, 0.18f, 0.10f, 1.0f};
        light_primary = {1.0f, 0.78f, 0.42f, 0.94f};
        light_secondary = {0.98f, 0.58f, 0.42f, 0.84f};
        parallax_tint = {0.96f, 0.78f, 0.58f, 0.45f};
    } else if (ContainsAnyToken(prompt_text, {"snow", "ice", "winter"})) {
        clear_base = {0.10f, 0.15f, 0.20f, 1.0f};
        ambient_base = {0.18f, 0.23f, 0.30f, 1.0f};
        light_primary = {0.74f, 0.88f, 1.0f, 0.94f};
        light_secondary = {1.0f, 0.94f, 0.82f, 0.82f};
        parallax_tint = {0.82f, 0.90f, 1.0f, 0.58f};
        moody_theme = true;
    } else if (ContainsAnyToken(prompt_text, {"night", "neon", "cyber", "tech"})) {
        clear_base = {0.04f, 0.05f, 0.09f, 1.0f};
        ambient_base = {0.10f, 0.12f, 0.20f, 1.0f};
        light_primary = {0.46f, 0.84f, 1.0f, 0.96f};
        light_secondary = {1.0f, 0.38f, 0.72f, 0.88f};
        parallax_tint = {0.78f, 0.72f, 1.0f, 0.50f};
        moody_theme = true;
    } else if (ContainsAnyToken(prompt_text, {"cave", "dungeon", "underground"})) {
        clear_base = {0.05f, 0.05f, 0.06f, 1.0f};
        ambient_base = {0.14f, 0.13f, 0.12f, 1.0f};
        light_primary = {1.0f, 0.72f, 0.42f, 0.96f};
        light_secondary = {0.68f, 0.86f, 1.0f, 0.82f};
        parallax_tint = {0.58f, 0.56f, 0.62f, 0.50f};
        moody_theme = true;
    }

    level.clear_color = mix_color(clear_base, {0.02f, 0.02f, 0.03f, 1.0f}, atmosphere * (moody_theme ? 0.34f : 0.18f));
    level.lighting.ambient_color = mix_color(ambient_base, light_primary, 0.14f * richness);
    if (ai_builder_settings_.include_lights) {
        level.lighting.enabled = true;
        level.lighting.rt_enabled = true;
        level.lighting.ambient_intensity = ai_builder_settings_.ambient_intensity;
    } else {
        level.lighting.ambient_intensity = std::clamp(ai_builder_settings_.ambient_intensity + 0.18f, 0.1f, 1.2f);
    }
    level.player_spawn = {tile_w * 2.0f, ground_y - tile_h * 2.5f};

    if (ai_builder_settings_.include_post_effects) {
        if (richness > 0.55f) {
            push_effect_if_available("bloom");
        }
        if (atmosphere > 0.36f) {
            push_effect_if_available("vignette");
        }
        if (ContainsAnyToken(prompt_text, {"fog", "mist", "forest", "swamp", "cave"}) && atmosphere > 0.28f) {
            push_effect_if_available("fog");
        }
        if (ContainsAnyToken(prompt_text, {"rain", "storm", "wet"})) {
            push_effect_if_available("rain");
        }
        if (ContainsAnyToken(prompt_text, {"neon", "cyber", "glitch"})) {
            push_effect_if_available("chromatic");
        }
    }

    ai_build_queue_.clear();
    ai_build_cursor_ = 0;
    ai_build_accumulator_ = 0.0f;

    struct SurfaceAnchor {
        glm::vec2 position{0.0f, 0.0f};
        int zone = 0;
        int column = 0;
    };
    struct ZoneLayout {
        float start_x = 0.0f;
        float end_x = 0.0f;
        float center_x = 0.0f;
        float target_surface = 0.0f;
    };

    std::vector<SurfaceAnchor> surface_anchors;
    std::vector<glm::vec2> landmark_points;

    auto queue_object = [&](const std::string& path, glm::vec2 center, const std::string& label) {
        ai_build_queue_.push_back({
            .kind = AiBuildOperationKind::PlaceObject,
            .resource_path = path,
            .label = label,
            .position = center
        });
    };
    auto queue_texture = [&](const std::string& path, glm::vec2 center, glm::vec2 size, const std::string& label) {
        ai_build_queue_.push_back({
            .kind = AiBuildOperationKind::PlaceTexture,
            .resource_path = path,
            .label = label,
            .position = center,
            .size = size
        });
    };
    auto queue_parallax = [&](const std::string& path, const std::string& label, glm::vec4 tint, float depth, float zoom_factor, glm::vec2 speed, glm::vec2 scale, glm::vec2 offset) {
        ai_build_queue_.push_back({
            .kind = AiBuildOperationKind::AddParallax,
            .resource_path = path,
            .label = label,
            .color = tint,
            .depth = depth,
            .zoom_factor = zoom_factor,
            .speed = speed,
            .scale = scale,
            .offset = offset,
            .enabled = true,
            .repeat = true
        });
    };

    const bool use_floor_object = floor_object != nullptr &&
        (floor_texture == nullptr || score_floor_object(*floor_object) >= score_floor_texture(*floor_texture));
    glm::vec2 floor_size = use_floor_object && floor_object != nullptr
        ? floor_object->asset.size
        : (floor_texture != nullptr ? floor_texture->size : glm::vec2{tile_w * 2.0f, tile_h * 2.0f});
    floor_size.x = std::max(floor_size.x, tile_w * 2.0f);
    floor_size.y = std::max(floor_size.y, tile_h * 1.5f);

    if (ai_builder_settings_.include_parallax && !parallax_textures.empty()) {
        const int parallax_count = std::clamp(1 + static_cast<int>(std::round(richness * 2.0f + atmosphere * 1.2f)), 1, 4);
        std::vector<std::string> used_paths;
        const std::array<float, 4> depth_values{-480.0f, -390.0f, -320.0f, -250.0f};
        const std::array<float, 4> zoom_values{0.78f, 0.86f, 0.94f, 1.0f};
        for (int i = 0; i < parallax_count; ++i) {
            const AiTextureCandidate* candidate = nullptr;
            for (const auto* option : parallax_textures) {
                if (std::find(used_paths.begin(), used_paths.end(), option->path) == used_paths.end()) {
                    candidate = option;
                    break;
                }
            }
            if (candidate == nullptr) {
                candidate = parallax_textures[static_cast<std::size_t>(i % parallax_textures.size())];
            }
            used_paths.push_back(candidate->path);
            const float layer_mix = static_cast<float>(i) / std::max(parallax_count - 1, 1);
            queue_parallax(
                candidate->path,
                "AI Parallax " + std::to_string(i + 1),
                mix_color(parallax_tint, light_secondary, 0.18f * layer_mix),
                depth_values[static_cast<std::size_t>(std::min(i, static_cast<int>(depth_values.size()) - 1))],
                zoom_values[static_cast<std::size_t>(std::min(i, static_cast<int>(zoom_values.size()) - 1))],
                {0.04f + 0.06f * layer_mix, 0.0f},
                {1.0f + richness * 0.15f, 1.0f + richness * 0.08f},
                {0.0f, -world_height * (0.10f + layer_mix * 0.06f)}
            );
        }
    }

    const int zone_count = std::clamp(ai_builder_settings_.zone_count, 1, 8);
    const float zone_width = world_width / static_cast<float>(zone_count);
    std::vector<ZoneLayout> zones(zone_count);
    std::uniform_real_distribution<float> unit_distribution(0.0f, 1.0f);
    const float base_surface_y = ground_y - floor_size.y;
    const float high_surface_y = std::max(tile_h * (3.0f + (1.0f - verticality) * 1.5f), tile_h * 2.0f);
    const float low_surface_y = std::max(base_surface_y, high_surface_y + tile_h * 4.0f);
    for (int i = 0; i < zone_count; ++i) {
        auto& zone = zones[static_cast<std::size_t>(i)];
        zone.start_x = static_cast<float>(i) * zone_width;
        zone.end_x = zone.start_x + zone_width;
        zone.center_x = zone.start_x + zone_width * 0.5f;
        const float wave = std::sin(static_cast<float>(i) * 0.9f + static_cast<float>(ai_builder_settings_.seed % 17) * 0.13f);
        const float random_bias = (unit_distribution(rng) - 0.5f) * tile_h * (1.5f + verticality * 4.0f);
        zone.target_surface = std::clamp(
            base_surface_y - wave * tile_h * (1.5f + verticality * 4.5f) + random_bias,
            high_surface_y,
            low_surface_y
        );
    }

    const int floor_segments = std::max(zone_count * 5, static_cast<int>(std::ceil(world_width / std::max(floor_size.x * 0.92f, tile_w * 2.0f))));
    float current_surface = zones.front().target_surface;
    for (int i = 0; i < floor_segments; ++i) {
        const float normalized = static_cast<float>(i) / std::max(floor_segments - 1, 1);
        const float center_x = (static_cast<float>(i) + 0.5f) * (world_width / static_cast<float>(floor_segments));
        const int zone_index = std::clamp(static_cast<int>(center_x / std::max(zone_width, 1.0f)), 0, zone_count - 1);
        const float target_surface = zones[static_cast<std::size_t>(zone_index)].target_surface;
        const float zone_wave = std::sin(normalized * 6.28318f * static_cast<float>(zone_count) + static_cast<float>(ai_builder_settings_.seed % 29) * 0.17f);
        const float desired_surface = std::clamp(
            target_surface + zone_wave * tile_h * (0.2f + verticality * 0.8f),
            high_surface_y,
            low_surface_y
        );
        const float approach = (desired_surface - current_surface) * (0.18f + verticality * 0.22f);
        const float noise = (unit_distribution(rng) - 0.5f) * tile_h * (0.12f + verticality * 0.30f);
        current_surface = std::clamp(current_surface + approach + noise, high_surface_y, low_surface_y);
        const glm::vec2 center{center_x, current_surface + floor_size.y * 0.5f};
        if (use_floor_object && floor_object != nullptr) {
            queue_object(floor_object->path, center, "floor");
        } else if (floor_texture != nullptr) {
            queue_texture(floor_texture->path, center, floor_size, "floor");
        }
        surface_anchors.push_back({.position = {center_x, current_surface}, .zone = zone_index, .column = i});
    }

    const bool use_platform_object = platform_object != nullptr &&
        (floor_texture == nullptr || score_platform_object(*platform_object) >= score_floor_texture(*floor_texture) - 1.0f);
    const glm::vec2 platform_size = use_platform_object && platform_object != nullptr
        ? glm::vec2{std::max(platform_object->asset.size.x, tile_w * 2.0f), std::max(platform_object->asset.size.y, tile_h)}
        : glm::vec2{std::max(floor_size.x * 0.8f, tile_w * 2.0f), std::max(floor_size.y * 0.7f, tile_h)};
    const int terrace_clusters = std::clamp(
        static_cast<int>(std::round(static_cast<float>(zone_count) * (0.8f + richness * 1.6f) * (0.6f + verticality * 1.3f) * ai_builder_settings_.platform_density)),
        1,
        zone_count * 4);
    std::uniform_int_distribution<int> span_distribution(2, 5);
    for (int cluster = 0; cluster < terrace_clusters; ++cluster) {
        const int zone_index = cluster % zone_count;
        const auto zone = zones[static_cast<std::size_t>(zone_index)];
        const int span = span_distribution(rng);
        const float terrace_surface = std::clamp(
            zone.target_surface - tile_h * (2.0f + unit_distribution(rng) * (2.5f + verticality * 4.5f)),
            tile_h * 2.0f,
            low_surface_y - tile_h * 2.0f
        );
        const float span_width = static_cast<float>(span) * platform_size.x;
        const float base_x = std::clamp(
            zone.start_x + zone_width * (0.12f + unit_distribution(rng) * 0.58f),
            platform_size.x * 0.5f,
            std::max(world_width - span_width, platform_size.x));
        for (int segment = 0; segment < span; ++segment) {
            const float center_x = base_x + static_cast<float>(segment) * platform_size.x;
            const glm::vec2 center{center_x, terrace_surface + platform_size.y * 0.5f};
            if (use_platform_object && platform_object != nullptr) {
                queue_object(platform_object->path, center, "platform");
            } else if (floor_texture != nullptr) {
                queue_texture(floor_texture->path, center, platform_size, "platform");
            }
            surface_anchors.push_back({.position = {center_x, terrace_surface}, .zone = zone_index, .column = floor_segments + cluster * 8 + segment});
        }
    }

    auto find_anchor_near = [&](float x, int preferred_zone) -> const SurfaceAnchor* {
        const SurfaceAnchor* best = nullptr;
        float best_score = std::numeric_limits<float>::max();
        for (const auto& anchor : surface_anchors) {
            float score = std::abs(anchor.position.x - x);
            if (preferred_zone >= 0 && anchor.zone != preferred_zone) {
                score += zone_width * 0.35f;
            }
            if (score < best_score) {
                best_score = score;
                best = &anchor;
            }
        }
        return best;
    };

    if (goal_object != nullptr && score_goal_object(*goal_object) > 1.0f) {
        const SurfaceAnchor* goal_anchor = find_anchor_near(world_width - tile_w * 4.0f, zone_count - 1);
        const glm::vec2 center{
            goal_anchor != nullptr ? goal_anchor->position.x : (world_width - goal_object->asset.size.x * 0.75f),
            (goal_anchor != nullptr ? goal_anchor->position.y : base_surface_y) - goal_object->asset.size.y * 0.5f
        };
        queue_object(goal_object->path, center, "goal");
        landmark_points.push_back({center.x, center.y + goal_object->asset.size.y * 0.3f});
    }

    const int landmark_count = std::clamp(static_cast<int>(std::round(static_cast<float>(zone_count) * (0.8f + richness))), 1, zone_count + 3);
    for (int zone_index = 0; zone_index < landmark_count; ++zone_index) {
        if (landmark_objects.empty()) {
            break;
        }
        const int wrapped_zone = zone_index % zone_count;
        const SurfaceAnchor* anchor = find_anchor_near(zones[static_cast<std::size_t>(wrapped_zone)].center_x, wrapped_zone);
        if (anchor == nullptr) {
            continue;
        }
        const AiObjectCandidate* candidate = landmark_objects[static_cast<std::size_t>(zone_index % landmark_objects.size())];
        if (candidate == nullptr) {
            continue;
        }
        const glm::vec2 center{
            anchor->position.x,
            anchor->position.y - candidate->asset.size.y * 0.5f
        };
        queue_object(candidate->path, center, "landmark");
        landmark_points.push_back(anchor->position);
    }

    const int prop_count = std::clamp(
        static_cast<int>(std::round(static_cast<float>(ai_builder_settings_.width_tiles) * ai_builder_settings_.prop_density * (0.85f + richness * 1.55f))),
        4,
        120);
    const int cluster_count = std::clamp(static_cast<int>(std::round(static_cast<float>(zone_count) * (1.5f + richness * 2.2f))), 2, zone_count * 5);
    for (int cluster = 0; cluster < cluster_count && !surface_anchors.empty(); ++cluster) {
        const int zone_index = cluster % zone_count;
        const SurfaceAnchor* cluster_anchor = find_anchor_near(
            zones[static_cast<std::size_t>(zone_index)].start_x + zone_width * (0.18f + unit_distribution(rng) * 0.64f),
            zone_index);
        if (cluster_anchor == nullptr) {
            continue;
        }
        const int cluster_items = std::clamp(
            1 + static_cast<int>(std::round(unit_distribution(rng) * (1.0f + richness * 4.0f))),
            1,
            6);
        for (int item = 0; item < cluster_items && cluster * 4 + item < prop_count; ++item) {
            const bool use_object = !decor_objects.empty() && (decor_textures.empty() || unit_distribution(rng) > 0.28f);
            if (use_object) {
                const AiObjectCandidate* candidate = RandomChoice(decor_objects, rng);
                if (candidate == nullptr) {
                    continue;
                }
                const glm::vec2 center{
                    cluster_anchor->position.x + (unit_distribution(rng) - 0.5f) * tile_w * (1.2f + richness * 2.8f),
                    cluster_anchor->position.y - candidate->asset.size.y * 0.5f
                };
                queue_object(candidate->path, center, item == 0 ? "decor_cluster" : "decor");
            } else {
                const AiTextureCandidate* candidate = RandomChoice(decor_textures, rng);
                if (candidate == nullptr) {
                    continue;
                }
                const glm::vec2 center{
                    cluster_anchor->position.x + (unit_distribution(rng) - 0.5f) * tile_w * (1.0f + richness * 2.4f),
                    cluster_anchor->position.y - candidate->size.y * 0.5f
                };
                queue_texture(candidate->path, center, candidate->size, item == 0 ? "decor_cluster" : "decor");
            }
        }
    }

    if (ai_builder_settings_.include_lights && !surface_anchors.empty()) {
        const float density_factor = ai_builder_settings_.light_density * (0.55f + richness * 0.85f) * (moody_theme ? 1.25f : 1.0f);
        const int stride = std::clamp(static_cast<int>(std::round(7.0f - density_factor * 5.0f)), 2, 8);
        std::vector<glm::vec2> light_positions;
        for (std::size_t i = 0; i < surface_anchors.size(); i += static_cast<std::size_t>(stride)) {
            light_positions.push_back(surface_anchors[i].position);
        }
        for (const auto& landmark : landmark_points) {
            light_positions.push_back(landmark);
        }
        std::sort(light_positions.begin(), light_positions.end(), [](const glm::vec2& lhs, const glm::vec2& rhs) {
            return lhs.x < rhs.x;
        });
        std::vector<glm::vec2> unique_positions;
        for (const auto& position : light_positions) {
            if (!unique_positions.empty() && std::abs(unique_positions.back().x - position.x) < tile_w * 1.4f) {
                continue;
            }
            unique_positions.push_back(position);
        }
        for (std::size_t i = 0; i < unique_positions.size(); ++i) {
            const glm::vec2 anchor = unique_positions[i];
            float visual_height = tile_h * (1.8f + richness * 1.2f);
            if (light_object != nullptr && score_light_object(*light_object) > 1.0f) {
                const glm::vec2 center{
                    anchor.x,
                    anchor.y - light_object->asset.size.y * 0.5f
                };
                queue_object(light_object->path, center, "light_fixture");
                visual_height = light_object->asset.size.y;
            }
            const glm::vec4 light_color = (i % 3 == 0) ? light_secondary : light_primary;
            ai_build_queue_.push_back({
                .kind = AiBuildOperationKind::AddLight,
                .label = "AI Light",
                .position = {anchor.x, anchor.y - visual_height * 0.82f},
                .color = light_color,
                .radius = std::max(tile_w * (7.0f + richness * 8.0f) * ai_builder_settings_.light_radius_scale, 180.0f),
                .intensity = (moody_theme ? 1.12f : 0.92f) * ai_builder_settings_.light_intensity_scale
            });
        }
    }

    if (ai_builder_settings_.include_audio && ambient_audio != nullptr) {
        const int audio_sources = std::clamp(1 + static_cast<int>(std::round(atmosphere * 2.0f)), 1, std::max(1, zone_count / 2));
        for (int i = 0; i < audio_sources; ++i) {
            const float x = zone_width * (0.5f + static_cast<float>(i) * (static_cast<float>(zone_count) / static_cast<float>(audio_sources)));
            ai_build_queue_.push_back({
                .kind = AiBuildOperationKind::AddAudioSource,
                .resource_path = ambient_audio->path,
                .label = "AI Ambient",
                .position = {std::min(x, world_width - tile_w * 2.0f), ground_y - tile_h * 4.0f},
                .radius = std::max(world_width * (0.26f + atmosphere * 0.18f), 320.0f),
                .volume = 0.58f + atmosphere * 0.22f
            });
        }
    }

    if (ai_builder_settings_.include_cameras) {
        for (int index = 0; index < zone_count; ++index) {
            const float overlap = zone_width * ai_builder_settings_.camera_overlap;
            const float left = std::max(0.0f, zones[static_cast<std::size_t>(index)].start_x - overlap);
            const float width = std::min(world_width - left, zone_width + overlap * 2.0f);
            ai_build_queue_.push_back({
                .kind = AiBuildOperationKind::AddVirtualCamera,
                .label = "AI Camera " + std::to_string(index + 1),
                .position = {left, 0.0f},
                .size = {width, std::max(world_height, tile_h * 9.0f)},
                .zoom = ai_builder_settings_.camera_zoom + ((index % 2 == 0 ? -1.0f : 1.0f) * 0.03f * richness),
                .follow_lag = ai_builder_settings_.camera_follow_lag,
                .zoom_lag = ai_builder_settings_.camera_zoom_lag,
                .dead_zone_scale = ai_builder_settings_.camera_dead_zone_scale,
                .follow_target = "player",
                .enabled = true,
                .auto_activate = true,
                .release_on_exit = true
            });
        }
    }

    if (ai_build_queue_.empty()) {
        status_message_ = "AI builder could not create a plan from the current assets.";
        return;
    }

    ai_build_running_ = true;
    status_message_ = "AI builder queued " + std::to_string(ai_build_queue_.size()) + " steps.";
}

void EditorApp::DrawPerformancePanel() {
    if (!show_performance_panel_) {
        return;
    }
    if (!ImGui::Begin("[PERF] Performance###PerformanceWindow", &show_performance_panel_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.34f, 0.84f, 0.56f, 1.0f});

    if (ImGui::Checkbox("Pause Preview", &preview_paused_)) {
        status_message_ = preview_paused_ ? "Preview paused." : "Preview resumed.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Frame")) {
        preview_paused_ = true;
        preview_step_requested_ = true;
        status_message_ = "Preview step queued.";
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Trace Execution", &preview_trace_enabled_)) {
        scene_.SetDebugTraceEnabled(preview_trace_enabled_);
        status_message_ = preview_trace_enabled_ ? "Preview trace enabled." : "Preview trace disabled.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Log")) {
        scene_.ClearMessages();
        status_message_ = "Output log cleared.";
    }

    ImGui::SeparatorText("Frame");
    ImGui::Text("FPS %.1f | Frame %.2f ms | Update %.2f ms | Render %.2f ms | GUI %.2f ms | GPU %.2f ms",
        performance_.fps,
        performance_.frame_ms,
        performance_.update_ms,
        performance_.render_ms,
        performance_.gui_ms,
        performance_.gpu_frame_ms);
    if (!performance_.frame_ms_history.empty()) {
        ImGui::PlotLines("Frame ms", performance_.frame_ms_history.data(), static_cast<int>(performance_.frame_ms_history.size()), 0, nullptr, 0.0f, 40.0f, ImVec2(0.0f, 62.0f));
    }
    if (!performance_.fps_history.empty()) {
        ImGui::PlotLines("FPS", performance_.fps_history.data(), static_cast<int>(performance_.fps_history.size()), 0, nullptr, 0.0f, 240.0f, ImVec2(0.0f, 48.0f));
    }

    ImGui::SeparatorText("Runtime");
    ImGui::Text("Entities %d | Lights %d | Triggers %d | Cameras %d | Animations %d | Post FX %d",
        static_cast<int>(scene_.RuntimeEntities().size()),
        static_cast<int>(scene_.Level().lights.size()),
        static_cast<int>(scene_.Level().triggers.size()),
        static_cast<int>(scene_.Level().virtual_cameras.size()),
        scene_.PlayingSceneAnimationCount(),
        static_cast<int>(scene_.Level().post_effects.size()));
    ImGui::Text("Preview %s | Live Preview %s | Trace %s",
        preview_paused_ ? "Paused" : "Running",
        live_preview_ ? "On" : "Off",
        preview_trace_enabled_ ? "On" : "Off");
    const unsigned hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    ImGui::Text("Backend %s | Scene Prepass %s | Worker Threads %zu | HW Threads %u",
        project_.renderer_backend.c_str(),
        project_.multithreading_enabled ? "multithreaded" : "single-threaded",
        core::ThreadPool::Shared().WorkerCount(),
        hardware_threads);
    ImGui::Text("Scene Target %d x %d | Game Viewport %d x %d",
        scene_target_.Size().x,
        scene_target_.Size().y,
        project_.game_viewport_size.x,
        project_.game_viewport_size.y);

    ImGui::SeparatorText("CPU / Memory");
    ImGui::Text("System CPU %.1f%% | Process CPU %.1f%% | Core-equivalent %.1f%%",
        performance_.system_cpu_percent,
        performance_.process_cpu_percent,
        performance_.process_cpu_core_equivalent_percent);
    ImGui::Text("RAM %.0f / %.0f MB | Process RAM %.0f MB", performance_.ram_used_mb, performance_.ram_total_mb, performance_.process_ram_mb);
    ImGui::Text("VRAM %.0f / %.0f MB", performance_.vram_used_mb, performance_.vram_budget_mb);
    if (!performance_.core_cpu_usage.empty() && ImGui::CollapsingHeader("CPU Cores", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int core_index = 0; core_index < static_cast<int>(performance_.core_cpu_usage.size()); ++core_index) {
            const float load = std::clamp(performance_.core_cpu_usage[static_cast<std::size_t>(core_index)], 0.0f, 100.0f);
            const std::string label = "Core " + std::to_string(core_index) + " " + std::to_string(static_cast<int>(std::round(load))) + "%";
            ImGui::ProgressBar(load / 100.0f, ImVec2(-1.0f, 0.0f), label.c_str());
        }
    }

    ImGui::SeparatorText("Render Passes");
    if (ImGui::BeginTable("PerformancePassTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("CPU ms");
        ImGui::TableHeadersRow();
        for (const auto& pass : scene_.LastRenderPassTimings()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(pass.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", pass.cpu_ms);
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Scene Total");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f", scene_.LastRenderTotalCpuMs());
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Post Effects");
    ImGui::Text("Post stack total: %.3f ms", post_stack_.LastTotalCpuMs());
    if (post_stack_.LastEffectTimings().empty()) {
        ImGui::TextDisabled("No active post effects.");
    } else if (ImGui::BeginTable("PerformancePostFxTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Effect");
        ImGui::TableSetupColumn("CPU ms");
        ImGui::TableHeadersRow();
        for (const auto& effect : post_stack_.LastEffectTimings()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(effect.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", effect.cpu_ms);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void EditorApp::DrawMessagesPanel() {
    if (!show_output_panel_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("LOG", Tr("editor.window.output", "Output"), "OutputWindow").c_str(), &show_output_panel_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.22f, 0.52f, 0.90f, 1.0f});
    ImGui::Text("Preview %s | Trace %s", preview_paused_ ? "Paused" : "Running", preview_trace_enabled_ ? "On" : "Off");
    if (ImGui::Button("Pause/Resume")) {
        preview_paused_ = !preview_paused_;
        status_message_ = preview_paused_ ? "Preview paused." : "Preview resumed.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Frame")) {
        preview_paused_ = true;
        preview_step_requested_ = true;
        status_message_ = "Preview step queued.";
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Trace Execution##Output", &preview_trace_enabled_)) {
        scene_.SetDebugTraceEnabled(preview_trace_enabled_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Output")) {
        scene_.ClearMessages();
    }
    ImGui::Separator();
    if (!status_message_.empty()) {
        ImGui::TextWrapped("%s", status_message_.c_str());
        ImGui::Separator();
    }
    if (build_in_progress_ || !build_log_.empty()) {
        ImGui::TextUnformatted("Build Report");
        ImGui::ProgressBar(build_progress_, ImVec2(-1.0f, 0.0f));
        for (const auto& line : build_log_) {
            ImGui::BulletText("%s", line.c_str());
        }
        ImGui::Separator();
    }

    for (const auto& message : scene_.Messages()) {
        ImGui::BulletText("%s", message.c_str());
    }
    ImGui::End();
}

void EditorApp::DrawHistoryPanel() {
    if (!show_history_panel_) {
        return;
    }
    if (!ImGui::Begin("[HIS] History###HistoryWindow", &show_history_panel_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.92f, 0.66f, 0.24f, 1.0f});

    ImGui::TextWrapped("Recent actions are stored as level snapshots. Click any row to rollback to that point.");
    ImGui::Separator();

    if (history_entries_.empty()) {
        ImGui::TextDisabled("No history entries yet.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("HistoryTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int index = static_cast<int>(history_entries_.size()) - 1; index >= 0; --index) {
            const auto& entry = history_entries_[static_cast<std::size_t>(index)];
            const bool active = index == history_cursor_;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string step_label = std::to_string(index);
            if (active) {
                ImGui::TextUnformatted(("> " + step_label).c_str());
            } else {
                ImGui::TextUnformatted(step_label.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.timestamp.c_str());

            ImGui::TableNextColumn();
            const std::string label = entry.label + (active ? "  (current)" : "");
            if (ImGui::Selectable((label + "###history_" + std::to_string(index)).c_str(), active, ImGuiSelectableFlags_SpanAllColumns)) {
                RestoreHistoryIndex(index);
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void EditorApp::DrawSpriteEditorWindow() {
    if (!show_sprite_editor_) {
        return;
    }
    if (!ImGui::Begin(StableWindowLabel("SPR", Tr("editor.window.sprite_editor", "Sprite Editor"), "SpriteEditorWindow").c_str(), &show_sprite_editor_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.86f, 0.46f, 0.32f, 1.0f});
    if (browser_selection_kind_ != BrowserSelectionKind::Sprite || browser_selection_relative_.empty()) {
        ImGui::TextUnformatted(Tr("editor.sprite_editor.empty", "Select a sprite resource in the Content Browser to edit it.").c_str());
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(browser_selection_relative_.c_str());
    ImGui::Separator();
    resource_dirty_ |= EditString("Sprite Name", selected_sprite_.name);
    resource_dirty_ |= EditString("Default Animation", selected_sprite_.default_animation);
    if (ImGui::InputInt2("Canvas Size", &selected_sprite_.canvas_size.x)) {
        resource_dirty_ = true;
        selected_sprite_.canvas_size.x = std::max(selected_sprite_.canvas_size.x, 1);
        selected_sprite_.canvas_size.y = std::max(selected_sprite_.canvas_size.y, 1);
        LoadSpriteCanvas();
    }
    selected_sprite_.canvas_size.x = std::max(selected_sprite_.canvas_size.x, 1);
    selected_sprite_.canvas_size.y = std::max(selected_sprite_.canvas_size.y, 1);

    EditString("Image Import", sprite_import_path_, 1024);
    if (ImGui::Button("Import Image Into Frame") && !sprite_import_path_.empty() && std::filesystem::exists(sprite_import_path_)) {
        ImportImageIntoCurrentFrame(sprite_import_path_);
    }
    ImGui::SameLine();
    if (browser_selection_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(browser_selection_relative_))) {
        if (ImGui::Button("Use Selected Image")) {
            ImportImageIntoCurrentFrame(project_root_ / browser_selection_relative_);
        }
    }

    DrawSpriteEditor();

    if (ImGui::Button(Tr("editor.resource.save", "Save Resource").c_str())) {
        resource_dirty_ = true;
        SaveSelectedResource();
        status_message_ = "Saved sprite resource: " + browser_selection_relative_;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("editor.resource.rename_file", "Rename File From Resource Name").c_str())) {
        if (RenameSelectedResourceFile()) {
            status_message_ = "Renamed sprite resource file.";
        }
    }

    ImGui::End();
}

void EditorApp::DrawAnimationEditorWindow() {
    if (!show_animation_editor_) {
        return;
    }
    if (!ImGui::Begin("[ANIM] Animation Editor###AnimationEditorWindow", &show_animation_editor_)) {
        ImGui::End();
        return;
    }
    DecorateEditorWindow({0.18f, 0.76f, 0.72f, 1.0f});

    if (browser_selection_kind_ != BrowserSelectionKind::Animation || browser_selection_relative_.empty()) {
        ImGui::TextWrapped("Select an animation resource in the Content Browser to edit curves and deformation.");
        ImGui::End();
        return;
    }

    bool changed = false;
    changed |= EditString("Animation Name", selected_animation_.name);
    changed |= ImGui::InputFloat("Duration", &selected_animation_.duration);
    selected_animation_.duration = std::max(selected_animation_.duration, 0.05f);
    changed |= EditString("Preview Texture", selected_animation_.preview_texture);
    changed |= ImGui::Checkbox("Affect Position##AnimationEditor", &selected_animation_.affect_position);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Affect Opacity##AnimationEditor", &selected_animation_.affect_opacity);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Affect Scale##AnimationEditor", &selected_animation_.affect_scale);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Affect Rotation##AnimationEditor", &selected_animation_.affect_rotation);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Affect Skew##AnimationEditor", &selected_animation_.affect_skew);

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NOVAISO_TEXTURE_PATH")) {
            selected_animation_.preview_texture = static_cast<const char*>(payload->Data);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }

    static float path_zoom = 1.0f;
    ImGui::SliderFloat("Path Zoom", &path_zoom, 0.25f, 4.0f, "%.2f");
    const ImVec2 canvas_size{std::max(ImGui::GetContentRegionAvail().x, 320.0f), 280.0f};
    const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("AnimationPathCanvas", canvas_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 canvas_max{canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y};
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(canvas_min, canvas_max, IM_COL32(12, 18, 26, 250), 10.0f);
    for (float x = canvas_min.x; x <= canvas_max.x; x += 32.0f) {
        draw_list->AddLine({x, canvas_min.y}, {x, canvas_max.y}, IM_COL32(255, 255, 255, 10));
    }
    for (float y = canvas_min.y; y <= canvas_max.y; y += 32.0f) {
        draw_list->AddLine({canvas_min.x, y}, {canvas_max.x, y}, IM_COL32(255, 255, 255, 10));
    }
    const ImVec2 origin{canvas_min.x + canvas_size.x * 0.5f, canvas_min.y + canvas_size.y * 0.5f};
    draw_list->AddLine({canvas_min.x, origin.y}, {canvas_max.x, origin.y}, IM_COL32(96, 188, 255, 72), 1.0f);
    draw_list->AddLine({origin.x, canvas_min.y}, {origin.x, canvas_max.y}, IM_COL32(96, 188, 255, 72), 1.0f);

    auto to_canvas = [&](glm::vec2 point) {
        return ImVec2{origin.x + point.x * path_zoom, origin.y + point.y * path_zoom};
    };
    auto from_canvas = [&](ImVec2 point) {
        return glm::vec2{
            (point.x - origin.x) / std::max(path_zoom, 0.001f),
            (point.y - origin.y) / std::max(path_zoom, 0.001f)
        };
    };
    auto sample_path = [&](float t) {
        const auto& points = selected_animation_.path_points;
        if (points.empty()) {
            return glm::vec2{0.0f, 0.0f};
        }
        if (points.size() == 1) {
            return points.front();
        }
        const float scaled = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(points.size() - 1);
        const int segment = std::clamp(static_cast<int>(std::floor(scaled)), 0, static_cast<int>(points.size()) - 2);
        const float local = scaled - static_cast<float>(segment);
        const glm::vec2 p0 = points[static_cast<std::size_t>(std::max(segment - 1, 0))];
        const glm::vec2 p1 = points[static_cast<std::size_t>(segment)];
        const glm::vec2 p2 = points[static_cast<std::size_t>(std::min(segment + 1, static_cast<int>(points.size()) - 1))];
        const glm::vec2 p3 = points[static_cast<std::size_t>(std::min(segment + 2, static_cast<int>(points.size()) - 1))];
        const float local2 = local * local;
        const float local3 = local2 * local;
        return 0.5f * ((2.0f * p1) +
                       (-p0 + p2) * local +
                       (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * local2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * local3);
    };

    if (selected_animation_.path_points.empty()) {
        selected_animation_.path_points = {{0.0f, 0.0f}, {96.0f, 0.0f}};
        changed = true;
    }

    if (selected_animation_.path_points.size() >= 2) {
        for (int i = 1; i <= 48; ++i) {
            const ImVec2 a = to_canvas(sample_path(static_cast<float>(i - 1) / 48.0f));
            const ImVec2 b = to_canvas(sample_path(static_cast<float>(i) / 48.0f));
            draw_list->AddLine(a, b, IM_COL32(88, 236, 212, 255), 2.0f);
        }
    }

    for (int i = 0; i < static_cast<int>(selected_animation_.path_points.size()); ++i) {
        const ImVec2 point = to_canvas(selected_animation_.path_points[static_cast<std::size_t>(i)]);
        draw_list->AddCircleFilled(point, animation_path_drag_index_ == i ? 8.0f : 6.0f, IM_COL32(255, 214, 92, 255));
        draw_list->AddCircle(point, animation_path_drag_index_ == i ? 11.0f : 9.0f, IM_COL32(18, 22, 28, 220), 0, 2.0f);
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto nearest_path_point = [&]() {
        int best_index = -1;
        float best_distance = 144.0f;
        for (int i = 0; i < static_cast<int>(selected_animation_.path_points.size()); ++i) {
            const ImVec2 point = to_canvas(selected_animation_.path_points[static_cast<std::size_t>(i)]);
            const float dx = mouse.x - point.x;
            const float dy = mouse.y - point.y;
            const float distance = dx * dx + dy * dy;
            if (distance < best_distance) {
                best_distance = distance;
                best_index = i;
            }
        }
        return best_index;
    };

    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int hit = nearest_path_point();
        if (hit >= 0) {
            animation_path_drag_index_ = hit;
        } else {
            selected_animation_.path_points.push_back(from_canvas(mouse));
            animation_path_drag_index_ = static_cast<int>(selected_animation_.path_points.size() - 1);
            changed = true;
        }
    }
    if (animation_path_drag_index_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        selected_animation_.path_points[static_cast<std::size_t>(animation_path_drag_index_)] = from_canvas(mouse);
        changed = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        animation_path_drag_index_ = -1;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const int hit = nearest_path_point();
        if (hit >= 0 && selected_animation_.path_points.size() > 2) {
            selected_animation_.path_points.erase(selected_animation_.path_points.begin() + hit);
            changed = true;
        }
    }

    ImGui::TextDisabled("LMB: add or move point. RMB: remove point.");
    if (ImGui::Button("Reset Path")) {
        selected_animation_.path_points = {{0.0f, 0.0f}, {96.0f, 0.0f}};
        changed = true;
    }

    auto draw_float_keys = [&](const char* title, std::vector<assets::AnimationFloatKey>& keys, float default_value) {
        if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        if (ImGui::BeginTable(title, 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Preview");
            ImGui::TableSetupColumn("Remove");
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
                ImGui::PushID((std::string(title) + std::to_string(i)).c_str());
                auto& key = keys[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                changed |= ImGui::SliderFloat("##time", &key.time, 0.0f, 1.0f, "%.2f");
                ImGui::TableSetColumnIndex(1);
                changed |= ImGui::InputFloat("##value", &key.value);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f", key.value);
                ImGui::TableSetColumnIndex(3);
                if (ImGui::Button("X") && keys.size() > 2) {
                    keys.erase(keys.begin() + i);
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button((std::string("Add Key##") + title).c_str())) {
            keys.push_back({.time = 1.0f, .value = default_value});
            changed = true;
        }
        std::sort(keys.begin(), keys.end(), [](const auto& lhs, const auto& rhs) { return lhs.time < rhs.time; });
    };

    auto draw_vec2_keys = [&](const char* title, std::vector<assets::AnimationVec2Key>& keys, glm::vec2 default_value) {
        if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        if (ImGui::BeginTable(title, 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("X");
            ImGui::TableSetupColumn("Y");
            ImGui::TableSetupColumn("Preview");
            ImGui::TableSetupColumn("Remove");
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
                ImGui::PushID((std::string(title) + std::to_string(i)).c_str());
                auto& key = keys[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                changed |= ImGui::SliderFloat("##time", &key.time, 0.0f, 1.0f, "%.2f");
                ImGui::TableSetColumnIndex(1);
                changed |= ImGui::InputFloat("##x", &key.value.x);
                ImGui::TableSetColumnIndex(2);
                changed |= ImGui::InputFloat("##y", &key.value.y);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("(%.2f, %.2f)", key.value.x, key.value.y);
                ImGui::TableSetColumnIndex(4);
                if (ImGui::Button("X") && keys.size() > 2) {
                    keys.erase(keys.begin() + i);
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button((std::string("Add Key##") + title).c_str())) {
            keys.push_back({.time = 1.0f, .value = default_value});
            changed = true;
        }
        std::sort(keys.begin(), keys.end(), [](const auto& lhs, const auto& rhs) { return lhs.time < rhs.time; });
    };

    draw_float_keys("Opacity Keys", selected_animation_.opacity_keys, 1.0f);
    draw_vec2_keys("Scale Keys", selected_animation_.scale_keys, {1.0f, 1.0f});
    draw_float_keys("Rotation Keys", selected_animation_.rotation_keys, 0.0f);
    draw_vec2_keys("Skew Keys", selected_animation_.skew_keys, {0.0f, 0.0f});

    if (changed) {
        resource_dirty_ = true;
    }
    if (ImGui::Button("Save Animation Resource")) {
        resource_dirty_ = true;
        SaveSelectedResource();
        status_message_ = "Saved animation resource: " + browser_selection_relative_;
    }

    ImGui::End();
}

void EditorApp::DrawSpriteEditor() {
    EnsureSpriteStructure(selected_sprite_, MakeUniqueRelativePath("assets/images/generated", selected_sprite_.name + "_frame", ".tga"));

    if (selected_sprite_.animations.empty()) {
        return;
    }

    sprite_animation_index_ = std::clamp(sprite_animation_index_, 0, static_cast<int>(selected_sprite_.animations.size()) - 1);
    if (selected_sprite_.animations[sprite_animation_index_].frames.empty()) {
        selected_sprite_.animations[sprite_animation_index_].frames.push_back({
            MakeUniqueRelativePath("assets/images/generated", selected_sprite_.name + "_frame", ".tga"),
            0.12f
        });
    }
    sprite_frame_index_ = std::clamp(sprite_frame_index_, 0, static_cast<int>(selected_sprite_.animations[sprite_animation_index_].frames.size()) - 1);

    ImGui::Separator();
    if (ImGui::BeginCombo("Animation", selected_sprite_.animations[sprite_animation_index_].name.c_str())) {
        for (int i = 0; i < static_cast<int>(selected_sprite_.animations.size()); ++i) {
            const bool selected = sprite_animation_index_ == i;
            if (ImGui::Selectable(selected_sprite_.animations[i].name.c_str(), selected)) {
                sprite_animation_index_ = i;
                sprite_frame_index_ = 0;
                LoadSpriteCanvas();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Animation")) {
        selected_sprite_.animations.push_back({
            .name = "anim_" + std::to_string(selected_sprite_.animations.size()),
            .loop = true,
            .frames = {{MakeUniqueRelativePath("assets/images/generated", selected_sprite_.name + "_anim_frame", ".tga"), 0.12f}}
        });
        sprite_animation_index_ = static_cast<int>(selected_sprite_.animations.size() - 1);
        sprite_frame_index_ = 0;
        resource_dirty_ = true;
        LoadSpriteCanvas();
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove Animation") && selected_sprite_.animations.size() > 1) {
        selected_sprite_.animations.erase(selected_sprite_.animations.begin() + sprite_animation_index_);
        sprite_animation_index_ = 0;
        sprite_frame_index_ = 0;
        resource_dirty_ = true;
        LoadSpriteCanvas();
    }

    auto& animation = selected_sprite_.animations[sprite_animation_index_];
    resource_dirty_ |= EditString("Animation Name", animation.name);
    resource_dirty_ |= ImGui::Checkbox("Loop", &animation.loop);

    if (ImGui::BeginCombo("Frame", ("Frame " + std::to_string(sprite_frame_index_)).c_str())) {
        for (int i = 0; i < static_cast<int>(animation.frames.size()); ++i) {
            const bool selected = sprite_frame_index_ == i;
            const std::string label = "Frame " + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), selected)) {
                sprite_frame_index_ = i;
                LoadSpriteCanvas();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Frame")) {
        animation.frames.push_back({
            MakeUniqueRelativePath("assets/images/generated", selected_sprite_.name + "_" + animation.name + "_frame", ".tga"),
            0.12f
        });
        sprite_frame_index_ = static_cast<int>(animation.frames.size() - 1);
        resource_dirty_ = true;
        LoadSpriteCanvas();
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove Frame") && animation.frames.size() > 1) {
        animation.frames.erase(animation.frames.begin() + sprite_frame_index_);
        sprite_frame_index_ = 0;
        resource_dirty_ = true;
        LoadSpriteCanvas();
    }

    auto& frame = animation.frames[sprite_frame_index_];
    resource_dirty_ |= EditString("Frame Texture", frame.texture);
    resource_dirty_ |= ImGui::InputFloat("Frame Duration", &frame.duration);
    frame.duration = std::max(frame.duration, 0.01f);
    if (ImGui::Button("Brush")) {
        sprite_tool_ = SpriteTool::Brush;
    }
    ImGui::SameLine();
    if (ImGui::Button("Erase")) {
        sprite_tool_ = SpriteTool::Erase;
    }
    ImGui::SameLine();
    if (ImGui::Button("Picker")) {
        sprite_tool_ = SpriteTool::Picker;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(
        sprite_tool_ == SpriteTool::Brush ? "Tool: Brush" :
        sprite_tool_ == SpriteTool::Erase ? "Tool: Erase" :
        "Tool: Picker"
    );
    if (ImGui::Button("Reload Frame Canvas")) {
        LoadSpriteCanvas();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Frame Canvas")) {
        SaveSpriteCanvas();
    }
    ImGui::SameLine();
    if (frame.texture.empty() || std::filesystem::path(frame.texture).extension() != ".tga") {
        if (ImGui::Button("Convert To Editable TGA")) {
            ConvertCurrentFrameToEditable();
        }
    }

    ImGui::ColorEdit4("Paint Color", &paint_color_.x);

    if (pixel_canvas_.pixels.empty()) {
        LoadSpriteCanvas();
    }

    const float available_width = ImGui::GetContentRegionAvail().x;
    const float max_editor_height = 420.0f;
    const float cell = std::max(
        8.0f,
        std::floor(std::min(
            available_width / static_cast<float>(std::max(pixel_canvas_.width, 1)),
            max_editor_height / static_cast<float>(std::max(pixel_canvas_.height, 1))
        ))
    );
    const ImVec2 editor_size{
        pixel_canvas_.width * cell,
        pixel_canvas_.height * cell
    };

    ImGui::InvisibleButton("PixelCanvas", editor_size);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetItemRectMin();
    const ImVec2 limit = ImGui::GetItemRectMax();
    draw_list->AddRectFilled(origin, limit, IM_COL32(22, 22, 26, 255));

    for (int y = 0; y < pixel_canvas_.height; ++y) {
        for (int x = 0; x < pixel_canvas_.width; ++x) {
            const std::size_t index = static_cast<std::size_t>((y * pixel_canvas_.width + x) * 4);
            const ImU32 color = IM_COL32(
                pixel_canvas_.pixels[index + 0],
                pixel_canvas_.pixels[index + 1],
                pixel_canvas_.pixels[index + 2],
                pixel_canvas_.pixels[index + 3]
            );
            const ImVec2 cell_min{origin.x + x * cell, origin.y + y * cell};
            const ImVec2 cell_max{cell_min.x + cell, cell_min.y + cell};
            draw_list->AddRectFilled(cell_min, cell_max, color);
            draw_list->AddRect(cell_min, cell_max, IM_COL32(55, 55, 65, 255));
        }
    }

    if (pixel_canvas_.texture_relative.empty()) {
        ImGui::TextUnformatted("Painting is disabled until the frame uses an editable .tga canvas.");
    }

    if (!pixel_canvas_.texture_relative.empty() && ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const int x = static_cast<int>((mouse.x - origin.x) / cell);
        const int y = static_cast<int>((mouse.y - origin.y) / cell);
        if (x >= 0 && y >= 0 && x < pixel_canvas_.width && y < pixel_canvas_.height) {
            const std::size_t index = static_cast<std::size_t>((y * pixel_canvas_.width + x) * 4);
            bool changed = false;
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (sprite_tool_ == SpriteTool::Picker) {
                    paint_color_ = {
                        pixel_canvas_.pixels[index + 0] / 255.0f,
                        pixel_canvas_.pixels[index + 1] / 255.0f,
                        pixel_canvas_.pixels[index + 2] / 255.0f,
                        pixel_canvas_.pixels[index + 3] / 255.0f
                    };
                } else if (sprite_tool_ == SpriteTool::Brush) {
                    const glm::u8vec4 color = ToByteColor(paint_color_);
                    changed =
                        pixel_canvas_.pixels[index + 0] != color.r ||
                        pixel_canvas_.pixels[index + 1] != color.g ||
                        pixel_canvas_.pixels[index + 2] != color.b ||
                        pixel_canvas_.pixels[index + 3] != color.a;
                    pixel_canvas_.pixels[index + 0] = color.r;
                    pixel_canvas_.pixels[index + 1] = color.g;
                    pixel_canvas_.pixels[index + 2] = color.b;
                    pixel_canvas_.pixels[index + 3] = color.a;
                } else if (sprite_tool_ == SpriteTool::Erase) {
                    changed =
                        pixel_canvas_.pixels[index + 0] != 0 ||
                        pixel_canvas_.pixels[index + 1] != 0 ||
                        pixel_canvas_.pixels[index + 2] != 0 ||
                        pixel_canvas_.pixels[index + 3] != 0;
                    pixel_canvas_.pixels[index + 0] = 0;
                    pixel_canvas_.pixels[index + 1] = 0;
                    pixel_canvas_.pixels[index + 2] = 0;
                    pixel_canvas_.pixels[index + 3] = 0;
                }
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                changed =
                    pixel_canvas_.pixels[index + 0] != 0 ||
                    pixel_canvas_.pixels[index + 1] != 0 ||
                    pixel_canvas_.pixels[index + 2] != 0 ||
                    pixel_canvas_.pixels[index + 3] != 0;
                pixel_canvas_.pixels[index + 0] = 0;
                pixel_canvas_.pixels[index + 1] = 0;
                pixel_canvas_.pixels[index + 2] = 0;
                pixel_canvas_.pixels[index + 3] = 0;
            }

            if (changed) {
                pixel_canvas_.dirty = true;
                resource_dirty_ = true;
                SaveSpriteCanvas();
            }
        }
    }

    renderer::Texture2D& preview_texture = asset_manager_.LoadTexture(frame.texture);
    const float preview_scale = 4.0f;
    ImGui::Separator();
    ImGui::TextUnformatted("Frame Preview");
    ImGui::Image(
        reinterpret_cast<void*>(static_cast<intptr_t>(preview_texture.Id())),
        ImVec2(preview_texture.Size().x * preview_scale, preview_texture.Size().y * preview_scale),
        ImVec2(0, 1),
        ImVec2(1, 0)
    );
}

assets::SpriteFrame* EditorApp::CurrentSpriteFrame() {
    if (selected_sprite_.animations.empty()) {
        return nullptr;
    }
    sprite_animation_index_ = std::clamp(sprite_animation_index_, 0, static_cast<int>(selected_sprite_.animations.size()) - 1);
    auto& animation = selected_sprite_.animations[sprite_animation_index_];
    if (animation.frames.empty()) {
        return nullptr;
    }
    sprite_frame_index_ = std::clamp(sprite_frame_index_, 0, static_cast<int>(animation.frames.size()) - 1);
    return &animation.frames[sprite_frame_index_];
}

const assets::SpriteFrame* EditorApp::CurrentSpriteFrame() const {
    if (selected_sprite_.animations.empty()) {
        return nullptr;
    }
    const int animation_index = std::clamp(sprite_animation_index_, 0, static_cast<int>(selected_sprite_.animations.size()) - 1);
    const auto& animation = selected_sprite_.animations[animation_index];
    if (animation.frames.empty()) {
        return nullptr;
    }
    const int frame_index = std::clamp(sprite_frame_index_, 0, static_cast<int>(animation.frames.size()) - 1);
    return &animation.frames[frame_index];
}

void EditorApp::ConvertCurrentFrameToEditable() {
    assets::SpriteFrame* frame = CurrentSpriteFrame();
    if (frame == nullptr) {
        return;
    }

    int width = selected_sprite_.canvas_size.x;
    int height = selected_sprite_.canvas_size.y;
    std::vector<std::uint8_t> pixels = MakeBlankPixels(width, height);

    const std::filesystem::path source_path = project_root_ / frame->texture;
    if (std::filesystem::exists(source_path)) {
        int channels = 0;
        stbi_uc* loaded = stbi_load(source_path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (loaded != nullptr) {
            pixels.assign(loaded, loaded + static_cast<std::ptrdiff_t>(width * height * 4));
            stbi_image_free(loaded);
            selected_sprite_.canvas_size = {std::max(width, 1), std::max(height, 1)};
        }
    }

    frame->texture = MakeUniqueRelativePath("assets/images/generated", selected_sprite_.name + "_editable", ".tga");
    WriteTga32(project_root_ / frame->texture, selected_sprite_.canvas_size.x, selected_sprite_.canvas_size.y, pixels);
    resource_dirty_ = true;
    LoadSpriteCanvas();
}

void EditorApp::ImportImageIntoCurrentFrame(const std::filesystem::path& source) {
    assets::SpriteFrame* frame = CurrentSpriteFrame();
    if (frame == nullptr || !std::filesystem::exists(source)) {
        return;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(source.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        status_message_ = "Failed to load image for sprite frame.";
        return;
    }

    selected_sprite_.canvas_size = {std::max(width, 1), std::max(height, 1)};
    if (frame->texture.empty() || std::filesystem::path(frame->texture).extension() != ".tga") {
        frame->texture = MakeUniqueRelativePath("assets/images/generated", selected_sprite_.name + "_frame", ".tga");
    }

    std::vector<std::uint8_t> buffer(pixels, pixels + static_cast<std::ptrdiff_t>(width * height * 4));
    stbi_image_free(pixels);
    WriteTga32(project_root_ / frame->texture, width, height, buffer);
    resource_dirty_ = true;
    LoadSpriteCanvas();
}

void EditorApp::SelectBrowserResource(BrowserSelectionKind kind, const std::string& relative_path) {
    SaveSelectedResource();
    SaveCurrentScript();
    browser_selection_kind_ = kind;
    browser_selection_relative_ = relative_path;

    if (kind == BrowserSelectionKind::File && std::filesystem::path(relative_path).extension() == ".py") {
        current_script_relative_ = relative_path;
    }

    if ((kind == BrowserSelectionKind::File || kind == BrowserSelectionKind::Sprite ||
         kind == BrowserSelectionKind::Object || kind == BrowserSelectionKind::Trigger ||
         kind == BrowserSelectionKind::Animation || kind == BrowserSelectionKind::Particle) &&
        IsEditableTextFile(relative_path)) {
        current_code_relative_ = relative_path;
        LoadCurrentScript();
    }

    LoadSelectedResource();
}

void EditorApp::CreateSpriteResource() {
    const std::string resource_path = MakeUniqueRelativePath("resources/sprites", "sprite", ".nsprite");
    const std::string frame_texture = MakeUniqueRelativePath("assets/images/generated", "sprite_frame", ".tga");

    assets::SpriteAsset sprite;
    sprite.name = std::filesystem::path(resource_path).stem().string();
    sprite.canvas_size = {16, 16};
    sprite.default_animation = "idle";
    sprite.animations = {{
        .name = "idle",
        .loop = true,
        .frames = {{frame_texture, 0.12f}}
    }};

    WriteTga32(project_root_ / frame_texture, sprite.canvas_size.x, sprite.canvas_size.y, MakeBlankPixels(sprite.canvas_size.x, sprite.canvas_size.y));
    assets::SaveSpriteAsset(project_root_ / resource_path, sprite);
    EnsureProjectListContains(project_.sprite_assets, resource_path);
    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    SelectBrowserResource(BrowserSelectionKind::Sprite, resource_path);
    SyncSceneResources();
    status_message_ = "Created sprite resource: " + resource_path;
}

void EditorApp::CreateObjectResource() {
    const std::string resource_path = MakeUniqueRelativePath("resources/objects", "object", ".nobject");

    assets::ObjectAsset object;
    object.name = std::filesystem::path(resource_path).stem().string();
    if (browser_selection_kind_ == BrowserSelectionKind::Sprite) {
        object.sprite = browser_selection_relative_;
    }
    assets::SaveObjectAsset(project_root_ / resource_path, object);
    EnsureProjectListContains(project_.object_assets, resource_path);
    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    SelectBrowserResource(BrowserSelectionKind::Object, resource_path);
    SyncSceneResources();
    status_message_ = "Created object resource: " + resource_path;
}

void EditorApp::CreateTriggerResource() {
    const std::string resource_path = MakeUniqueRelativePath("resources/triggers", "trigger", ".ntrigger");

    assets::TriggerAsset trigger;
    trigger.name = std::filesystem::path(resource_path).stem().string();
    assets::SaveTriggerAsset(project_root_ / resource_path, trigger);
    EnsureProjectListContains(project_.trigger_assets, resource_path);
    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    SelectBrowserResource(BrowserSelectionKind::Trigger, resource_path);
    SyncSceneResources();
    status_message_ = "Created trigger resource: " + resource_path;
}

void EditorApp::CreateAnimationResource() {
    const std::string resource_path = MakeUniqueRelativePath("resources/animations", "animation", ".nanim");

    assets::ObjectAnimationAsset animation;
    animation.name = std::filesystem::path(resource_path).stem().string();
    if (browser_selection_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(browser_selection_relative_))) {
        animation.preview_texture = browser_selection_relative_;
    }
    assets::SaveObjectAnimationAsset(project_root_ / resource_path, animation);
    EnsureProjectListContains(project_.animation_assets, resource_path);
    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    SelectBrowserResource(BrowserSelectionKind::Animation, resource_path);
    SyncSceneResources();
    status_message_ = "Created animation resource: " + resource_path;
}

void EditorApp::CreateParticleResource() {
    const std::string resource_path = MakeUniqueRelativePath("resources/particles", "particle", ".nparticle");

    assets::ParticleEffectAsset particle;
    particle.name = std::filesystem::path(resource_path).stem().string();
    particle.preset = "fire_loop";
    particle.texture = "assets/images/generated/particle_fire.tga";
    if (browser_selection_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(browser_selection_relative_))) {
        particle.texture = browser_selection_relative_;
        particle.preset = "custom";
    }
    ApplyParticlePresetDefaults(particle);
    assets::SaveParticleEffectAsset(project_root_ / resource_path, particle);
    EnsureProjectListContains(project_.particle_assets, resource_path);
    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    SelectBrowserResource(BrowserSelectionKind::Particle, resource_path);
    SyncSceneResources();
    status_message_ = "Created particle resource: " + resource_path;
}

void EditorApp::CreateObjectFromTexture(const std::string& texture_path) {
    CreateSpriteFromTexture(texture_path);
    const std::string sprite_path = browser_selection_relative_;
    const std::string object_path = MakeUniqueRelativePath("resources/objects", std::filesystem::path(texture_path).stem().string(), ".nobject");

    assets::ObjectAsset object;
    object.name = std::filesystem::path(object_path).stem().string();
    object.sprite = sprite_path;
    const glm::ivec2 size = asset_manager_.LoadTexture(texture_path).Size();
    object.size = {
        static_cast<float>(std::max(size.x, 1)),
        static_cast<float>(std::max(size.y, 1))
    };
    object.collider_size = object.size;

    assets::SaveObjectAsset(project_root_ / object_path, object);
    EnsureProjectListContains(project_.object_assets, object_path);
    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    SelectBrowserResource(BrowserSelectionKind::Object, object_path);
    SyncSceneResources();
    status_message_ = "Created object resource from image: " + object_path;
}

void EditorApp::CreateSpriteFromTexture(const std::string& texture_path) {
    const std::string sprite_path = MakeUniqueRelativePath("resources/sprites", std::filesystem::path(texture_path).stem().string(), ".nsprite");
    const glm::ivec2 size = asset_manager_.LoadTexture(texture_path).Size();

    assets::SpriteAsset sprite;
    sprite.name = std::filesystem::path(sprite_path).stem().string();
    sprite.canvas_size = {
        std::max(size.x, 16),
        std::max(size.y, 16)
    };

    const bool gif_created = PopulateSpriteFromDecodedGif(
        project_root_,
        [&](const std::filesystem::path& folder, const std::string& stem, const std::string& extension) {
            return MakeUniqueRelativePath(folder, stem, extension);
        },
        sprite.name,
        texture_path,
        sprite);

    if (!gif_created) {
        sprite.animations = {{
            .name = "idle",
            .loop = true,
            .frames = {{texture_path, 0.12f}}
        }};
    }

    assets::SaveSpriteAsset(project_root_ / sprite_path, sprite);
    EnsureProjectListContains(project_.sprite_assets, sprite_path);
    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    SelectBrowserResource(BrowserSelectionKind::Sprite, sprite_path);
    SyncSceneResources();
    status_message_ = gif_created
        ? "Created animated sprite resource from GIF: " + sprite_path
        : "Created sprite resource from image: " + sprite_path;
}

void EditorApp::ExportGamePackage() {
    SaveAll();
    build_in_progress_ = true;
    build_progress_ = 0.0f;
    build_log_.clear();
    scene_.ClearMessages();

    auto log_build = [&](float progress, std::string message) {
        build_progress_ = std::clamp(progress, 0.0f, 1.0f);
        build_log_.push_back(message);
        scene_.Log("[Build] " + message);
        status_message_ = message;
    };

    const std::filesystem::path runtime_source = ExecutableDirectory() / "NovaIsoRuntime.exe";
    if (!std::filesystem::exists(runtime_source)) {
        build_in_progress_ = false;
        status_message_ = "Runtime executable not found beside the editor.";
        return;
    }

    const std::string safe_name = SanitizeName(project_.name);
    const std::filesystem::path export_root = project_root_ / project_.export_directory / safe_name;
    log_build(0.06f, "Preparing export root: " + export_root.string());
    std::filesystem::remove_all(export_root);
    core::FileIO::EnsureDirectory(export_root);
    const std::filesystem::path staging_root = export_root / ".pack_staging";
    core::FileIO::EnsureDirectory(staging_root);

    const std::filesystem::path packaged_executable = export_root / (safe_name + ".exe");
    std::filesystem::copy_file(runtime_source, packaged_executable, std::filesystem::copy_options::overwrite_existing);
    log_build(0.14f, "Copied runtime executable to: " + packaged_executable.string());
    for (const auto& entry : std::filesystem::directory_iterator(ExecutableDirectory())) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto extension = entry.path().extension().string();
        if (extension == ".dll" || extension == ".pyd") {
            std::filesystem::copy_file(entry.path(), export_root / entry.path().filename(), std::filesystem::copy_options::overwrite_existing);
            build_log_.push_back("Copied dependency: " + entry.path().filename().string());
        }
    }
    log_build(0.20f, "Copied runtime dependencies beside executable.");

    if (project_.encrypt_archive && project_.archive_password.empty()) {
        build_in_progress_ = false;
        std::filesystem::remove_all(export_root);
        status_message_ = "Archive encryption is enabled, but the password is empty.";
        scene_.Log("[Build] Archive encryption is enabled, but the password is empty.");
        return;
    }

    auto stage_copy_file = [&](const std::filesystem::path& source, const std::filesystem::path& relative_target) {
        if (!std::filesystem::is_regular_file(source)) {
            return false;
        }
        const std::filesystem::path destination = staging_root / relative_target;
        core::FileIO::EnsureDirectory(destination.parent_path());
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
        return true;
    };
    auto stage_copy_directory = [&](const std::filesystem::path& source_root, const std::filesystem::path& relative_root) {
        if (!std::filesystem::exists(source_root)) {
            return;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root)) {
            if (entry.is_regular_file()) {
                stage_copy_file(entry.path(), relative_root / std::filesystem::relative(entry.path(), source_root));
            }
        }
    };
    auto remap_into_pack = [&](const std::string& configured_path, const std::string& fallback_relative, const std::string& staged_name) -> std::string {
        std::filesystem::path source_path;
        if (!configured_path.empty()) {
            source_path = std::filesystem::path(configured_path);
            if (!source_path.is_absolute()) {
                source_path = project_root_ / source_path;
            }
        } else if (!fallback_relative.empty() && std::filesystem::exists(project_root_ / fallback_relative)) {
            source_path = project_root_ / fallback_relative;
        } else if (!fallback_relative.empty() && std::filesystem::exists(ExecutableDirectory() / fallback_relative)) {
            source_path = ExecutableDirectory() / fallback_relative;
        }

        if (source_path.empty() || !std::filesystem::exists(source_path)) {
            return {};
        }

        std::filesystem::path relative_target;
        if (!configured_path.empty() && !std::filesystem::path(configured_path).is_absolute()) {
            relative_target = std::filesystem::path(configured_path).lexically_normal();
        } else if (!fallback_relative.empty() && std::filesystem::exists(project_root_ / fallback_relative)) {
            relative_target = std::filesystem::path(fallback_relative).lexically_normal();
        } else {
            relative_target = std::filesystem::path("_embedded") / (staged_name + source_path.extension().string());
        }

        if (!stage_copy_file(source_path, relative_target)) {
            return {};
        }
        return relative_target.generic_string();
    };

    log_build(0.28f, "Staging assets, scripts, UI, levels, resources, localization and shaders.");
    stage_copy_directory(project_root_ / "assets", "assets");
    stage_copy_directory(project_root_ / "scripts", "scripts");
    stage_copy_directory(project_root_ / "ui", "ui");
    stage_copy_directory(project_root_ / "levels", "levels");
    stage_copy_directory(project_root_ / "resources", "resources");
    stage_copy_directory(project_root_ / "localization", "localization");
    stage_copy_directory(ActiveShaderRoot(), "shaders");

    assets::ProjectData packaged_project = project_;
    packaged_project.icon = remap_into_pack(project_.icon, "", "project_icon");
    packaged_project.preview_image = remap_into_pack(project_.preview_image, "", "project_preview");
    packaged_project.splash_image = remap_into_pack(project_.splash_image, "logo.png", "project_splash");
    packaged_project.encrypt_archive = false;
    packaged_project.archive_password.clear();
    assets::SaveProject(staging_root / "project.json", packaged_project);
    log_build(0.44f, "Saved packaged project.json into staging directory.");

    std::vector<std::filesystem::path> pack_entries;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(staging_root)) {
        if (entry.is_regular_file()) {
            pack_entries.push_back(std::filesystem::relative(entry.path(), staging_root).lexically_normal());
        }
    }
    std::sort(pack_entries.begin(), pack_entries.end());
    pack_entries.erase(std::unique(pack_entries.begin(), pack_entries.end()), pack_entries.end());
    const std::filesystem::path pack_path = export_root / "content.npak";
    log_build(0.62f, std::string(project_.encrypt_archive ? "Building encrypted pack: " : "Building pack: ") + pack_path.string());
    if (!core::FileIO::CreatePack(pack_path, staging_root, pack_entries, project_.encrypt_archive ? std::string_view(project_.archive_password) : std::string_view{})) {
        build_in_progress_ = false;
        status_message_ = "Failed to build content.npak.";
        std::filesystem::remove_all(export_root);
        return;
    }
    log_build(0.76f, "Packed " + std::to_string(pack_entries.size()) + " files into content.npak.");

    const std::filesystem::path splash_source = packaged_project.splash_image.empty() ? std::filesystem::path{} : (staging_root / packaged_project.splash_image);
    if (project_.splash_enabled && !splash_source.empty() && std::filesystem::exists(splash_source)) {
        const auto splash_bytes = core::FileIO::ReadBinary(splash_source);
        core::EmbedBinaryResource(packaged_executable, L"NOVAISO_BIN", L"NOVAISO_SPLASH", splash_bytes);
        build_log_.push_back("Embedded splash resource into executable.");
    }

    if (!packaged_project.icon.empty()) {
        const std::filesystem::path icon_source = staging_root / packaged_project.icon;
        if (std::filesystem::exists(icon_source)) {
            const auto icon_bytes = core::FileIO::ReadBinary(icon_source);
            core::EmbedPngIconResource(packaged_executable, icon_bytes);
            build_log_.push_back("Embedded icon resource into executable.");
        }
    }
    if (project_.encrypt_archive) {
        const std::vector<std::uint8_t> password_bytes(project_.archive_password.begin(), project_.archive_password.end());
        core::EmbedBinaryResource(packaged_executable, L"NOVAISO_BIN", L"NOVAISO_PACK_PASSWORD", password_bytes);
        build_log_.push_back("Embedded pack decryption password into packaged runtime.");
    }

    std::filesystem::remove_all(staging_root);
    build_in_progress_ = false;
    build_progress_ = 1.0f;
    status_message_ = "Build package exported to: " + export_root.string() + " (content.npak)";
    scene_.Log("[Build] Finished export to: " + export_root.string());
}

void EditorApp::PlaceTextureEntity(const std::string& texture_path, glm::vec2 world_position) {
    pending_history_label_ = "Place image";
    if (snap_to_grid_) {
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        world_position.x = std::round(world_position.x / grid_w) * grid_w;
        world_position.y = std::round(world_position.y / grid_h) * grid_h;
    }
    renderer::Texture2D& texture = asset_manager_.LoadTexture(texture_path);
    const glm::vec2 size = {
        static_cast<float>(std::max(texture.Size().x, 16)),
        static_cast<float>(std::max(texture.Size().y, 16))
    };

    scene_.EditableLevel().entities.push_back({
        .id = "entity_" + std::to_string(scene_.EditableLevel().entities.size()),
        .archetype = std::filesystem::path(texture_path).stem().string(),
        .position = world_position - size * 0.5f,
        .size = size,
        .texture = texture_path,
        .visible = true,
        .collider_size = size
    });
    SetHierarchySelectionSingle(SelectionKind::Entity, static_cast<int>(scene_.EditableLevel().entities.size() - 1));
    scene_.ResetSimulation(false);
}

void EditorApp::PlaceObjectEntity(const std::string& object_path, glm::vec2 world_position) {
    pending_history_label_ = "Place object";
    if (snap_to_grid_) {
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        world_position.x = std::round(world_position.x / grid_w) * grid_w;
        world_position.y = std::round(world_position.y / grid_h) * grid_h;
    }
    const assets::ObjectAsset object = assets::LoadObjectAsset(project_root_ / object_path);
    assets::EntityDefinition entity;
    entity.id = "entity_" + std::to_string(scene_.EditableLevel().entities.size());
    ApplyObjectAssetToEntity(entity, object, object_path);
    entity.position = world_position - object.size * 0.5f;
    entity.visible = true;
    scene_.EditableLevel().entities.push_back(std::move(entity));
    SetHierarchySelectionSingle(SelectionKind::Entity, static_cast<int>(scene_.EditableLevel().entities.size() - 1));
    scene_.ResetSimulation(false);
}

void EditorApp::PlaceTriggerZone(const std::string& trigger_path, glm::vec2 world_position) {
    pending_history_label_ = "Place trigger";
    if (snap_to_grid_) {
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        world_position.x = std::round(world_position.x / grid_w) * grid_w;
        world_position.y = std::round(world_position.y / grid_h) * grid_h;
    }
    const assets::TriggerAsset trigger_asset = assets::LoadTriggerAsset(project_root_ / trigger_path);
    scene_.EditableLevel().triggers.push_back({
        .id = "trigger_" + std::to_string(scene_.EditableLevel().triggers.size()),
        .name = trigger_asset.name + "_" + std::to_string(scene_.EditableLevel().triggers.size()),
        .asset = trigger_path,
        .position = world_position - trigger_asset.default_size * 0.5f,
        .size = trigger_asset.default_size,
        .color = trigger_asset.color,
        .once = trigger_asset.once,
        .enabled = trigger_asset.enabled,
        .conditions = trigger_asset.conditions,
        .actions = trigger_asset.actions
    });
    SetHierarchySelectionSingle(SelectionKind::Trigger, static_cast<int>(scene_.EditableLevel().triggers.size() - 1));
    scene_.ResetSimulation(false);
}

void EditorApp::PlaceLight(glm::vec2 world_position, std::string_view light_type) {
    const bool flashlight = light_type == "flashlight";
    pending_history_label_ = flashlight ? "Place flashlight" : "Place light";
    if (snap_to_grid_ && !(GetInput().IsKeyDown(SDL_SCANCODE_LCTRL) || GetInput().IsKeyDown(SDL_SCANCODE_RCTRL))) {
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        world_position.x = std::round(world_position.x / grid_w) * grid_w;
        world_position.y = std::round(world_position.y / grid_h) * grid_h;
    }
    scene_.EditableLevel().lights.push_back({
        .id = std::string(flashlight ? "flashlight_" : "light_") + std::to_string(scene_.EditableLevel().lights.size()),
        .name = std::string(flashlight ? "flashlight_" : "light_") + std::to_string(scene_.EditableLevel().lights.size()),
        .type = flashlight ? "flashlight" : "point",
        .position = world_position,
        .radius = flashlight ? 220.0f : 280.0f,
        .length = flashlight ? 620.0f : 520.0f,
        .source_radius = flashlight ? 10.0f : 26.0f,
        .scatter = flashlight ? 1.45f : 1.0f,
        .direction_degrees = flashlight ? -25.0f : -35.0f,
        .cone_angle = flashlight ? 38.0f : 42.0f,
        .cone_softness = flashlight ? 0.22f : 0.28f,
    });
    SetHierarchySelectionSingle(SelectionKind::Light, static_cast<int>(scene_.EditableLevel().lights.size() - 1));
    scene_.ResetSimulation(false);
}

void EditorApp::PlaceAudioSource(const std::string& audio_path, glm::vec2 world_position) {
    pending_history_label_ = "Place audio source";
    if (snap_to_grid_ && !(GetInput().IsKeyDown(SDL_SCANCODE_LCTRL) || GetInput().IsKeyDown(SDL_SCANCODE_RCTRL))) {
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        world_position.x = std::round(world_position.x / grid_w) * grid_w;
        world_position.y = std::round(world_position.y / grid_h) * grid_h;
    }
    auto& sources = scene_.EditableLevel().audio_sources;
    sources.push_back({
        .id = "audio_source_" + std::to_string(sources.size()),
        .name = std::filesystem::path(audio_path).stem().string(),
        .audio = audio_path,
        .position = world_position
    });
    SetHierarchySelectionSingle(SelectionKind::AudioSource, static_cast<int>(sources.size() - 1));
    scene_.ResetSimulation(false);
}

void EditorApp::PlaceAudioPak(glm::vec2 world_position) {
    pending_history_label_ = "Place audio pak";
    if (snap_to_grid_ && !(GetInput().IsKeyDown(SDL_SCANCODE_LCTRL) || GetInput().IsKeyDown(SDL_SCANCODE_RCTRL))) {
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        world_position.x = std::round(world_position.x / grid_w) * grid_w;
        world_position.y = std::round(world_position.y / grid_h) * grid_h;
    }
    auto& paks = scene_.EditableLevel().audio_paks;
    paks.push_back({
        .id = "audio_pak_" + std::to_string(paks.size()),
        .name = "audio_pak_" + std::to_string(paks.size()),
        .position = world_position
    });
    SetHierarchySelectionSingle(SelectionKind::AudioPak, static_cast<int>(paks.size() - 1));
    scene_.ResetSimulation(false);
}

void EditorApp::PlaceSceneAnimation(const std::string& animation_path, glm::vec2) {
    pending_history_label_ = "Place scene animation";
    auto& animations = scene_.EditableLevel().animations;
    const std::string fallback_target =
        selection_kind_ == SelectionKind::Entity &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())
            ? scene_.EditableLevel().entities[static_cast<std::size_t>(selection_index_)].id
            : std::string("player");
    animations.push_back({
        .id = "scene_animation_" + std::to_string(animations.size()),
        .name = std::filesystem::path(animation_path).stem().string(),
        .asset = animation_path,
        .target_entity = fallback_target,
        .enabled = true,
        .play_on_start = true,
        .loop = false,
        .speed = 1.0f
    });
    SetHierarchySelectionSingle(SelectionKind::SceneAnimation, static_cast<int>(animations.size() - 1));
    scene_.ResetSimulation(false);
}

void EditorApp::PlaceParticleEmitter(const std::string& particle_path, glm::vec2 world_position) {
    pending_history_label_ = "Place particle emitter";
    if (snap_to_grid_ && !(GetInput().IsKeyDown(SDL_SCANCODE_LCTRL) || GetInput().IsKeyDown(SDL_SCANCODE_RCTRL))) {
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        world_position.x = std::round(world_position.x / grid_w) * grid_w;
        world_position.y = std::round(world_position.y / grid_h) * grid_h;
    }

    const assets::ParticleEffectAsset particle = assets::LoadParticleEffectAsset(project_root_ / particle_path);
    auto& entities = scene_.EditableLevel().entities;
    assets::EntityDefinition entity;
    entity.id = "particle_" + std::to_string(entities.size());
    entity.archetype = "particle_emitter";
    entity.position = world_position - glm::vec2(24.0f, 24.0f);
    entity.size = {48.0f, 48.0f};
    entity.visible = false;
    entity.dynamic = false;
    entity.collidable = false;
    entity.collider_size = entity.size;
    entity.properties = {
        {"particle_emitter", true},
        {"particle_asset", particle_path},
        {"emitter_enabled", true},
        {"particle_autoplay", particle.loop},
        {"burst_on_start", !particle.loop}
    };
    entities.push_back(std::move(entity));
    SetHierarchySelectionSingle(SelectionKind::Entity, static_cast<int>(entities.size() - 1));
    scene_.ResetSimulation(false);
}

void EditorApp::HandleViewportSelectionAndPlacement(glm::vec2 world_position) {
    if (!viewport_hovered_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            dragging_selection_ = false;
            resizing_selection_ = false;
            marquee_selecting_ = false;
            constructor_dragging_ = false;
            resize_handle_ = ResizeHandle::None;
            constructor_handle_ = ResizeHandle::None;
            drag_selection_snapshots_.clear();
            constructor_source_selection_.clear();
            constructor_applied_steps_.clear();
            paint_stamps_.clear();
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            panning_camera_ = false;
        }
        return;
    }

    const bool ctrl_down = GetInput().IsKeyDown(SDL_SCANCODE_LCTRL) || GetInput().IsKeyDown(SDL_SCANCODE_RCTRL);
    const bool shift_paint = GetInput().IsKeyDown(SDL_SCANCODE_LSHIFT) || GetInput().IsKeyDown(SDL_SCANCODE_RSHIFT);
    const bool alt_resize = GetInput().IsKeyDown(SDL_SCANCODE_LALT) || GetInput().IsKeyDown(SDL_SCANCODE_RALT);
    const bool constructor_mode = ctrl_down && alt_resize;
    const bool ctrl_free_move = ctrl_down && !alt_resize;
    const glm::vec2 camera_viewport = scene_.Camera().Viewport();

    auto snap_position = [&](glm::vec2 position) {
        if (!snap_to_grid_ || ctrl_free_move) {
            return position;
        }
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        position.x = std::round(position.x / grid_w) * grid_w;
        position.y = std::round(position.y / grid_h) * grid_h;
        return position;
    };
    auto snap_scalar = [&](float value, bool horizontal) {
        if (!snap_to_grid_ || ctrl_free_move) {
            return value;
        }
        const float grid = static_cast<float>(std::max(horizontal ? scene_.Level().tile_width : scene_.Level().tile_height, 1));
        return std::round(value / grid) * grid;
    };

    const auto to_screen = [&](const ImVec2& mouse) {
        return glm::vec2{
            ((mouse.x - viewport_image_min_.x) / std::max(viewport_image_size_.x, 1.0f)) * camera_viewport.x,
            ((mouse.y - viewport_image_min_.y) / std::max(viewport_image_size_.y, 1.0f)) * camera_viewport.y
        };
    };

    auto paint_key = [&](BrowserSelectionKind kind, glm::vec2 position) {
        const glm::vec2 snapped = snap_position(position);
        return std::to_string(static_cast<int>(kind)) + ":" +
            std::to_string(static_cast<int>(std::round(snapped.x))) + ":" +
            std::to_string(static_cast<int>(std::round(snapped.y))) + ":" +
            placement_relative_;
    };
    auto point_in_rect = [&](glm::vec2 point, glm::vec2 position, glm::vec2 size) {
        return point.x >= position.x && point.y >= position.y &&
               point.x <= position.x + size.x && point.y <= position.y + size.y;
    };
    auto rects_intersect = [&](glm::vec2 a_position, glm::vec2 a_size, glm::vec2 b_position, glm::vec2 b_size) {
        return a_position.x <= b_position.x + b_size.x &&
               a_position.x + a_size.x >= b_position.x &&
               a_position.y <= b_position.y + b_size.y &&
               a_position.y + a_size.y >= b_position.y;
    };
    auto screen_from_world = [&](glm::vec2 world) {
        const glm::vec2 screen = scene_.Camera().WorldToScreen(world);
        return ImVec2{
            viewport_image_min_.x + (screen.x / std::max(camera_viewport.x, 1.0f)) * viewport_image_size_.x,
            viewport_image_min_.y + (screen.y / std::max(camera_viewport.y, 1.0f)) * viewport_image_size_.y
        };
    };
    auto selected_rect = [&]() -> std::optional<std::pair<glm::vec2, glm::vec2>> {
        if (selection_kind_ == SelectionKind::Entity &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())) {
            const auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(selection_index_)];
            return std::make_pair(entity.position, entity.size);
        }
        if (selection_kind_ == SelectionKind::Trigger &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) {
            const auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(selection_index_)];
            return std::make_pair(trigger.position, trigger.size);
        }
        if (selection_kind_ == SelectionKind::VirtualCamera &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
            const auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(selection_index_)];
            return std::make_pair(camera.position, camera.size);
        }
        return std::nullopt;
    };
    auto selected_entity_bounds = [&]() -> std::optional<std::pair<glm::vec2, glm::vec2>> {
        if (hierarchy_selection_.empty()) {
            return std::nullopt;
        }
        std::optional<std::pair<glm::vec2, glm::vec2>> bounds;
        for (const auto& item : hierarchy_selection_) {
            if (item.kind != SelectionKind::Entity ||
                item.index < 0 ||
                item.index >= static_cast<int>(scene_.EditableLevel().entities.size())) {
                return std::nullopt;
            }
            const auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)];
            if (!bounds.has_value()) {
                bounds = std::make_pair(entity.position, entity.size);
                continue;
            }
            auto& [bounds_position, bounds_size] = *bounds;
            const glm::vec2 min_corner{
                std::min(bounds_position.x, entity.position.x),
                std::min(bounds_position.y, entity.position.y)
            };
            const glm::vec2 max_corner{
                std::max(bounds_position.x + bounds_size.x, entity.position.x + entity.size.x),
                std::max(bounds_position.y + bounds_size.y, entity.position.y + entity.size.y)
            };
            bounds_position = min_corner;
            bounds_size = max_corner - min_corner;
        }
        return bounds;
    };
    auto item_position = [&](const HierarchySelectionItem& item) -> std::optional<glm::vec2> {
        switch (item.kind) {
            case SelectionKind::Entity:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
                    return scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)].position;
                }
                break;
            case SelectionKind::Trigger:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().triggers.size())) {
                    return scene_.EditableLevel().triggers[static_cast<std::size_t>(item.index)].position;
                }
                break;
            case SelectionKind::Parallax:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
                    return scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(item.index)].offset;
                }
                break;
            case SelectionKind::Light:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().lights.size())) {
                    return scene_.EditableLevel().lights[static_cast<std::size_t>(item.index)].position;
                }
                break;
            case SelectionKind::AudioSource:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
                    return scene_.EditableLevel().audio_sources[static_cast<std::size_t>(item.index)].position;
                }
                break;
            case SelectionKind::AudioPak:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
                    return scene_.EditableLevel().audio_paks[static_cast<std::size_t>(item.index)].position;
                }
                break;
            case SelectionKind::VirtualCamera:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
                    return scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(item.index)].position;
                }
                break;
            case SelectionKind::SceneAnimation:
            case SelectionKind::None:
            default:
                break;
        }
        return std::nullopt;
    };
    auto set_item_position = [&](const HierarchySelectionItem& item, glm::vec2 position) {
        switch (item.kind) {
            case SelectionKind::Entity:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
                    scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)].position = position;
                }
                break;
            case SelectionKind::Trigger:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().triggers.size())) {
                    scene_.EditableLevel().triggers[static_cast<std::size_t>(item.index)].position = position;
                }
                break;
            case SelectionKind::Parallax:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
                    scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(item.index)].offset = position;
                }
                break;
            case SelectionKind::Light:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().lights.size())) {
                    scene_.EditableLevel().lights[static_cast<std::size_t>(item.index)].position = position;
                }
                break;
            case SelectionKind::AudioSource:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
                    scene_.EditableLevel().audio_sources[static_cast<std::size_t>(item.index)].position = position;
                }
                break;
            case SelectionKind::AudioPak:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
                    scene_.EditableLevel().audio_paks[static_cast<std::size_t>(item.index)].position = position;
                }
                break;
            case SelectionKind::VirtualCamera:
                if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
                    scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(item.index)].position = position;
                }
                break;
            case SelectionKind::SceneAnimation:
            case SelectionKind::None:
            default:
                break;
        }
    };
    auto handle_from_mouse = [&](glm::vec2 position, glm::vec2 size) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const std::array<std::pair<ResizeHandle, glm::vec2>, 4> handles{{
            {ResizeHandle::Left, {position.x, position.y + size.y * 0.5f}},
            {ResizeHandle::Right, {position.x + size.x, position.y + size.y * 0.5f}},
            {ResizeHandle::Top, {position.x + size.x * 0.5f, position.y}},
            {ResizeHandle::Bottom, {position.x + size.x * 0.5f, position.y + size.y}}
        }};
        for (const auto& [handle, point] : handles) {
            const ImVec2 screen = screen_from_world(point);
            const float dx = mouse.x - screen.x;
            const float dy = mouse.y - screen.y;
            if ((dx * dx + dy * dy) <= 121.0f) {
                return handle;
            }
        }
        return ResizeHandle::None;
    };
    auto begin_drag = [&](SelectionKind kind, int index, glm::vec2 anchor_position) {
        const bool drag_existing_group = hierarchy_selection_.size() > 1 && IsHierarchyItemSelected(kind, index);
        if (!drag_existing_group) {
            SetHierarchySelectionSingle(kind, index);
        } else {
            selection_kind_ = kind;
            selection_index_ = index;
        }
        dragging_selection_ = true;
        drag_grab_offset_ = world_position - anchor_position;
        drag_selection_snapshots_.clear();
        const auto& source_items = drag_existing_group ? hierarchy_selection_ : std::vector<HierarchySelectionItem>{{kind, index}};
        for (const auto& item : source_items) {
            if (const auto position = item_position(item); position.has_value()) {
                drag_selection_snapshots_.push_back({item, *position});
            }
        }
    };

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        panning_camera_ = true;
        scene_.SetCameraFollowEnabled(false);
        camera_pan_origin_mouse_ = {ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y};
        camera_pan_origin_position_ = scene_.Camera().Position();
    }
    if (panning_camera_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            const glm::vec2 origin_screen = to_screen({camera_pan_origin_mouse_.x, camera_pan_origin_mouse_.y});
            const glm::vec2 current_screen = to_screen(ImGui::GetIO().MousePos);
            const glm::vec2 origin_world = scene_.Camera().ScreenToWorld(origin_screen);
            const glm::vec2 current_world = scene_.Camera().ScreenToWorld(current_screen);
            scene_.Camera().SetPosition(camera_pan_origin_position_ - (current_world - origin_world));
        } else {
            panning_camera_ = false;
        }
        return;
    }

    if (tile_paint_mode_ &&
        active_tile_layer_index_ >= 0 &&
        active_tile_layer_index_ < static_cast<int>(scene_.EditableLevel().tile_layers.size())) {
        auto& tile_layer = scene_.EditableLevel().tile_layers[static_cast<std::size_t>(active_tile_layer_index_)];
        EnsureTileLayerStorage(tile_layer);
        const float grid_w = static_cast<float>(std::max(scene_.Level().tile_width, 1));
        const float grid_h = static_cast<float>(std::max(scene_.Level().tile_height, 1));
        const int tile_x = static_cast<int>(std::floor(world_position.x / grid_w));
        const int tile_y = static_cast<int>(std::floor(world_position.y / grid_h));
        auto write_tile = [&](int tile_value) {
            if (tile_x < 0 || tile_y < 0 || tile_x >= tile_layer.width || tile_y >= tile_layer.height) {
                return false;
            }
            const std::string key = "tile:" + std::to_string(active_tile_layer_index_) + ":" + std::to_string(tile_x) + ":" + std::to_string(tile_y) + ":" + std::to_string(tile_value);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            const int tile_index = tile_y * tile_layer.width + tile_x;
            if (tile_index < 0 || tile_index >= static_cast<int>(tile_layer.tiles.size())) {
                return false;
            }
            if (tile_layer.tiles[static_cast<std::size_t>(tile_index)] == tile_value) {
                return true;
            }
            tile_layer.tiles[static_cast<std::size_t>(tile_index)] = tile_value;
            pending_history_label_ = tile_value == 0 ? "Erase tile" : "Paint tile";
            scene_.ResetSimulation(false);
            return true;
        };

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            write_tile(std::max(selected_tileset_index_ + 1, 0));
            return;
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            write_tile(0);
            return;
        }
    }

    if (marquee_selecting_) {
        marquee_current_world_ = world_position;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            marquee_selecting_ = false;
            const glm::vec2 min_corner{
                std::min(marquee_start_world_.x, marquee_current_world_.x),
                std::min(marquee_start_world_.y, marquee_current_world_.y)
            };
            const glm::vec2 max_corner{
                std::max(marquee_start_world_.x, marquee_current_world_.x),
                std::max(marquee_start_world_.y, marquee_current_world_.y)
            };
            const glm::vec2 marquee_size = max_corner - min_corner;
            const bool is_click = marquee_size.x < 4.0f && marquee_size.y < 4.0f;
            if (is_click) {
                ClearHierarchySelection();
                selection_kind_ = SelectionKind::None;
                selection_index_ = -1;
            } else {
                std::vector<HierarchySelectionItem> picked_items;
                for (int index = 0; index < static_cast<int>(scene_.EditableLevel().triggers.size()); ++index) {
                    const auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(index)];
                    if (rects_intersect(min_corner, marquee_size, trigger.position, trigger.size)) {
                        picked_items.push_back({SelectionKind::Trigger, index});
                    }
                }
                for (int index = 0; index < static_cast<int>(scene_.EditableLevel().lights.size()); ++index) {
                    if (point_in_rect(scene_.EditableLevel().lights[static_cast<std::size_t>(index)].position, min_corner, marquee_size)) {
                        picked_items.push_back({SelectionKind::Light, index});
                    }
                }
                for (int index = 0; index < static_cast<int>(scene_.EditableLevel().audio_sources.size()); ++index) {
                    if (point_in_rect(scene_.EditableLevel().audio_sources[static_cast<std::size_t>(index)].position, min_corner, marquee_size)) {
                        picked_items.push_back({SelectionKind::AudioSource, index});
                    }
                }
                for (int index = 0; index < static_cast<int>(scene_.EditableLevel().audio_paks.size()); ++index) {
                    if (point_in_rect(scene_.EditableLevel().audio_paks[static_cast<std::size_t>(index)].position, min_corner, marquee_size)) {
                        picked_items.push_back({SelectionKind::AudioPak, index});
                    }
                }
                for (int index = 0; index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size()); ++index) {
                    const auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(index)];
                    if (rects_intersect(min_corner, marquee_size, camera.position, camera.size)) {
                        picked_items.push_back({SelectionKind::VirtualCamera, index});
                    }
                }
                for (int index = 0; index < static_cast<int>(scene_.EditableLevel().parallax_layers.size()); ++index) {
                    if (point_in_rect(scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(index)].offset, min_corner, marquee_size)) {
                        picked_items.push_back({SelectionKind::Parallax, index});
                    }
                }
                for (int index = 0; index < static_cast<int>(scene_.EditableLevel().entities.size()); ++index) {
                    const auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(index)];
                    if (rects_intersect(min_corner, marquee_size, entity.position, entity.size)) {
                        picked_items.push_back({SelectionKind::Entity, index});
                    }
                }
                ApplyHierarchySelection(picked_items);
            }
        }
        return;
    }

    if (constructor_dragging_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            constructor_dragging_ = false;
            constructor_handle_ = ResizeHandle::None;
            constructor_source_selection_.clear();
            constructor_applied_steps_.clear();
        } else if (const auto bounds = selected_entity_bounds(); bounds.has_value()) {
            auto unique_entity_id = [&](const std::string& base) {
                std::string candidate = base.empty() ? "entity_copy" : base + "_copy";
                int suffix = 1;
                while (std::any_of(scene_.EditableLevel().entities.begin(), scene_.EditableLevel().entities.end(),
                           [&](const assets::EntityDefinition& entity) { return entity.id == candidate; })) {
                    candidate = (base.empty() ? "entity" : base) + "_copy_" + std::to_string(suffix++);
                }
                return candidate;
            };

            const float step_size =
                (constructor_handle_ == ResizeHandle::Left || constructor_handle_ == ResizeHandle::Right)
                    ? std::max(constructor_origin_size_.x, static_cast<float>(std::max(scene_.Level().tile_width, 1)))
                    : std::max(constructor_origin_size_.y, static_cast<float>(std::max(scene_.Level().tile_height, 1)));
            float distance = 0.0f;
            if (constructor_handle_ == ResizeHandle::Left) {
                distance = constructor_origin_position_.x - snap_scalar(world_position.x, true);
            } else if (constructor_handle_ == ResizeHandle::Right) {
                distance = snap_scalar(world_position.x, true) - (constructor_origin_position_.x + constructor_origin_size_.x);
            } else if (constructor_handle_ == ResizeHandle::Top) {
                distance = constructor_origin_position_.y - snap_scalar(world_position.y, false);
            } else if (constructor_handle_ == ResizeHandle::Bottom) {
                distance = snap_scalar(world_position.y, false) - (constructor_origin_position_.y + constructor_origin_size_.y);
            }
            const int steps = distance > 0.0f ? static_cast<int>(std::floor(distance / std::max(step_size, 1.0f))) : 0;
            for (int step = 1; step <= steps; ++step) {
                if (!constructor_applied_steps_.insert(step).second) {
                    continue;
                }
                glm::vec2 offset{0.0f, 0.0f};
                if (constructor_handle_ == ResizeHandle::Left) {
                    offset.x = -step_size * static_cast<float>(step);
                } else if (constructor_handle_ == ResizeHandle::Right) {
                    offset.x = step_size * static_cast<float>(step);
                } else if (constructor_handle_ == ResizeHandle::Top) {
                    offset.y = -step_size * static_cast<float>(step);
                } else if (constructor_handle_ == ResizeHandle::Bottom) {
                    offset.y = step_size * static_cast<float>(step);
                }

                pending_history_label_ = "Construct entity strip";
                std::vector<HierarchySelectionItem> extended_selection = hierarchy_selection_;
                for (const auto& item : constructor_source_selection_) {
                    if (item.kind != SelectionKind::Entity ||
                        item.index < 0 ||
                        item.index >= static_cast<int>(scene_.EditableLevel().entities.size())) {
                        continue;
                    }
                    auto clone = scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)];
                    clone.id = unique_entity_id(clone.id);
                    clone.position += offset;
                    scene_.EditableLevel().entities.push_back(std::move(clone));
                    extended_selection.push_back({SelectionKind::Entity, static_cast<int>(scene_.EditableLevel().entities.size() - 1)});
                }
                ApplyHierarchySelection(extended_selection);
                scene_.ResetSimulation(false);
            }
        }
        return;
    }

    if (resizing_selection_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            resizing_selection_ = false;
            resize_handle_ = ResizeHandle::None;
        } else if (const auto rect = selected_rect(); rect.has_value()) {
            glm::vec2 position = resize_origin_position_;
            glm::vec2 size = resize_origin_size_;
            const float minimum_size = 8.0f;

            if (resize_handle_ == ResizeHandle::Left) {
                const float right = resize_origin_position_.x + resize_origin_size_.x;
                position.x = std::min(snap_scalar(world_position.x, true), right - minimum_size);
                size.x = std::max(minimum_size, right - position.x);
            } else if (resize_handle_ == ResizeHandle::Right) {
                const float right = std::max(snap_scalar(world_position.x, true), resize_origin_position_.x + minimum_size);
                size.x = std::max(minimum_size, right - resize_origin_position_.x);
            } else if (resize_handle_ == ResizeHandle::Top) {
                const float bottom = resize_origin_position_.y + resize_origin_size_.y;
                position.y = std::min(snap_scalar(world_position.y, false), bottom - minimum_size);
                size.y = std::max(minimum_size, bottom - position.y);
            } else if (resize_handle_ == ResizeHandle::Bottom) {
                const float bottom = std::max(snap_scalar(world_position.y, false), resize_origin_position_.y + minimum_size);
                size.y = std::max(minimum_size, bottom - resize_origin_position_.y);
            }

            if (selection_kind_ == SelectionKind::Entity &&
                selection_index_ >= 0 &&
                selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())) {
                pending_history_label_ = "Resize entity";
                auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(selection_index_)];
                entity.position = position;
                entity.size = size;
                if (entity.collidable) {
                    entity.collider_size = size;
                }
            } else if (selection_kind_ == SelectionKind::Trigger &&
                       selection_index_ >= 0 &&
                       selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) {
                pending_history_label_ = "Resize trigger";
                auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(selection_index_)];
                trigger.position = position;
                trigger.size = size;
            } else if (selection_kind_ == SelectionKind::VirtualCamera &&
                       selection_index_ >= 0 &&
                       selection_index_ < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
                pending_history_label_ = "Resize virtual camera";
                auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(selection_index_)];
                camera.position = position;
                camera.size = size;
            }
            scene_.ResetSimulation(false);
        }
        return;
    }

    auto clone_selected_to = [&](glm::vec2 position) -> bool {
        const glm::vec2 snapped = snap_position(position);
        if (selection_kind_ == SelectionKind::Entity &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())) {
            const auto source = scene_.EditableLevel().entities[static_cast<std::size_t>(selection_index_)];
            auto clone = source;
            clone.id = source.id + "_" + std::to_string(scene_.EditableLevel().entities.size());
            clone.position = snapped - source.size * 0.5f;
            scene_.EditableLevel().entities.push_back(std::move(clone));
            SetHierarchySelectionSingle(SelectionKind::Entity, static_cast<int>(scene_.EditableLevel().entities.size() - 1));
            scene_.ResetSimulation(false);
            return true;
        }
        if (selection_kind_ == SelectionKind::Trigger &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) {
            const auto source = scene_.EditableLevel().triggers[static_cast<std::size_t>(selection_index_)];
            auto clone = source;
            clone.name = source.name + "_" + std::to_string(scene_.EditableLevel().triggers.size());
            clone.position = snapped - source.size * 0.5f;
            scene_.EditableLevel().triggers.push_back(std::move(clone));
            SetHierarchySelectionSingle(SelectionKind::Trigger, static_cast<int>(scene_.EditableLevel().triggers.size() - 1));
            scene_.ResetSimulation(false);
            return true;
        }
        if (selection_kind_ == SelectionKind::Light &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().lights.size())) {
            const auto source = scene_.EditableLevel().lights[static_cast<std::size_t>(selection_index_)];
            auto clone = source;
            clone.name = source.name + "_" + std::to_string(scene_.EditableLevel().lights.size());
            clone.position = snapped;
            scene_.EditableLevel().lights.push_back(std::move(clone));
            SetHierarchySelectionSingle(SelectionKind::Light, static_cast<int>(scene_.EditableLevel().lights.size() - 1));
            scene_.ResetSimulation(false);
            return true;
        }
        if (selection_kind_ == SelectionKind::AudioSource &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
            const auto source = scene_.EditableLevel().audio_sources[static_cast<std::size_t>(selection_index_)];
            auto clone = source;
            clone.id = source.id + "_" + std::to_string(scene_.EditableLevel().audio_sources.size());
            clone.name = source.name + "_" + std::to_string(scene_.EditableLevel().audio_sources.size());
            clone.position = snapped;
            scene_.EditableLevel().audio_sources.push_back(std::move(clone));
            SetHierarchySelectionSingle(SelectionKind::AudioSource, static_cast<int>(scene_.EditableLevel().audio_sources.size() - 1));
            scene_.ResetSimulation(false);
            return true;
        }
        if (selection_kind_ == SelectionKind::AudioPak &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
            const auto source = scene_.EditableLevel().audio_paks[static_cast<std::size_t>(selection_index_)];
            auto clone = source;
            clone.id = source.id + "_" + std::to_string(scene_.EditableLevel().audio_paks.size());
            clone.name = source.name + "_" + std::to_string(scene_.EditableLevel().audio_paks.size());
            clone.position = snapped;
            scene_.EditableLevel().audio_paks.push_back(std::move(clone));
            SetHierarchySelectionSingle(SelectionKind::AudioPak, static_cast<int>(scene_.EditableLevel().audio_paks.size() - 1));
            scene_.ResetSimulation(false);
            return true;
        }
        if (selection_kind_ == SelectionKind::VirtualCamera &&
            selection_index_ >= 0 &&
            selection_index_ < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
            const auto source = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(selection_index_)];
            auto clone = source;
            clone.id = source.id + "_" + std::to_string(scene_.EditableLevel().virtual_cameras.size());
            clone.name = source.name + "_" + std::to_string(scene_.EditableLevel().virtual_cameras.size());
            clone.position = snapped - source.size * 0.5f;
            scene_.EditableLevel().virtual_cameras.push_back(std::move(clone));
            SetHierarchySelectionSingle(SelectionKind::VirtualCamera, static_cast<int>(scene_.EditableLevel().virtual_cameras.size() - 1));
            scene_.ResetSimulation(false);
            return true;
        }
        return false;
    };

    auto paint_current = [&](glm::vec2 position) -> bool {
        if (placement_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(placement_relative_))) {
            const std::string key = paint_key(placement_kind_, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            PlaceTextureEntity(placement_relative_, position);
            return true;
        }
        if ((placement_kind_ == BrowserSelectionKind::File && IsAudioExtension(std::filesystem::path(placement_relative_))) ||
            placement_kind_ == BrowserSelectionKind::AudioSource) {
            const std::string key = paint_key(BrowserSelectionKind::AudioSource, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            PlaceAudioSource(placement_relative_, position);
            return true;
        }
        if (placement_kind_ == BrowserSelectionKind::Object) {
            const std::string key = paint_key(placement_kind_, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            PlaceObjectEntity(placement_relative_, position);
            return true;
        }
        if (placement_kind_ == BrowserSelectionKind::Trigger) {
            const std::string key = paint_key(placement_kind_, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            PlaceTriggerZone(placement_relative_, position);
            return true;
        }
        if (placement_kind_ == BrowserSelectionKind::Light) {
            const std::string key = paint_key(placement_kind_, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            PlaceLight(position, placement_relative_ == "__builtin_flashlight__" ? "flashlight" : "point");
            return true;
        }
        if (placement_kind_ == BrowserSelectionKind::AudioPak) {
            const std::string key = paint_key(placement_kind_, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            PlaceAudioPak(position);
            return true;
        }
        if (placement_kind_ == BrowserSelectionKind::Animation) {
            const std::string key = paint_key(placement_kind_, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            PlaceSceneAnimation(placement_relative_, position);
            return true;
        }
        if (placement_kind_ == BrowserSelectionKind::Particle) {
            const std::string key = paint_key(placement_kind_, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            PlaceParticleEmitter(placement_relative_, position);
            return true;
        }
        if (selection_kind_ == SelectionKind::Entity || selection_kind_ == SelectionKind::Trigger || selection_kind_ == SelectionKind::Light ||
            selection_kind_ == SelectionKind::AudioSource || selection_kind_ == SelectionKind::AudioPak ||
            selection_kind_ == SelectionKind::VirtualCamera) {
            const std::string key = paint_key(BrowserSelectionKind::None, position);
            if (!paint_stamps_.insert(key).second) {
                return true;
            }
            return clone_selected_to(position);
        }
        return false;
    };

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        paint_stamps_.clear();
    }

    if (shift_paint && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (paint_current(world_position)) {
            return;
        }
    }

    if (!placement_relative_.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        world_position = snap_position(world_position);
        if (placement_kind_ == BrowserSelectionKind::File && IsImageExtension(std::filesystem::path(placement_relative_))) {
            PlaceTextureEntity(placement_relative_, world_position);
        } else if (placement_kind_ == BrowserSelectionKind::File && IsAudioExtension(std::filesystem::path(placement_relative_))) {
            PlaceAudioSource(placement_relative_, world_position);
        } else if (placement_kind_ == BrowserSelectionKind::Object) {
            PlaceObjectEntity(placement_relative_, world_position);
        } else if (placement_kind_ == BrowserSelectionKind::Trigger) {
            PlaceTriggerZone(placement_relative_, world_position);
        } else if (placement_kind_ == BrowserSelectionKind::Animation) {
            PlaceSceneAnimation(placement_relative_, world_position);
        } else if (placement_kind_ == BrowserSelectionKind::Particle) {
            PlaceParticleEmitter(placement_relative_, world_position);
        } else if (placement_kind_ == BrowserSelectionKind::Light) {
            PlaceLight(world_position, placement_relative_ == "__builtin_flashlight__" ? "flashlight" : "point");
        } else if (placement_kind_ == BrowserSelectionKind::AudioPak) {
            PlaceAudioPak(world_position);
        }
        if (!ImGui::GetIO().KeyShift) {
            placement_kind_ = BrowserSelectionKind::None;
            placement_relative_.clear();
        }
        return;
    }

    const auto parallax_handle_screen = [&](glm::vec2 offset) {
        return screen_from_world(offset);
    };

    const auto light_handle_screen = [&](glm::vec2 position) {
        return screen_from_world(position);
    };

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragging_selection_ = false;
        resizing_selection_ = false;
        marquee_selecting_ = false;
        constructor_dragging_ = false;
        resize_handle_ = ResizeHandle::None;
        constructor_handle_ = ResizeHandle::None;
        drag_selection_snapshots_.clear();
        constructor_source_selection_.clear();
        constructor_applied_steps_.clear();

        if (constructor_mode) {
            if (const auto rect = selected_entity_bounds(); rect.has_value()) {
                const ResizeHandle hovered_handle = handle_from_mouse(rect->first, rect->second);
                if (hovered_handle != ResizeHandle::None) {
                    constructor_dragging_ = true;
                    constructor_handle_ = hovered_handle;
                    constructor_origin_position_ = rect->first;
                    constructor_origin_size_ = rect->second;
                    constructor_source_selection_ = hierarchy_selection_.empty()
                        ? std::vector<HierarchySelectionItem>{}
                        : hierarchy_selection_;
                    constructor_source_selection_.erase(
                        std::remove_if(constructor_source_selection_.begin(), constructor_source_selection_.end(),
                            [&](const HierarchySelectionItem& item) {
                                return item.kind != SelectionKind::Entity ||
                                       item.index < 0 ||
                                       item.index >= static_cast<int>(scene_.EditableLevel().entities.size());
                            }),
                        constructor_source_selection_.end());
                    if (constructor_source_selection_.empty() &&
                        selection_kind_ == SelectionKind::Entity &&
                        selection_index_ >= 0 &&
                        selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())) {
                        constructor_source_selection_.push_back({SelectionKind::Entity, selection_index_});
                    }
                    return;
                }
            }
        } else if (alt_resize) {
            if (const auto rect = selected_rect(); rect.has_value()) {
                const ResizeHandle hovered_handle = handle_from_mouse(rect->first, rect->second);
                if (hovered_handle != ResizeHandle::None) {
                    resizing_selection_ = true;
                    resize_handle_ = hovered_handle;
                    resize_origin_position_ = rect->first;
                    resize_origin_size_ = rect->second;
                    return;
                }
            }
        }

        for (int i = static_cast<int>(scene_.EditableLevel().triggers.size()) - 1; i >= 0; --i) {
            auto& trigger = scene_.EditableLevel().triggers[static_cast<std::size_t>(i)];
            if (point_in_rect(world_position, trigger.position, trigger.size)) {
                begin_drag(SelectionKind::Trigger, i, trigger.position);
                return;
            }
        }

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for (int i = static_cast<int>(scene_.EditableLevel().lights.size()) - 1; i >= 0; --i) {
            const auto& light = scene_.EditableLevel().lights[static_cast<std::size_t>(i)];
            const ImVec2 handle = light_handle_screen(light.position);
            const float dx = mouse.x - handle.x;
            const float dy = mouse.y - handle.y;
            if ((dx * dx + dy * dy) <= 144.0f) {
                begin_drag(SelectionKind::Light, i, light.position);
                return;
            }
        }

        for (int i = static_cast<int>(scene_.EditableLevel().audio_sources.size()) - 1; i >= 0; --i) {
            const auto& source = scene_.EditableLevel().audio_sources[static_cast<std::size_t>(i)];
            const ImVec2 handle = light_handle_screen(source.position);
            const float dx = mouse.x - handle.x;
            const float dy = mouse.y - handle.y;
            if ((dx * dx + dy * dy) <= 144.0f) {
                begin_drag(SelectionKind::AudioSource, i, source.position);
                return;
            }
        }

        for (int i = static_cast<int>(scene_.EditableLevel().audio_paks.size()) - 1; i >= 0; --i) {
            const auto& pak = scene_.EditableLevel().audio_paks[static_cast<std::size_t>(i)];
            const ImVec2 handle = light_handle_screen(pak.position);
            const float dx = mouse.x - handle.x;
            const float dy = mouse.y - handle.y;
            if ((dx * dx + dy * dy) <= 169.0f) {
                begin_drag(SelectionKind::AudioPak, i, pak.position);
                return;
            }
        }

        for (int i = static_cast<int>(scene_.EditableLevel().virtual_cameras.size()) - 1; i >= 0; --i) {
            const auto& camera = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(i)];
            const ImVec2 handle = light_handle_screen(camera.position + camera.size * 0.5f);
            const float dx = mouse.x - handle.x;
            const float dy = mouse.y - handle.y;
            if ((dx * dx + dy * dy) <= 196.0f) {
                begin_drag(SelectionKind::VirtualCamera, i, camera.position);
                return;
            }
        }

        for (int i = static_cast<int>(scene_.EditableLevel().parallax_layers.size()) - 1; i >= 0; --i) {
            const auto& layer = scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(i)];
            const ImVec2 handle = parallax_handle_screen(layer.offset);
            const float dx = mouse.x - handle.x;
            const float dy = mouse.y - handle.y;
            if ((dx * dx + dy * dy) <= 100.0f) {
                begin_drag(SelectionKind::Parallax, i, layer.offset);
                return;
            }
        }

        std::vector<int> entity_pick_order(scene_.EditableLevel().entities.size());
        std::iota(entity_pick_order.begin(), entity_pick_order.end(), 0);
        std::sort(entity_pick_order.begin(), entity_pick_order.end(), [&](int lhs, int rhs) {
            const auto& a = scene_.EditableLevel().entities[static_cast<std::size_t>(lhs)];
            const auto& b = scene_.EditableLevel().entities[static_cast<std::size_t>(rhs)];
            const bool a_particle = IsParticleEmitterEntity(a);
            const bool b_particle = IsParticleEmitterEntity(b);
            if (a_particle != b_particle) {
                return a_particle;
            }
            if (a.layer != b.layer) {
                return a.layer > b.layer;
            }
            return lhs > rhs;
        });
        for (int i : entity_pick_order) {
            auto& entity = scene_.EditableLevel().entities[static_cast<std::size_t>(i)];
            if (point_in_rect(world_position, entity.position, entity.size)) {
                begin_drag(SelectionKind::Entity, i, entity.position);
                return;
            }
        }

        marquee_selecting_ = true;
        marquee_start_world_ = world_position;
        marquee_current_world_ = world_position;
    }

    if (dragging_selection_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const auto active_snapshot = std::find_if(drag_selection_snapshots_.begin(), drag_selection_snapshots_.end(),
            [&](const DragSelectionSnapshot& snapshot) {
                return snapshot.item.kind == selection_kind_ && snapshot.item.index == selection_index_;
            });
        if (active_snapshot != drag_selection_snapshots_.end()) {
            const glm::vec2 anchor_target =
                selection_kind_ == SelectionKind::Parallax
                    ? (world_position - drag_grab_offset_)
                    : snap_position(world_position - drag_grab_offset_);
            const glm::vec2 delta = anchor_target - active_snapshot->position;
            pending_history_label_ = drag_selection_snapshots_.size() > 1 ? "Move selection" : "Move item";
            for (const auto& snapshot : drag_selection_snapshots_) {
                set_item_position(snapshot.item, snapshot.position + delta);
            }
            scene_.ResetSimulation(false);
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_selection_ = false;
        drag_selection_snapshots_.clear();
    }
}

void EditorApp::ApplyProjectPresentation() {
    SetWindowTitle(project_.name + " v" + project_.version + " - " + Tr("editor.app_title", "NovaIso Editor"));
    if (!project_.icon.empty()) {
        SetWindowIcon(project_root_ / project_.icon);
    }
}

void EditorApp::EnsureLocalizationFiles() const {
    const std::filesystem::path en_path = project_root_ / "localization/editor_en.json";
    const std::filesystem::path ru_path = project_root_ / "localization/editor_ru.json";
    if (!std::filesystem::exists(en_path)) {
        core::FileIO::WriteText(en_path, R"json({
  "editor.app_title": "NovaIso Editor",
  "editor.menu.file": "File",
  "editor.menu.view": "View",
  "editor.action.save": "Save",
  "editor.action.build": "Build Package",
  "editor.action.reset": "Reset Simulation",
  "editor.action.quit": "Quit",
  "editor.toggle.live_preview": "Live Preview",
  "editor.toggle.debug_draw": "Debug Draw",
  "editor.toggle.editor_audio": "Editor Audio",
  "editor.toggle.rt_lights": "RT Lights",
  "editor.window.browser": "Content Browser",
  "editor.window.hierarchy": "Hierarchy",
  "editor.window.properties": "Properties",
  "editor.window.trigger_editor": "Trigger Editor",
  "editor.window.inspector": "Resource Inspector",
  "editor.window.project": "Project Settings",
  "editor.window.python": "Python",
  "editor.window.toolbar": "Toolbar",
  "editor.window.output": "Output",
  "editor.window.sprite_editor": "Sprite Editor",
  "editor.window.viewport": "Viewport",
  "editor.browser.import_path": "Import Path",
  "editor.browser.import": "Import From Disk",
  "editor.browser.new_sprite": "New Sprite",
  "editor.browser.new_object": "New Object",
  "editor.browser.new_trigger": "New Trigger",
  "editor.browser.help": "Double-click or drag textures, objects and triggers into the viewport.",
  "editor.browser.placement": "Placement mode:",
  "editor.browser.clear_placement": "Clear Placement",
  "editor.browser.images": "Images",
  "editor.browser.audio": "Audio",
  "editor.browser.scripts": "Scripts",
  "editor.browser.levels": "Levels",
  "editor.browser.sprites": "Sprite Resources",
  "editor.browser.objects": "Object Resources",
  "editor.browser.triggers": "Trigger Resources",
  "editor.inspector.empty": "Select a file or engine resource from the Content Browser.",
  "editor.inspector.create_sprite": "Create Sprite Resource",
  "editor.inspector.create_object": "Create Object Resource",
  "editor.inspector.place_image": "Place In Viewport",
  "editor.inspector.place_object": "Place In Viewport",
  "editor.inspector.place_trigger": "Place In Viewport",
  "editor.inspector.open_sprite_editor": "Open Sprite Editor",
  "editor.inspector.audio_info": "Audio asset. Attach this path to objects or play it from Python.",
  "editor.inspector.script_info": "Python script. Attach it to objects or entities.",
  "editor.inspector.generic_info": "Generic project file.",
  "editor.project.use_selected_icon": "Use Selected Image As Icon",
  "editor.project.localization_info": "Localization files are stored in localization/editor_en.json and localization/editor_ru.json.",
  "editor.project.save": "Save Project Settings",
  "editor.toolbar.switch_side": "Switch To Side",
  "editor.toolbar.switch_iso": "Switch To Isometric",
  "editor.toolbar.follow_camera": "Follow Player",
  "editor.toolbar.mode_edit": "Edit",
  "editor.toolbar.mode_preview": "Preview",
  "editor.toolbar.snap_grid": "Snap To Grid",
  "editor.toolbar.save_all": "Save All",
  "editor.resource.save": "Save Resource",
  "editor.resource.rename_file": "Rename File From Resource Name",
  "editor.sprite_editor.empty": "Select a sprite resource in the Content Browser to edit it.",
  "editor.viewport.help": "Middle mouse pans the camera. Left mouse selects or places assets."
})json");
    }
    if (!std::filesystem::exists(ru_path)) {
        core::FileIO::WriteText(ru_path, R"json({
  "editor.app_title": "Редактор NovaIso",
  "editor.menu.file": "Файл",
  "editor.menu.view": "Вид",
  "editor.action.save": "Сохранить",
  "editor.action.build": "Собрать пакет",
  "editor.action.reset": "Сбросить симуляцию",
  "editor.action.quit": "Выход",
  "editor.toggle.live_preview": "Живой просмотр",
  "editor.toggle.debug_draw": "Отладка",
  "editor.toggle.editor_audio": "Звук редактора",
  "editor.toggle.rt_lights": "RT свет",
  "editor.window.browser": "Проводник ресурсов",
  "editor.window.hierarchy": "Иерархия",
  "editor.window.properties": "Свойства",
  "editor.window.trigger_editor": "Редактор триггеров",
  "editor.window.inspector": "Инспектор ресурса",
  "editor.window.project": "Настройки проекта",
  "editor.window.python": "Python",
  "editor.window.toolbar": "Панель",
  "editor.window.output": "Вывод",
  "editor.window.sprite_editor": "Редактор спрайта",
  "editor.window.viewport": "Окно сцены",
  "editor.browser.import_path": "Путь импорта",
  "editor.browser.import": "Импортировать",
  "editor.browser.new_sprite": "Новый спрайт",
  "editor.browser.new_object": "Новый объект",
  "editor.browser.new_trigger": "Новый триггер",
  "editor.browser.help": "Двойной клик или перетаскивание помещают текстуры, объекты и триггеры в сцену.",
  "editor.browser.placement": "Режим размещения:",
  "editor.browser.clear_placement": "Сбросить режим",
  "editor.browser.images": "Изображения",
  "editor.browser.audio": "Звуки",
  "editor.browser.scripts": "Скрипты",
  "editor.browser.levels": "Уровни",
  "editor.browser.sprites": "Ресурсы спрайтов",
  "editor.browser.objects": "Ресурсы объектов",
  "editor.browser.triggers": "Ресурсы триггеров",
  "editor.inspector.empty": "Выберите файл или ресурс в проводнике.",
  "editor.inspector.create_sprite": "Создать ресурс спрайта",
  "editor.inspector.create_object": "Создать ресурс объекта",
  "editor.inspector.place_image": "Поставить в сцену",
  "editor.inspector.place_object": "Поставить в сцену",
  "editor.inspector.place_trigger": "Поставить в сцену",
  "editor.inspector.open_sprite_editor": "Открыть редактор спрайта",
  "editor.inspector.audio_info": "Аудио-ресурс. Его можно привязать к объекту или вызвать из Python.",
  "editor.inspector.script_info": "Python-скрипт. Его можно привязать к объекту или сущности.",
  "editor.inspector.generic_info": "Обычный файл проекта.",
  "editor.project.use_selected_icon": "Использовать выбранную картинку как иконку",
  "editor.project.localization_info": "Файлы локализации лежат в localization/editor_en.json и localization/editor_ru.json.",
  "editor.project.save": "Сохранить настройки проекта",
  "editor.toolbar.switch_side": "Переключить в сайд",
  "editor.toolbar.switch_iso": "Переключить в изометрию",
  "editor.toolbar.follow_camera": "Следовать за игроком",
  "editor.toolbar.mode_edit": "Редактирование",
  "editor.toolbar.mode_preview": "Просмотр",
  "editor.toolbar.snap_grid": "Привязка к сетке",
  "editor.toolbar.save_all": "Сохранить всё",
  "editor.resource.save": "Сохранить ресурс",
  "editor.resource.rename_file": "Переименовать файл по имени ресурса",
  "editor.sprite_editor.empty": "Выберите ресурс спрайта в проводнике, чтобы редактировать его.",
  "editor.viewport.help": "Средняя кнопка мыши двигает камеру. Левая выбирает или размещает ресурсы."
})json");
    }
}

void EditorApp::LoadEditorLocalization() {
    editor_locale_ = nlohmann::json::object();
    const std::filesystem::path path = project_root_ / "localization" / ("editor_" + project_.editor_language + ".json");
    if (std::filesystem::exists(path)) {
        editor_locale_ = nlohmann::json::parse(core::FileIO::ReadText(path), nullptr, false);
        if (editor_locale_.is_discarded()) {
            editor_locale_ = nlohmann::json::object();
        }
    }
}

std::string EditorApp::Tr(std::string_view key, std::string_view fallback) const {
    if (editor_locale_.is_object()) {
        const auto it = editor_locale_.find(std::string(key));
        if (it != editor_locale_.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return std::string(fallback);
}

bool EditorApp::RenameSelectedResourceFile() {
    if (browser_selection_relative_.empty() ||
        (browser_selection_kind_ != BrowserSelectionKind::Sprite &&
         browser_selection_kind_ != BrowserSelectionKind::Object &&
         browser_selection_kind_ != BrowserSelectionKind::Trigger &&
         browser_selection_kind_ != BrowserSelectionKind::Animation &&
         browser_selection_kind_ != BrowserSelectionKind::Particle)) {
        return false;
    }

    const std::filesystem::path old_relative(browser_selection_relative_);
    const std::string resource_name =
        browser_selection_kind_ == BrowserSelectionKind::Sprite ? selected_sprite_.name :
        browser_selection_kind_ == BrowserSelectionKind::Object ? selected_object_.name :
        browser_selection_kind_ == BrowserSelectionKind::Trigger ? selected_trigger_.name :
        browser_selection_kind_ == BrowserSelectionKind::Animation ? selected_animation_.name :
        selected_particle_.name;

    const std::filesystem::path new_relative = old_relative.parent_path() / (SanitizeName(resource_name) + old_relative.extension().string());
    if (new_relative == old_relative || std::filesystem::exists(project_root_ / new_relative)) {
        return false;
    }

    SaveSelectedResource();
    std::filesystem::rename(project_root_ / old_relative, project_root_ / new_relative);
    const std::string old_path = old_relative.generic_string();
    const std::string new_path = new_relative.generic_string();

    auto replace_in_list = [&](std::vector<std::string>& list) {
        for (auto& item : list) {
            if (item == old_path) {
                item = new_path;
            }
        }
    };
    replace_in_list(project_.sprite_assets);
    replace_in_list(project_.object_assets);
    replace_in_list(project_.trigger_assets);
    replace_in_list(project_.animation_assets);
    replace_in_list(project_.particle_assets);

    for (auto& entity : scene_.EditableLevel().entities) {
        if (entity.sprite_asset == old_path) {
            entity.sprite_asset = new_path;
        }
        if (entity.object_asset == old_path) {
            entity.object_asset = new_path;
        }
        if (entity.properties.is_object() && entity.properties.value("particle_asset", std::string{}) == old_path) {
            entity.properties["particle_asset"] = new_path;
        }
    }
    for (auto& trigger : scene_.EditableLevel().triggers) {
        if (trigger.asset == old_path) {
            trigger.asset = new_path;
        }
    }
    for (auto& animation : scene_.EditableLevel().animations) {
        if (animation.asset == old_path) {
            animation.asset = new_path;
        }
    }

    if (browser_selection_kind_ == BrowserSelectionKind::Sprite) {
        for (const auto& object_path : project_.object_assets) {
            auto object = assets::LoadObjectAsset(project_root_ / object_path);
            if (object.sprite == old_path) {
                object.sprite = new_path;
                assets::SaveObjectAsset(project_root_ / object_path, object);
            }
        }
    }

    browser_selection_relative_ = new_path;
    resource_dirty_ = true;
    SaveSelectedResource();
    assets::SaveProject(project_root_ / "project.json", project_);
    InvalidateAssetBrowserCache();
    scene_.SaveLevelToDisk();
    SyncSceneResources();
    return true;
}

bool EditorApp::ViewportMouseToWorld(glm::vec2& out_world) const {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x < viewport_image_min_.x || mouse.y < viewport_image_min_.y ||
        mouse.x > viewport_image_min_.x + viewport_image_size_.x ||
        mouse.y > viewport_image_min_.y + viewport_image_size_.y) {
        return false;
    }

    const glm::vec2 normalized{
        (mouse.x - viewport_image_min_.x) / std::max(viewport_image_size_.x, 1.0f),
        (mouse.y - viewport_image_min_.y) / std::max(viewport_image_size_.y, 1.0f)
    };
    const glm::vec2 camera_viewport = scene_.Camera().Viewport();

    out_world = scene_.Camera().ScreenToWorld({
        normalized.x * camera_viewport.x,
        normalized.y * camera_viewport.y
    });
    return true;
}

const std::vector<std::filesystem::path>& EditorApp::EnumerateRelativeFiles(const std::filesystem::path& folder, const std::string& extension) {
    if (asset_browser_cache_dirty_) {
        asset_browser_file_cache_.clear();
        asset_browser_cache_dirty_ = false;
    }
    const std::string cache_key = folder.generic_string() + "|" + extension;
    const auto cached = asset_browser_file_cache_.find(cache_key);
    if (cached != asset_browser_file_cache_.end()) {
        return cached->second;
    }

    std::vector<std::filesystem::path> result;
    const auto full_path = project_root_ / folder;
    if (!std::filesystem::exists(full_path)) {
        return asset_browser_file_cache_.emplace(cache_key, std::move(result)).first->second;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(full_path)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (!extension.empty() && entry.path().extension() != extension) {
            continue;
        }
        result.push_back(std::filesystem::relative(entry.path(), project_root_));
    }

    std::sort(result.begin(), result.end());
    return asset_browser_file_cache_.emplace(cache_key, std::move(result)).first->second;
}

void EditorApp::InvalidateAssetBrowserCache() {
    asset_browser_cache_dirty_ = true;
}

std::filesystem::path EditorApp::ActiveShaderRoot() const {
    const std::filesystem::path project_shader_root = project_root_ / "shaders";
    if (core::FileIO::Exists(project_shader_root / "post.vert")) {
        return project_shader_root;
    }
    return ExecutableDirectory() / "shaders";
}

std::string EditorApp::MakeUniqueRelativePath(const std::filesystem::path& folder, const std::string& stem, const std::string& extension) const {
    const std::string safe_stem = SanitizeName(stem);
    std::filesystem::path relative = folder / (safe_stem + extension);
    int suffix = 1;
    while (std::filesystem::exists(project_root_ / relative)) {
        relative = folder / (safe_stem + "_" + std::to_string(suffix++) + extension);
    }
    return relative.generic_string();
}

void EditorApp::EnsureProjectListContains(std::vector<std::string>& list, const std::string& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
        std::sort(list.begin(), list.end());
    }
}

void EditorApp::ClearHierarchySelection() {
    hierarchy_selection_.clear();
    hierarchy_anchor_flat_index_ = -1;
}

void EditorApp::SetHierarchySelectionSingle(SelectionKind kind, int index) {
    hierarchy_selection_.clear();
    if (kind != SelectionKind::None && index >= 0) {
        hierarchy_selection_.push_back({kind, index});
    }
    selection_kind_ = kind;
    selection_index_ = index;
    if (kind == SelectionKind::Entity &&
        index >= 0 &&
        index < static_cast<int>(scene_.EditableLevel().entities.size())) {
        current_script_relative_ = scene_.EditableLevel().entities[static_cast<std::size_t>(index)].script;
        current_code_relative_ = current_script_relative_;
        LoadCurrentScript();
    }
}

void EditorApp::ApplyHierarchySelection(const std::vector<HierarchySelectionItem>& items) {
    hierarchy_selection_.clear();
    for (const auto& item : items) {
        if (item.kind == SelectionKind::None || item.index < 0) {
            continue;
        }
        if (!IsHierarchyItemSelected(item.kind, item.index)) {
            hierarchy_selection_.push_back(item);
        }
    }
    PruneHierarchySelection();
    if (hierarchy_selection_.empty()) {
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        return;
    }
    const auto active = hierarchy_selection_.back();
    selection_kind_ = active.kind;
    selection_index_ = active.index;
    if (selection_kind_ == SelectionKind::Entity &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())) {
        current_script_relative_ = scene_.EditableLevel().entities[static_cast<std::size_t>(selection_index_)].script;
        current_code_relative_ = current_script_relative_;
        LoadCurrentScript();
    }
}

bool EditorApp::IsHierarchyItemSelected(SelectionKind kind, int index) const {
    return std::any_of(hierarchy_selection_.begin(), hierarchy_selection_.end(),
        [&](const HierarchySelectionItem& item) {
            return item.kind == kind && item.index == index;
        });
}

void EditorApp::PruneHierarchySelection() {
    auto is_valid = [&](const HierarchySelectionItem& item) {
        switch (item.kind) {
            case SelectionKind::Entity:
                return item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().entities.size());
            case SelectionKind::Trigger:
                return item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().triggers.size());
            case SelectionKind::Parallax:
                return item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size());
            case SelectionKind::Light:
                return item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().lights.size());
            case SelectionKind::AudioSource:
                return item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size());
            case SelectionKind::AudioPak:
                return item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size());
            case SelectionKind::VirtualCamera:
                return item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size());
            case SelectionKind::SceneAnimation:
                return item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().animations.size());
            case SelectionKind::None:
            default:
                return false;
        }
    };
    hierarchy_selection_.erase(std::remove_if(hierarchy_selection_.begin(), hierarchy_selection_.end(),
        [&](const HierarchySelectionItem& item) { return !is_valid(item); }),
        hierarchy_selection_.end());
}

std::string EditorApp::HierarchyItemLabel(SelectionKind kind, int index) const {
    std::string name;
    std::string group;
    switch (kind) {
        case SelectionKind::Entity:
            if (index >= 0 && index < static_cast<int>(scene_.Level().entities.size())) {
                const auto& entity = scene_.Level().entities[static_cast<std::size_t>(index)];
                name = entity.id.empty() ? "Entity" : entity.id;
                if (IsParticleEmitterEntity(entity)) {
                    name = "[FX] " + name;
                }
                group = entity.editor_group;
            }
            break;
        case SelectionKind::Trigger:
            if (index >= 0 && index < static_cast<int>(scene_.Level().triggers.size())) {
                const auto& trigger = scene_.Level().triggers[static_cast<std::size_t>(index)];
                name = "[TRG] " + std::string(trigger.name.empty() ? "Trigger" : trigger.name);
                group = trigger.editor_group;
            }
            break;
        case SelectionKind::Parallax:
            if (index >= 0 && index < static_cast<int>(scene_.Level().parallax_layers.size())) {
                const auto& layer = scene_.Level().parallax_layers[static_cast<std::size_t>(index)];
                name = layer.name.empty() ? "Parallax" : layer.name;
                group = layer.editor_group;
            }
            break;
        case SelectionKind::Light:
            if (index >= 0 && index < static_cast<int>(scene_.Level().lights.size())) {
                const auto& light = scene_.Level().lights[static_cast<std::size_t>(index)];
                name = light.name.empty() ? "Light" : light.name;
                group = light.editor_group;
            }
            break;
        case SelectionKind::AudioSource:
            if (index >= 0 && index < static_cast<int>(scene_.Level().audio_sources.size())) {
                const auto& source = scene_.Level().audio_sources[static_cast<std::size_t>(index)];
                name = "[SND] " + std::string(source.name.empty() ? "Audio Source" : source.name);
                group = source.editor_group;
            }
            break;
        case SelectionKind::AudioPak:
            if (index >= 0 && index < static_cast<int>(scene_.Level().audio_paks.size())) {
                const auto& pak = scene_.Level().audio_paks[static_cast<std::size_t>(index)];
                name = "[PAK] " + std::string(pak.name.empty() ? "Audio Pak" : pak.name);
                group = pak.editor_group;
            }
            break;
        case SelectionKind::VirtualCamera:
            if (index >= 0 && index < static_cast<int>(scene_.Level().virtual_cameras.size())) {
                const auto& camera = scene_.Level().virtual_cameras[static_cast<std::size_t>(index)];
                name = camera.name.empty() ? "Virtual Camera" : camera.name;
                group = camera.editor_group;
            }
            break;
        case SelectionKind::SceneAnimation:
            if (index >= 0 && index < static_cast<int>(scene_.Level().animations.size())) {
                const auto& animation = scene_.Level().animations[static_cast<std::size_t>(index)];
                name = animation.name.empty() ? "Scene Animation" : animation.name;
                group = animation.editor_group;
            }
            break;
        case SelectionKind::None:
        default:
            break;
    }
    if (!group.empty()) {
        name += "  [" + group + "]";
    }
    return name;
}

void EditorApp::CopyHierarchySelection() {
    PruneHierarchySelection();
    std::vector<HierarchySelectionItem> source_selection = hierarchy_selection_;
    if (source_selection.empty() && selection_kind_ != SelectionKind::None && selection_index_ >= 0) {
        source_selection.push_back({selection_kind_, selection_index_});
    }
    if (source_selection.empty()) {
        status_message_ = "Nothing selected to copy.";
        return;
    }

    selection_clipboard_ = {};
    for (const auto& item : source_selection) {
        if (item.kind == SelectionKind::Entity &&
            item.index >= 0 &&
            item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            selection_clipboard_.entities.push_back(scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)]);
        } else if (item.kind == SelectionKind::Trigger &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().triggers.size())) {
            selection_clipboard_.triggers.push_back(scene_.EditableLevel().triggers[static_cast<std::size_t>(item.index)]);
        } else if (item.kind == SelectionKind::Parallax &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
            selection_clipboard_.parallax_layers.push_back(scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(item.index)]);
        } else if (item.kind == SelectionKind::Light &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().lights.size())) {
            selection_clipboard_.lights.push_back(scene_.EditableLevel().lights[static_cast<std::size_t>(item.index)]);
        } else if (item.kind == SelectionKind::AudioSource &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
            selection_clipboard_.audio_sources.push_back(scene_.EditableLevel().audio_sources[static_cast<std::size_t>(item.index)]);
        } else if (item.kind == SelectionKind::AudioPak &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
            selection_clipboard_.audio_paks.push_back(scene_.EditableLevel().audio_paks[static_cast<std::size_t>(item.index)]);
        } else if (item.kind == SelectionKind::VirtualCamera &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
            selection_clipboard_.virtual_cameras.push_back(scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(item.index)]);
        } else if (item.kind == SelectionKind::SceneAnimation &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().animations.size())) {
            selection_clipboard_.scene_animations.push_back(scene_.EditableLevel().animations[static_cast<std::size_t>(item.index)]);
        }
    }

    const std::size_t copied_count =
        selection_clipboard_.entities.size() +
        selection_clipboard_.triggers.size() +
        selection_clipboard_.parallax_layers.size() +
        selection_clipboard_.lights.size() +
        selection_clipboard_.audio_sources.size() +
        selection_clipboard_.audio_paks.size() +
        selection_clipboard_.virtual_cameras.size() +
        selection_clipboard_.scene_animations.size();
    selection_clipboard_.has_data = copied_count > 0;
    clipboard_paste_serial_ = 0;
    status_message_ = selection_clipboard_.has_data
        ? "Copied " + std::to_string(copied_count) + " item(s)."
        : "Nothing selected to copy.";
}

void EditorApp::PasteHierarchySelection() {
    if (!selection_clipboard_.has_data) {
        status_message_ = "Clipboard is empty.";
        return;
    }

    pending_history_label_ = "Paste selection";
    ++clipboard_paste_serial_;
    const glm::vec2 duplicate_offset{
        static_cast<float>(std::max(scene_.Level().tile_width, 16) * std::max(clipboard_paste_serial_, 1)),
        static_cast<float>(std::max(scene_.Level().tile_height, 16) * std::max(clipboard_paste_serial_, 1))
    };
    auto unique_name = [](const std::string& base, const auto& exists_fn) {
        std::string candidate = base.empty() ? "item_copy" : base + "_copy";
        int suffix = 1;
        while (exists_fn(candidate)) {
            candidate = (base.empty() ? "item" : base) + "_copy_" + std::to_string(suffix++);
        }
        return candidate;
    };

    std::vector<HierarchySelectionItem> new_selection;
    for (auto clone : selection_clipboard_.entities) {
        clone.id = unique_name(clone.id, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().entities.begin(), scene_.EditableLevel().entities.end(),
                [&](const assets::EntityDefinition& entity) { return entity.id == candidate; });
        });
        clone.position += duplicate_offset;
        scene_.EditableLevel().entities.push_back(std::move(clone));
        new_selection.push_back({SelectionKind::Entity, static_cast<int>(scene_.EditableLevel().entities.size() - 1)});
    }
    for (auto clone : selection_clipboard_.triggers) {
        clone.id = unique_name(clone.id, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().triggers.begin(), scene_.EditableLevel().triggers.end(),
                [&](const assets::TriggerZone& trigger) { return trigger.id == candidate; });
        });
        clone.name = unique_name(clone.name, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().triggers.begin(), scene_.EditableLevel().triggers.end(),
                [&](const assets::TriggerZone& trigger) { return trigger.name == candidate; });
        });
        clone.position += duplicate_offset;
        clone.fired = false;
        scene_.EditableLevel().triggers.push_back(std::move(clone));
        new_selection.push_back({SelectionKind::Trigger, static_cast<int>(scene_.EditableLevel().triggers.size() - 1)});
    }
    for (auto clone : selection_clipboard_.parallax_layers) {
        clone.id = unique_name(clone.id, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().parallax_layers.begin(), scene_.EditableLevel().parallax_layers.end(),
                [&](const assets::ParallaxLayer& layer) { return layer.id == candidate; });
        });
        clone.name = unique_name(clone.name, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().parallax_layers.begin(), scene_.EditableLevel().parallax_layers.end(),
                [&](const assets::ParallaxLayer& layer) { return layer.name == candidate; });
        });
        clone.offset += duplicate_offset;
        scene_.EditableLevel().parallax_layers.push_back(std::move(clone));
        new_selection.push_back({SelectionKind::Parallax, static_cast<int>(scene_.EditableLevel().parallax_layers.size() - 1)});
    }
    for (auto clone : selection_clipboard_.lights) {
        clone.id = unique_name(clone.id, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().lights.begin(), scene_.EditableLevel().lights.end(),
                [&](const assets::LightDefinition& light) { return light.id == candidate; });
        });
        clone.name = unique_name(clone.name, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().lights.begin(), scene_.EditableLevel().lights.end(),
                [&](const assets::LightDefinition& light) { return light.name == candidate; });
        });
        clone.position += duplicate_offset;
        scene_.EditableLevel().lights.push_back(std::move(clone));
        new_selection.push_back({SelectionKind::Light, static_cast<int>(scene_.EditableLevel().lights.size() - 1)});
    }
    for (auto clone : selection_clipboard_.audio_sources) {
        clone.id = unique_name(clone.id, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().audio_sources.begin(), scene_.EditableLevel().audio_sources.end(),
                [&](const assets::AudioSourceDefinition& source) { return source.id == candidate; });
        });
        clone.name = unique_name(clone.name, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().audio_sources.begin(), scene_.EditableLevel().audio_sources.end(),
                [&](const assets::AudioSourceDefinition& source) { return source.name == candidate; });
        });
        clone.position += duplicate_offset;
        scene_.EditableLevel().audio_sources.push_back(std::move(clone));
        new_selection.push_back({SelectionKind::AudioSource, static_cast<int>(scene_.EditableLevel().audio_sources.size() - 1)});
    }
    for (auto clone : selection_clipboard_.audio_paks) {
        clone.id = unique_name(clone.id, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().audio_paks.begin(), scene_.EditableLevel().audio_paks.end(),
                [&](const assets::AudioPakDefinition& pak) { return pak.id == candidate; });
        });
        clone.name = unique_name(clone.name, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().audio_paks.begin(), scene_.EditableLevel().audio_paks.end(),
                [&](const assets::AudioPakDefinition& pak) { return pak.name == candidate; });
        });
        clone.position += duplicate_offset;
        scene_.EditableLevel().audio_paks.push_back(std::move(clone));
        new_selection.push_back({SelectionKind::AudioPak, static_cast<int>(scene_.EditableLevel().audio_paks.size() - 1)});
    }
    for (auto clone : selection_clipboard_.virtual_cameras) {
        clone.id = unique_name(clone.id, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().virtual_cameras.begin(), scene_.EditableLevel().virtual_cameras.end(),
                [&](const assets::VirtualCameraDefinition& camera) { return camera.id == candidate; });
        });
        clone.name = unique_name(clone.name, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().virtual_cameras.begin(), scene_.EditableLevel().virtual_cameras.end(),
                [&](const assets::VirtualCameraDefinition& camera) { return camera.name == candidate; });
        });
        clone.position += duplicate_offset;
        scene_.EditableLevel().virtual_cameras.push_back(std::move(clone));
        new_selection.push_back({SelectionKind::VirtualCamera, static_cast<int>(scene_.EditableLevel().virtual_cameras.size() - 1)});
    }
    for (auto clone : selection_clipboard_.scene_animations) {
        clone.id = unique_name(clone.id, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().animations.begin(), scene_.EditableLevel().animations.end(),
                [&](const assets::SceneAnimationDefinition& animation) { return animation.id == candidate; });
        });
        clone.name = unique_name(clone.name, [&](const std::string& candidate) {
            return std::any_of(scene_.EditableLevel().animations.begin(), scene_.EditableLevel().animations.end(),
                [&](const assets::SceneAnimationDefinition& animation) { return animation.name == candidate; });
        });
        scene_.EditableLevel().animations.push_back(std::move(clone));
        new_selection.push_back({SelectionKind::SceneAnimation, static_cast<int>(scene_.EditableLevel().animations.size() - 1)});
    }

    if (!new_selection.empty()) {
        ApplyHierarchySelection(new_selection);
        scene_.ResetSimulation(false);
        status_message_ = "Pasted " + std::to_string(new_selection.size()) + " item(s).";
    } else {
        status_message_ = "Clipboard had no supported scene items.";
    }
}

void EditorApp::DuplicateHierarchySelection() {
    PruneHierarchySelection();
    if (hierarchy_selection_.empty()) {
        return;
    }
    pending_history_label_ = "Duplicate selection";

    auto unique_name = [](const std::string& base, const auto& exists_fn) {
        std::string candidate = base.empty() ? "item_copy" : base + "_copy";
        int suffix = 1;
        while (exists_fn(candidate)) {
            candidate = (base.empty() ? "item" : base) + "_copy_" + std::to_string(suffix++);
        }
        return candidate;
    };

    const glm::vec2 duplicate_offset{
        static_cast<float>(std::max(scene_.Level().tile_width, 16)),
        static_cast<float>(std::max(scene_.Level().tile_height, 16))
    };
    std::vector<HierarchySelectionItem> new_selection;

    for (const auto& item : hierarchy_selection_) {
        if (item.kind == SelectionKind::Entity &&
            item.index >= 0 &&
            item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            auto clone = scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)];
            clone.id = unique_name(clone.id, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().entities.begin(), scene_.EditableLevel().entities.end(),
                    [&](const assets::EntityDefinition& entity) { return entity.id == candidate; });
            });
            clone.position += duplicate_offset;
            scene_.EditableLevel().entities.push_back(std::move(clone));
            new_selection.push_back({SelectionKind::Entity, static_cast<int>(scene_.EditableLevel().entities.size() - 1)});
        } else if (item.kind == SelectionKind::Trigger &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().triggers.size())) {
            auto clone = scene_.EditableLevel().triggers[static_cast<std::size_t>(item.index)];
            clone.name = unique_name(clone.name, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().triggers.begin(), scene_.EditableLevel().triggers.end(),
                    [&](const assets::TriggerZone& trigger) { return trigger.name == candidate; });
            });
            clone.position += duplicate_offset;
            clone.fired = false;
            scene_.EditableLevel().triggers.push_back(std::move(clone));
            new_selection.push_back({SelectionKind::Trigger, static_cast<int>(scene_.EditableLevel().triggers.size() - 1)});
        } else if (item.kind == SelectionKind::Parallax &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
            auto clone = scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(item.index)];
            clone.name = unique_name(clone.name, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().parallax_layers.begin(), scene_.EditableLevel().parallax_layers.end(),
                    [&](const assets::ParallaxLayer& layer) { return layer.name == candidate; });
            });
            clone.offset += duplicate_offset;
            scene_.EditableLevel().parallax_layers.push_back(std::move(clone));
            new_selection.push_back({SelectionKind::Parallax, static_cast<int>(scene_.EditableLevel().parallax_layers.size() - 1)});
        } else if (item.kind == SelectionKind::Light &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().lights.size())) {
            auto clone = scene_.EditableLevel().lights[static_cast<std::size_t>(item.index)];
            clone.name = unique_name(clone.name, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().lights.begin(), scene_.EditableLevel().lights.end(),
                    [&](const assets::LightDefinition& light) { return light.name == candidate; });
            });
            clone.position += duplicate_offset;
            scene_.EditableLevel().lights.push_back(std::move(clone));
            new_selection.push_back({SelectionKind::Light, static_cast<int>(scene_.EditableLevel().lights.size() - 1)});
        } else if (item.kind == SelectionKind::AudioSource &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
            auto clone = scene_.EditableLevel().audio_sources[static_cast<std::size_t>(item.index)];
            clone.id = unique_name(clone.id, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().audio_sources.begin(), scene_.EditableLevel().audio_sources.end(),
                    [&](const assets::AudioSourceDefinition& source) { return source.id == candidate; });
            });
            clone.name = unique_name(clone.name, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().audio_sources.begin(), scene_.EditableLevel().audio_sources.end(),
                    [&](const assets::AudioSourceDefinition& source) { return source.name == candidate; });
            });
            clone.position += duplicate_offset;
            scene_.EditableLevel().audio_sources.push_back(std::move(clone));
            new_selection.push_back({SelectionKind::AudioSource, static_cast<int>(scene_.EditableLevel().audio_sources.size() - 1)});
        } else if (item.kind == SelectionKind::AudioPak &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
            auto clone = scene_.EditableLevel().audio_paks[static_cast<std::size_t>(item.index)];
            clone.id = unique_name(clone.id, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().audio_paks.begin(), scene_.EditableLevel().audio_paks.end(),
                    [&](const assets::AudioPakDefinition& pak) { return pak.id == candidate; });
            });
            clone.name = unique_name(clone.name, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().audio_paks.begin(), scene_.EditableLevel().audio_paks.end(),
                    [&](const assets::AudioPakDefinition& pak) { return pak.name == candidate; });
            });
            clone.position += duplicate_offset;
            scene_.EditableLevel().audio_paks.push_back(std::move(clone));
            new_selection.push_back({SelectionKind::AudioPak, static_cast<int>(scene_.EditableLevel().audio_paks.size() - 1)});
        } else if (item.kind == SelectionKind::VirtualCamera &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
            auto clone = scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(item.index)];
            clone.id = unique_name(clone.id, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().virtual_cameras.begin(), scene_.EditableLevel().virtual_cameras.end(),
                    [&](const assets::VirtualCameraDefinition& camera) { return camera.id == candidate; });
            });
            clone.name = unique_name(clone.name, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().virtual_cameras.begin(), scene_.EditableLevel().virtual_cameras.end(),
                    [&](const assets::VirtualCameraDefinition& camera) { return camera.name == candidate; });
            });
            clone.position += duplicate_offset;
            scene_.EditableLevel().virtual_cameras.push_back(std::move(clone));
            new_selection.push_back({SelectionKind::VirtualCamera, static_cast<int>(scene_.EditableLevel().virtual_cameras.size() - 1)});
        } else if (item.kind == SelectionKind::SceneAnimation &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().animations.size())) {
            auto clone = scene_.EditableLevel().animations[static_cast<std::size_t>(item.index)];
            clone.id = unique_name(clone.id, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().animations.begin(), scene_.EditableLevel().animations.end(),
                    [&](const assets::SceneAnimationDefinition& animation) { return animation.id == candidate; });
            });
            clone.name = unique_name(clone.name, [&](const std::string& candidate) {
                return std::any_of(scene_.EditableLevel().animations.begin(), scene_.EditableLevel().animations.end(),
                    [&](const assets::SceneAnimationDefinition& animation) { return animation.name == candidate; });
            });
            scene_.EditableLevel().animations.push_back(std::move(clone));
            new_selection.push_back({SelectionKind::SceneAnimation, static_cast<int>(scene_.EditableLevel().animations.size() - 1)});
        }
    }

    if (!new_selection.empty()) {
        hierarchy_selection_ = std::move(new_selection);
        const auto active = hierarchy_selection_.back();
        selection_kind_ = active.kind;
        selection_index_ = active.index;
        status_message_ = "Duplicated " + std::to_string(hierarchy_selection_.size()) + " item(s).";
        scene_.ResetSimulation(false);
    }
}

void EditorApp::DeleteHierarchySelection() {
    PruneHierarchySelection();
    if (hierarchy_selection_.empty()) {
        return;
    }
    pending_history_label_ = "Delete selection";

    auto delete_kind = [&](SelectionKind kind, auto& items) {
        std::vector<int> indices;
        for (const auto& selection : hierarchy_selection_) {
            if (selection.kind == kind) {
                indices.push_back(selection.index);
            }
        }
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        for (int index : indices) {
            if (index >= 0 && index < static_cast<int>(items.size())) {
                items.erase(items.begin() + index);
            }
        }
    };

    delete_kind(SelectionKind::Entity, scene_.EditableLevel().entities);
    delete_kind(SelectionKind::Trigger, scene_.EditableLevel().triggers);
    delete_kind(SelectionKind::Parallax, scene_.EditableLevel().parallax_layers);
    delete_kind(SelectionKind::Light, scene_.EditableLevel().lights);
    delete_kind(SelectionKind::AudioSource, scene_.EditableLevel().audio_sources);
    delete_kind(SelectionKind::AudioPak, scene_.EditableLevel().audio_paks);
    delete_kind(SelectionKind::VirtualCamera, scene_.EditableLevel().virtual_cameras);
    delete_kind(SelectionKind::SceneAnimation, scene_.EditableLevel().animations);

    const std::size_t deleted_count = hierarchy_selection_.size();
    ClearHierarchySelection();
    selection_kind_ = SelectionKind::None;
    selection_index_ = -1;
    scene_.ResetSimulation(false);
    status_message_ = "Deleted " + std::to_string(deleted_count) + " item(s).";
}

void EditorApp::RenameHierarchySelection() {
    PruneHierarchySelection();
    if (hierarchy_selection_.empty() || hierarchy_name_buffer_.empty()) {
        return;
    }
    pending_history_label_ = hierarchy_selection_.size() == 1 ? "Rename item" : "Rename folder";

    if (hierarchy_selection_.size() == 1) {
        const auto item = hierarchy_selection_.front();
        if (item.kind == SelectionKind::Entity &&
            item.index >= 0 &&
            item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)].id = hierarchy_name_buffer_;
        } else if (item.kind == SelectionKind::Trigger &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().triggers.size())) {
            scene_.EditableLevel().triggers[static_cast<std::size_t>(item.index)].name = hierarchy_name_buffer_;
        } else if (item.kind == SelectionKind::Parallax &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
            scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(item.index)].name = hierarchy_name_buffer_;
        } else if (item.kind == SelectionKind::Light &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().lights.size())) {
            scene_.EditableLevel().lights[static_cast<std::size_t>(item.index)].name = hierarchy_name_buffer_;
        } else if (item.kind == SelectionKind::AudioSource &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
            scene_.EditableLevel().audio_sources[static_cast<std::size_t>(item.index)].name = hierarchy_name_buffer_;
        } else if (item.kind == SelectionKind::AudioPak &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
            scene_.EditableLevel().audio_paks[static_cast<std::size_t>(item.index)].name = hierarchy_name_buffer_;
        } else if (item.kind == SelectionKind::VirtualCamera &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
            scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(item.index)].name = hierarchy_name_buffer_;
        } else if (item.kind == SelectionKind::SceneAnimation &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().animations.size())) {
            scene_.EditableLevel().animations[static_cast<std::size_t>(item.index)].name = hierarchy_name_buffer_;
        }
        status_message_ = "Renamed selected item.";
    } else {
        const std::string group_name = SanitizeName(hierarchy_name_buffer_);
        for (const auto& item : hierarchy_selection_) {
            if (item.kind == SelectionKind::Entity &&
                item.index >= 0 &&
                item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
                scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)].editor_group = group_name;
            } else if (item.kind == SelectionKind::Trigger &&
                       item.index >= 0 &&
                       item.index < static_cast<int>(scene_.EditableLevel().triggers.size())) {
                scene_.EditableLevel().triggers[static_cast<std::size_t>(item.index)].editor_group = group_name;
            } else if (item.kind == SelectionKind::Parallax &&
                       item.index >= 0 &&
                       item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
                scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(item.index)].editor_group = group_name;
            } else if (item.kind == SelectionKind::Light &&
                       item.index >= 0 &&
                       item.index < static_cast<int>(scene_.EditableLevel().lights.size())) {
                scene_.EditableLevel().lights[static_cast<std::size_t>(item.index)].editor_group = group_name;
            } else if (item.kind == SelectionKind::AudioSource &&
                       item.index >= 0 &&
                       item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
                scene_.EditableLevel().audio_sources[static_cast<std::size_t>(item.index)].editor_group = group_name;
            } else if (item.kind == SelectionKind::AudioPak &&
                       item.index >= 0 &&
                       item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
                scene_.EditableLevel().audio_paks[static_cast<std::size_t>(item.index)].editor_group = group_name;
            } else if (item.kind == SelectionKind::VirtualCamera &&
                       item.index >= 0 &&
                       item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
                scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(item.index)].editor_group = group_name;
            } else if (item.kind == SelectionKind::SceneAnimation &&
                       item.index >= 0 &&
                       item.index < static_cast<int>(scene_.EditableLevel().animations.size())) {
                scene_.EditableLevel().animations[static_cast<std::size_t>(item.index)].editor_group = group_name;
            }
        }
        status_message_ = "Updated selection group: " + group_name;
    }
    scene_.ResetSimulation(false);
}

void EditorApp::CombineHierarchySelection() {
    PruneHierarchySelection();
    if (hierarchy_selection_.size() < 2) {
        return;
    }
    pending_history_label_ = "Group selection";
    const std::string group_name = SanitizeName(hierarchy_name_buffer_.empty() ? "group" : hierarchy_name_buffer_);
    for (const auto& item : hierarchy_selection_) {
        if (item.kind == SelectionKind::Entity &&
            item.index >= 0 &&
            item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            scene_.EditableLevel().entities[static_cast<std::size_t>(item.index)].editor_group = group_name;
        } else if (item.kind == SelectionKind::Trigger &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().triggers.size())) {
            scene_.EditableLevel().triggers[static_cast<std::size_t>(item.index)].editor_group = group_name;
        } else if (item.kind == SelectionKind::Parallax &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
            scene_.EditableLevel().parallax_layers[static_cast<std::size_t>(item.index)].editor_group = group_name;
        } else if (item.kind == SelectionKind::Light &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().lights.size())) {
            scene_.EditableLevel().lights[static_cast<std::size_t>(item.index)].editor_group = group_name;
        } else if (item.kind == SelectionKind::AudioSource &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
            scene_.EditableLevel().audio_sources[static_cast<std::size_t>(item.index)].editor_group = group_name;
        } else if (item.kind == SelectionKind::AudioPak &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
            scene_.EditableLevel().audio_paks[static_cast<std::size_t>(item.index)].editor_group = group_name;
        } else if (item.kind == SelectionKind::VirtualCamera &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
            scene_.EditableLevel().virtual_cameras[static_cast<std::size_t>(item.index)].editor_group = group_name;
        } else if (item.kind == SelectionKind::SceneAnimation &&
                   item.index >= 0 &&
                   item.index < static_cast<int>(scene_.EditableLevel().animations.size())) {
            scene_.EditableLevel().animations[static_cast<std::size_t>(item.index)].editor_group = group_name;
        }
    }
    scene_.ResetSimulation(false);
    status_message_ = "Combined " + std::to_string(hierarchy_selection_.size()) + " item(s) into group: " + group_name;
}

void EditorApp::MergeHierarchySelectionToTexture() {
    PruneHierarchySelection();
    if (hierarchy_selection_.size() < 2) {
        return;
    }

    std::vector<int> entity_indices;
    entity_indices.reserve(hierarchy_selection_.size());
    for (const auto& item : hierarchy_selection_) {
        if (item.kind != SelectionKind::Entity) {
            status_message_ = "Merge to image works only for entity selections.";
            return;
        }
        if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            entity_indices.push_back(item.index);
        }
    }
    if (entity_indices.size() < 2) {
        return;
    }

    std::sort(entity_indices.begin(), entity_indices.end());
    entity_indices.erase(std::unique(entity_indices.begin(), entity_indices.end()), entity_indices.end());

    scene_.ResetSimulation(false);
    const auto& runtime_entities = scene_.RuntimeEntities();
    auto& entities = scene_.EditableLevel().entities;

    struct MergeSource {
        int index = -1;
        glm::vec2 position{0.0f, 0.0f};
        glm::vec2 size{32.0f, 32.0f};
        glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
        std::string texture;
        int layer = 0;
        bool collidable = false;
        bool dynamic = false;
        float reflection = 0.0f;
        bool relief_enabled = false;
        float bump_strength = 1.0f;
        float relief_depth = 0.035f;
        float parallax_depth = 0.018f;
        float relief_contrast = 1.35f;
        bool pseudo_3d = false;
        float pseudo_3d_height = 16.0f;
        std::string editor_group;
    };

    std::vector<MergeSource> sources;
    sources.reserve(entity_indices.size());
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();

    for (int index : entity_indices) {
        if (index < 0 || index >= static_cast<int>(entities.size())) {
            continue;
        }
        const auto& definition = entities[static_cast<std::size_t>(index)];
        const entities::Entity* runtime = index < static_cast<int>(runtime_entities.size())
            ? &runtime_entities[static_cast<std::size_t>(index)]
            : nullptr;
        const glm::vec2 position = definition.position;
        const glm::vec2 size = {
            std::max(definition.size.x, 1.0f),
            std::max(definition.size.y, 1.0f)
        };
        MergeSource source;
        source.index = index;
        source.position = position;
        source.size = size;
        source.tint = runtime != nullptr ? runtime->tint : definition.tint;
        source.uv = runtime != nullptr ? runtime->uv : definition.uv;
        source.texture = runtime != nullptr ? runtime->texture : definition.texture;
        source.layer = definition.layer;
        source.collidable = definition.collidable;
        source.dynamic = definition.dynamic;
        source.reflection = definition.reflection;
        source.relief_enabled = definition.relief_enabled;
        source.bump_strength = definition.bump_strength;
        source.relief_depth = definition.relief_depth;
        source.parallax_depth = definition.parallax_depth;
        source.relief_contrast = definition.relief_contrast;
        source.pseudo_3d = definition.pseudo_3d;
        source.pseudo_3d_height = definition.pseudo_3d_height;
        source.editor_group = definition.editor_group;
        sources.push_back(std::move(source));
        min_x = std::min(min_x, position.x);
        min_y = std::min(min_y, position.y);
        max_x = std::max(max_x, position.x + size.x);
        max_y = std::max(max_y, position.y + size.y);
    }

    if (sources.size() < 2 ||
        !std::isfinite(min_x) ||
        !std::isfinite(min_y) ||
        !std::isfinite(max_x) ||
        !std::isfinite(max_y)) {
        status_message_ = "Unable to merge selection to image.";
        return;
    }

    std::sort(sources.begin(), sources.end(), [](const MergeSource& lhs, const MergeSource& rhs) {
        if (lhs.layer != rhs.layer) {
            return lhs.layer < rhs.layer;
        }
        if (std::abs(lhs.position.y - rhs.position.y) > 0.01f) {
            return lhs.position.y < rhs.position.y;
        }
        return lhs.index < rhs.index;
    });

    const int canvas_width = std::max(1, static_cast<int>(std::ceil(max_x - min_x)));
    const int canvas_height = std::max(1, static_cast<int>(std::ceil(max_y - min_y)));
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(canvas_width) * static_cast<std::size_t>(canvas_height) * 4, 0);

    auto blend_pixel = [](std::uint8_t* dst, const glm::u8vec4& src) {
        const float src_alpha = static_cast<float>(src.a) / 255.0f;
        const float dst_alpha = static_cast<float>(dst[3]) / 255.0f;
        const float out_alpha = src_alpha + dst_alpha * (1.0f - src_alpha);
        if (out_alpha <= 0.0001f) {
            dst[0] = 0;
            dst[1] = 0;
            dst[2] = 0;
            dst[3] = 0;
            return;
        }
        const auto blend_channel = [&](int channel) {
            const float src_value = static_cast<float>(src[channel]) / 255.0f;
            const float dst_value = static_cast<float>(dst[channel]) / 255.0f;
            const float out_value = (src_value * src_alpha + dst_value * dst_alpha * (1.0f - src_alpha)) / out_alpha;
            dst[channel] = static_cast<std::uint8_t>(std::clamp(out_value, 0.0f, 1.0f) * 255.0f);
        };
        blend_channel(0);
        blend_channel(1);
        blend_channel(2);
        dst[3] = static_cast<std::uint8_t>(std::clamp(out_alpha, 0.0f, 1.0f) * 255.0f);
    };

    int drawn_sources = 0;
    for (const auto& source : sources) {
        if (source.texture.empty()) {
            continue;
        }
        const std::filesystem::path absolute_texture = project_root_ / source.texture;
        int image_width = 0;
        int image_height = 0;
        int channels = 0;
        stbi_uc* raw = stbi_load(absolute_texture.string().c_str(), &image_width, &image_height, &channels, 4);
        if (raw == nullptr || image_width <= 0 || image_height <= 0) {
            if (raw != nullptr) {
                stbi_image_free(raw);
            }
            continue;
        }

        const float uv_min_x = std::clamp(std::min(source.uv.x, source.uv.z), 0.0f, 1.0f);
        const float uv_min_y = std::clamp(std::min(source.uv.y, source.uv.w), 0.0f, 1.0f);
        const float uv_max_x = std::clamp(std::max(source.uv.x, source.uv.z), 0.0f, 1.0f);
        const float uv_max_y = std::clamp(std::max(source.uv.y, source.uv.w), 0.0f, 1.0f);
        const int src_x0 = std::clamp(static_cast<int>(std::floor(uv_min_x * image_width)), 0, image_width - 1);
        const int src_y0 = std::clamp(static_cast<int>(std::floor(uv_min_y * image_height)), 0, image_height - 1);
        const int src_x1 = std::clamp(static_cast<int>(std::ceil(uv_max_x * image_width)), src_x0 + 1, image_width);
        const int src_y1 = std::clamp(static_cast<int>(std::ceil(uv_max_y * image_height)), src_y0 + 1, image_height);
        const int src_width = std::max(1, src_x1 - src_x0);
        const int src_height = std::max(1, src_y1 - src_y0);
        const int dst_x0 = static_cast<int>(std::lround(source.position.x - min_x));
        const int dst_y0 = static_cast<int>(std::lround(source.position.y - min_y));
        const int dst_width = std::max(1, static_cast<int>(std::lround(source.size.x)));
        const int dst_height = std::max(1, static_cast<int>(std::lround(source.size.y)));

        for (int y = 0; y < dst_height; ++y) {
            const int canvas_y = dst_y0 + y;
            if (canvas_y < 0 || canvas_y >= canvas_height) {
                continue;
            }
            const int sample_y = src_y0 + std::clamp((y * src_height) / dst_height, 0, src_height - 1);
            for (int x = 0; x < dst_width; ++x) {
                const int canvas_x = dst_x0 + x;
                if (canvas_x < 0 || canvas_x >= canvas_width) {
                    continue;
                }
                const int sample_x = src_x0 + std::clamp((x * src_width) / dst_width, 0, src_width - 1);
                const std::size_t source_offset =
                    (static_cast<std::size_t>(sample_y) * static_cast<std::size_t>(image_width) +
                     static_cast<std::size_t>(sample_x)) * 4;
                glm::u8vec4 color{
                    raw[source_offset + 0],
                    raw[source_offset + 1],
                    raw[source_offset + 2],
                    raw[source_offset + 3]
                };
                color.r = static_cast<std::uint8_t>(std::clamp((static_cast<float>(color.r) / 255.0f) * source.tint.r, 0.0f, 1.0f) * 255.0f);
                color.g = static_cast<std::uint8_t>(std::clamp((static_cast<float>(color.g) / 255.0f) * source.tint.g, 0.0f, 1.0f) * 255.0f);
                color.b = static_cast<std::uint8_t>(std::clamp((static_cast<float>(color.b) / 255.0f) * source.tint.b, 0.0f, 1.0f) * 255.0f);
                color.a = static_cast<std::uint8_t>(std::clamp((static_cast<float>(color.a) / 255.0f) * source.tint.a, 0.0f, 1.0f) * 255.0f);
                std::uint8_t* destination = &pixels[(static_cast<std::size_t>(canvas_y) * static_cast<std::size_t>(canvas_width) +
                                                      static_cast<std::size_t>(canvas_x)) * 4];
                blend_pixel(destination, color);
            }
        }

        ++drawn_sources;
        stbi_image_free(raw);
    }

    if (drawn_sources == 0) {
        status_message_ = "Selected entities have no readable textures to merge.";
        return;
    }

    const std::string base_name = !entities[static_cast<std::size_t>(entity_indices.front())].id.empty()
        ? entities[static_cast<std::size_t>(entity_indices.front())].id + "_merged"
        : "merged_selection";
    const std::string merged_texture = MakeUniqueRelativePath("assets/images/generated", base_name, ".png");
    std::filesystem::create_directories((project_root_ / std::filesystem::path(merged_texture)).parent_path());
    if (stbi_write_png((project_root_ / std::filesystem::path(merged_texture)).string().c_str(),
                       canvas_width,
                       canvas_height,
                       4,
                       pixels.data(),
                       canvas_width * 4) == 0) {
        status_message_ = "Failed to write merged image.";
        return;
    }

    assets::EntityDefinition merged_entity;
    merged_entity.id = SanitizeName(base_name);
    merged_entity.position = {min_x, min_y};
    merged_entity.size = {static_cast<float>(canvas_width), static_cast<float>(canvas_height)};
    merged_entity.texture = merged_texture;
    merged_entity.uv = {0.0f, 0.0f, 1.0f, 1.0f};
    merged_entity.layer = sources.front().layer;
    merged_entity.dynamic = std::all_of(sources.begin(), sources.end(), [](const MergeSource& source) { return source.dynamic; });
    merged_entity.collidable = std::any_of(sources.begin(), sources.end(), [](const MergeSource& source) { return source.collidable; });
    merged_entity.visible = true;
    merged_entity.reflection = std::accumulate(sources.begin(), sources.end(), 0.0f,
        [](float value, const MergeSource& source) { return std::max(value, source.reflection); });
    merged_entity.normal_map.clear();
    merged_entity.height_map.clear();
    merged_entity.displacement_map.clear();
    merged_entity.relief_enabled = false;
    merged_entity.bump_strength = std::accumulate(sources.begin(), sources.end(), 0.0f,
        [](float value, const MergeSource& source) { return std::max(value, source.bump_strength); });
    merged_entity.relief_depth = std::accumulate(sources.begin(), sources.end(), 0.0f,
        [](float value, const MergeSource& source) { return std::max(value, source.relief_depth); });
    merged_entity.parallax_depth = std::accumulate(sources.begin(), sources.end(), 0.0f,
        [](float value, const MergeSource& source) { return std::max(value, source.parallax_depth); });
    merged_entity.relief_contrast = std::accumulate(sources.begin(), sources.end(), 0.2f,
        [](float value, const MergeSource& source) { return std::max(value, source.relief_contrast); });
    merged_entity.pseudo_3d = std::any_of(sources.begin(), sources.end(), [](const MergeSource& source) { return source.pseudo_3d; });
    merged_entity.pseudo_3d_height = std::accumulate(sources.begin(), sources.end(), 0.0f,
        [](float value, const MergeSource& source) { return std::max(value, source.pseudo_3d_height); });
    merged_entity.collider_offset = {0.0f, 0.0f};
    merged_entity.collider_size = merged_entity.size;
    merged_entity.editor_group = sources.front().editor_group;

    for (auto it = entity_indices.rbegin(); it != entity_indices.rend(); ++it) {
        entities.erase(entities.begin() + *it);
    }
    entities.push_back(std::move(merged_entity));
    const int merged_index = static_cast<int>(entities.size() - 1);

    pending_history_label_ = "Merge selection to image";
    SetHierarchySelectionSingle(SelectionKind::Entity, merged_index);
    scene_.ResetSimulation(false);
    status_message_ = "Merged selection into " + merged_texture;
}

void EditorApp::RemoveHierarchySelectionGaps() {
    PruneHierarchySelection();
    if (hierarchy_selection_.size() < 2) {
        return;
    }

    std::vector<int> entity_indices;
    entity_indices.reserve(hierarchy_selection_.size());
    for (const auto& item : hierarchy_selection_) {
        if (item.kind != SelectionKind::Entity) {
            status_message_ = "Remove gaps works only for entity selections.";
            return;
        }
        if (item.index >= 0 && item.index < static_cast<int>(scene_.EditableLevel().entities.size())) {
            entity_indices.push_back(item.index);
        }
    }
    if (entity_indices.size() < 2) {
        return;
    }

    std::sort(entity_indices.begin(), entity_indices.end(), [&](int lhs, int rhs) {
        const auto& a = scene_.EditableLevel().entities[static_cast<std::size_t>(lhs)];
        const auto& b = scene_.EditableLevel().entities[static_cast<std::size_t>(rhs)];
        if (std::abs(a.position.y - b.position.y) > 0.01f) {
            return a.position.y < b.position.y;
        }
        return a.position.x < b.position.x;
    });
    entity_indices.erase(std::unique(entity_indices.begin(), entity_indices.end()), entity_indices.end());

    auto& entities = scene_.EditableLevel().entities;
    float min_y = std::numeric_limits<float>::max();
    float min_height = std::numeric_limits<float>::max();
    for (int index : entity_indices) {
        const auto& entity = entities[static_cast<std::size_t>(index)];
        min_y = std::min(min_y, entity.position.y);
        min_height = std::min(min_height, std::max(entity.size.y, 1.0f));
    }
    const float row_tolerance = std::max(2.0f, min_height * 0.45f);

    struct RowGroup {
        float y = 0.0f;
        std::vector<int> indices;
    };

    std::vector<RowGroup> rows;
    for (int index : entity_indices) {
        const auto& entity = entities[static_cast<std::size_t>(index)];
        if (rows.empty() || std::abs(entity.position.y - rows.back().y) > row_tolerance) {
            rows.push_back({entity.position.y, {index}});
        } else {
            rows.back().indices.push_back(index);
        }
    }

    float current_y = min_y;
    for (auto& row : rows) {
        std::sort(row.indices.begin(), row.indices.end(), [&](int lhs, int rhs) {
            return entities[static_cast<std::size_t>(lhs)].position.x < entities[static_cast<std::size_t>(rhs)].position.x;
        });
        float current_x = std::numeric_limits<float>::max();
        float row_height = 1.0f;
        for (int index : row.indices) {
            current_x = std::min(current_x, entities[static_cast<std::size_t>(index)].position.x);
            row_height = std::max(row_height, std::max(entities[static_cast<std::size_t>(index)].size.y, 1.0f));
        }
        if (!std::isfinite(current_x)) {
            current_x = 0.0f;
        }
        for (int index : row.indices) {
            auto& entity = entities[static_cast<std::size_t>(index)];
            entity.position = {current_x, current_y};
            current_x += std::max(entity.size.x, 1.0f);
        }
        current_y += row_height;
    }

    pending_history_label_ = "Remove selection gaps";
    scene_.ResetSimulation(false);
    status_message_ = "Removed gaps in selected blocks.";
}

void EditorApp::RecordHistorySnapshot(const std::string& label, bool force) {
    if (!project_loaded_) {
        return;
    }

    const std::string snapshot = assets::SaveLevelToText(scene_.EditableLevel());
    if (!force && snapshot == last_history_snapshot_) {
        return;
    }

    if (history_cursor_ + 1 < static_cast<int>(history_entries_.size())) {
        history_entries_.erase(history_entries_.begin() + history_cursor_ + 1, history_entries_.end());
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t current_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &current_time);
#else
    localtime_r(&current_time, &local_time);
#endif
    std::ostringstream timestamp_stream;
    timestamp_stream << std::put_time(&local_time, "%H:%M:%S");

    HistoryEntry entry;
    entry.label = label.empty() ? "Scene edit" : label;
    entry.timestamp = timestamp_stream.str();
    entry.level_snapshot = snapshot;
    history_entries_.push_back(std::move(entry));
    history_cursor_ = static_cast<int>(history_entries_.size() - 1);
    last_history_snapshot_ = snapshot;
    history_last_commit_time_ = elapsed_time_;

    if (history_entries_.size() > 256) {
        history_entries_.erase(history_entries_.begin());
        history_cursor_ = static_cast<int>(history_entries_.size() - 1);
    }
}

void EditorApp::SyncHistorySnapshot() {
    if (!project_loaded_ || suppress_history_capture_) {
        return;
    }

    const std::string snapshot = assets::SaveLevelToText(scene_.EditableLevel());
    if (snapshot == last_history_snapshot_) {
        return;
    }

    const std::string label = pending_history_label_.empty() ? "Scene edit" : pending_history_label_;
    if (history_cursor_ >= 0 &&
        history_cursor_ == static_cast<int>(history_entries_.size() - 1) &&
        elapsed_time_ - history_last_commit_time_ < 0.35f &&
        history_entries_[static_cast<std::size_t>(history_cursor_)].label == label) {
        history_entries_[static_cast<std::size_t>(history_cursor_)].level_snapshot = snapshot;
        last_history_snapshot_ = snapshot;
        history_last_commit_time_ = elapsed_time_;
        return;
    }

    RecordHistorySnapshot(label, true);
    pending_history_label_ = "Scene edit";
}

void EditorApp::RestoreHistoryIndex(int index) {
    if (index < 0 || index >= static_cast<int>(history_entries_.size())) {
        return;
    }

    suppress_history_capture_ = true;
    scene_.EditableLevel() = assets::LoadLevelFromText(history_entries_[static_cast<std::size_t>(index)].level_snapshot, scene_.LevelPath());
    scene_.ResetSimulation(false);
    history_cursor_ = index;
    last_history_snapshot_ = history_entries_[static_cast<std::size_t>(index)].level_snapshot;
    suppress_history_capture_ = false;
    status_message_ = "Restored history: " + history_entries_[static_cast<std::size_t>(index)].label;
}

assets::MenuDefinition& EditorApp::EditedMenuDefinition() {
    return menu_edit_target_ == MenuEditTarget::Main ? project_.main_menu : project_.pause_menu;
}

const assets::MenuDefinition& EditorApp::EditedMenuDefinition() const {
    return menu_edit_target_ == MenuEditTarget::Main ? project_.main_menu : project_.pause_menu;
}

void EditorApp::DeleteSelection() {
    PruneHierarchySelection();
    if (hierarchy_selection_.size() > 1) {
        DeleteHierarchySelection();
        return;
    }
    if (selection_kind_ == SelectionKind::Entity &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().entities.size())) {
        pending_history_label_ = "Delete entity";
        scene_.EditableLevel().entities.erase(scene_.EditableLevel().entities.begin() + selection_index_);
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        scene_.ResetSimulation(false);
        status_message_ = "Deleted entity.";
        return;
    }
    if (selection_kind_ == SelectionKind::Trigger &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().triggers.size())) {
        pending_history_label_ = "Delete trigger";
        scene_.EditableLevel().triggers.erase(scene_.EditableLevel().triggers.begin() + selection_index_);
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        scene_.ResetSimulation(false);
        status_message_ = "Deleted trigger.";
        return;
    }
    if (selection_kind_ == SelectionKind::Parallax &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().parallax_layers.size())) {
        pending_history_label_ = "Delete parallax";
        scene_.EditableLevel().parallax_layers.erase(scene_.EditableLevel().parallax_layers.begin() + selection_index_);
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        scene_.ResetSimulation(false);
        status_message_ = "Deleted parallax layer.";
        return;
    }
    if (selection_kind_ == SelectionKind::Light &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().lights.size())) {
        pending_history_label_ = "Delete light";
        scene_.EditableLevel().lights.erase(scene_.EditableLevel().lights.begin() + selection_index_);
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        scene_.ResetSimulation(false);
        status_message_ = "Deleted light.";
        return;
    }
    if (selection_kind_ == SelectionKind::AudioSource &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().audio_sources.size())) {
        pending_history_label_ = "Delete audio source";
        scene_.EditableLevel().audio_sources.erase(scene_.EditableLevel().audio_sources.begin() + selection_index_);
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        scene_.ResetSimulation(false);
        status_message_ = "Deleted audio source.";
        return;
    }
    if (selection_kind_ == SelectionKind::AudioPak &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().audio_paks.size())) {
        pending_history_label_ = "Delete audio pak";
        scene_.EditableLevel().audio_paks.erase(scene_.EditableLevel().audio_paks.begin() + selection_index_);
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        scene_.ResetSimulation(false);
        status_message_ = "Deleted audio pak.";
        return;
    }
    if (selection_kind_ == SelectionKind::VirtualCamera &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().virtual_cameras.size())) {
        pending_history_label_ = "Delete virtual camera";
        scene_.EditableLevel().virtual_cameras.erase(scene_.EditableLevel().virtual_cameras.begin() + selection_index_);
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        scene_.ResetSimulation(false);
        status_message_ = "Deleted virtual camera.";
        return;
    }
    if (selection_kind_ == SelectionKind::SceneAnimation &&
        selection_index_ >= 0 &&
        selection_index_ < static_cast<int>(scene_.EditableLevel().animations.size())) {
        pending_history_label_ = "Delete scene animation";
        scene_.EditableLevel().animations.erase(scene_.EditableLevel().animations.begin() + selection_index_);
        selection_kind_ = SelectionKind::None;
        selection_index_ = -1;
        scene_.ResetSimulation(false);
        status_message_ = "Deleted scene animation.";
    }
}

void EditorApp::LoadSpriteCanvas() {
    if (selected_sprite_.animations.empty()) {
        return;
    }

    sprite_animation_index_ = std::clamp(sprite_animation_index_, 0, static_cast<int>(selected_sprite_.animations.size()) - 1);
    auto& animation = selected_sprite_.animations[sprite_animation_index_];
    if (animation.frames.empty()) {
        animation.frames.push_back({
            MakeUniqueRelativePath("assets/images/generated", selected_sprite_.name + "_frame", ".tga"),
            0.12f
        });
        resource_dirty_ = true;
    }
    sprite_frame_index_ = std::clamp(sprite_frame_index_, 0, static_cast<int>(animation.frames.size()) - 1);
    auto& frame = animation.frames[sprite_frame_index_];
    if (frame.texture.empty()) {
        frame.texture = MakeUniqueRelativePath("assets/images/generated", selected_sprite_.name + "_frame", ".tga");
    }

    pixel_canvas_.width = std::max(selected_sprite_.canvas_size.x, 1);
    pixel_canvas_.height = std::max(selected_sprite_.canvas_size.y, 1);
    pixel_canvas_.texture_relative = frame.texture;
    pixel_canvas_.dirty = false;

    int loaded_width = 0;
    int loaded_height = 0;
    const auto full_path = project_root_ / frame.texture;
    if (full_path.extension() != ".tga") {
        pixel_canvas_.texture_relative.clear();
        int channels = 0;
        if (stbi_uc* pixels = stbi_load(full_path.string().c_str(), &loaded_width, &loaded_height, &channels, STBI_rgb_alpha); pixels != nullptr) {
            pixel_canvas_.width = loaded_width;
            pixel_canvas_.height = loaded_height;
            pixel_canvas_.pixels.assign(pixels, pixels + static_cast<std::ptrdiff_t>(loaded_width * loaded_height * 4));
            stbi_image_free(pixels);
        } else {
            pixel_canvas_.pixels = MakeBlankPixels(pixel_canvas_.width, pixel_canvas_.height);
        }
        return;
    }

    if (!ReadTga32(full_path, loaded_width, loaded_height, pixel_canvas_.pixels) ||
        loaded_width != pixel_canvas_.width || loaded_height != pixel_canvas_.height) {
        pixel_canvas_.pixels = MakeBlankPixels(pixel_canvas_.width, pixel_canvas_.height);
        WriteTga32(full_path, pixel_canvas_.width, pixel_canvas_.height, pixel_canvas_.pixels);
    }
}

void EditorApp::SaveSpriteCanvas() {
    if (pixel_canvas_.texture_relative.empty() || pixel_canvas_.pixels.empty()) {
        return;
    }
    WriteTga32(project_root_ / pixel_canvas_.texture_relative, pixel_canvas_.width, pixel_canvas_.height, pixel_canvas_.pixels);
    pixel_canvas_.dirty = false;
}

}  // namespace novaiso::editor



