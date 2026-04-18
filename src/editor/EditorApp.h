#pragma once

#include "assets/AssetManager.h"
#include "assets/Project.h"
#include "core/Application.h"
#include "entities/Scene.h"
#include "renderer/Framebuffer.h"
#include "renderer/PostProcessStack.h"
#include "renderer/Renderer2D.h"
#include "renderer/Texture2D.h"
#include "scripting/PythonScripting.h"

#include <TextEditor.h>
#include <glm/ext/vector_uint4.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace novaiso::editor {

class EditorApp : public core::Application {
public:
    explicit EditorApp(std::filesystem::path project_root = {});

protected:
    bool OnInit() override;
    void OnShutdown() override;
    void OnUpdate(float delta_time) override;
    void OnRender() override;
    void OnGui() override;
    void OnDropFile(const std::filesystem::path& path) override;

private:
    enum class SelectionKind {
        None,
        Entity,
        Trigger,
        Parallax,
        Light,
        AudioSource,
        AudioPak,
        VirtualCamera,
        SceneAnimation
    };

    enum class BrowserSelectionKind {
        None,
        File,
        Sprite,
        Object,
        Trigger,
        Light,
        AudioSource,
        AudioPak,
        Animation,
        Particle
    };

    enum class ViewportMode {
        Edit,
        Preview
    };

    enum class SpriteTool {
        Brush,
        Erase,
        Picker
    };

    enum class ResizeHandle {
        None,
        Left,
        Right,
        Top,
        Bottom
    };

    enum class MenuEditTarget {
        Main,
        Pause
    };

    enum class GraphNodeSelectionKind {
        None,
        Condition,
        Action
    };

    enum class AiBuildOperationKind {
        PlaceObject,
        PlaceTexture,
        AddLight,
        AddAudioSource,
        AddVirtualCamera,
        AddParallax
    };

    struct PixelCanvas {
        int width = 16;
        int height = 16;
        std::string texture_relative;
        std::vector<std::uint8_t> pixels;
        bool dirty = false;
    };

    struct ProjectLauncherEntry {
        std::filesystem::path root;
        assets::ProjectData project;
        renderer::Texture2D preview_texture;
        bool preview_loaded = false;
    };

    struct HierarchySelectionItem {
        SelectionKind kind = SelectionKind::None;
        int index = -1;
    };

    struct HistoryEntry {
        std::string label;
        std::string timestamp;
        std::string level_snapshot;
    };

    struct SelectionClipboard {
        bool has_data = false;
        std::vector<assets::EntityDefinition> entities;
        std::vector<assets::TriggerZone> triggers;
        std::vector<assets::ParallaxLayer> parallax_layers;
        std::vector<assets::LightDefinition> lights;
        std::vector<assets::AudioSourceDefinition> audio_sources;
        std::vector<assets::AudioPakDefinition> audio_paks;
        std::vector<assets::VirtualCameraDefinition> virtual_cameras;
        std::vector<assets::SceneAnimationDefinition> scene_animations;
    };

    struct DragSelectionSnapshot {
        HierarchySelectionItem item;
        glm::vec2 position{0.0f, 0.0f};
    };

    struct ToastNotification {
        std::string message;
        glm::vec4 color{0.22f, 0.70f, 0.98f, 1.0f};
        float created_at = 0.0f;
        float duration = 4.0f;
    };

    struct AiBuilderSettings {
        std::string prompt = "industrial ruins";
        int width_tiles = 48;
        int height_tiles = 18;
        int seed = 1337;
        int operations_per_second = 24;
        int zone_count = 3;
        float platform_density = 0.35f;
        float prop_density = 0.55f;
        float light_density = 0.20f;
        float richness = 0.72f;
        float verticality = 0.42f;
        float atmosphere = 0.58f;
        float ambient_intensity = 0.28f;
        float light_radius_scale = 1.15f;
        float light_intensity_scale = 1.15f;
        float camera_zoom = 1.0f;
        float camera_follow_lag = 4.5f;
        float camera_zoom_lag = 4.0f;
        float camera_dead_zone_scale = 0.22f;
        float camera_overlap = 0.16f;
        bool include_lights = true;
        bool include_audio = true;
        bool include_cameras = true;
        bool include_parallax = true;
        bool include_post_effects = true;
        bool include_raw_images = true;
        bool clear_existing_ai = true;
    };

    struct AiBuildOperation {
        AiBuildOperationKind kind = AiBuildOperationKind::PlaceObject;
        std::string resource_path;
        std::string label;
        glm::vec2 position{0.0f, 0.0f};
        glm::vec2 size{0.0f, 0.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        float radius = 0.0f;
        float intensity = 1.0f;
        float volume = 1.0f;
        float zoom = 1.0f;
        float follow_lag = 4.5f;
        float zoom_lag = 4.0f;
        float dead_zone_scale = 0.22f;
        float depth = -200.0f;
        float zoom_factor = 1.0f;
        glm::vec2 speed{0.25f, 0.0f};
        glm::vec2 scale{1.0f, 1.0f};
        glm::vec2 offset{0.0f, 0.0f};
        std::string follow_target = "player";
        bool enabled = true;
        bool auto_activate = true;
        bool release_on_exit = true;
        bool repeat = true;
    };

    struct PerformanceSnapshot {
        float fps = 0.0f;
        float frame_ms = 0.0f;
        float update_ms = 0.0f;
        float render_ms = 0.0f;
        float gui_ms = 0.0f;
        float scene_render_ms = 0.0f;
        float post_process_ms = 0.0f;
        float gpu_frame_ms = 0.0f;
        float system_cpu_percent = 0.0f;
        float process_cpu_percent = 0.0f;
        float process_cpu_core_equivalent_percent = 0.0f;
        float ram_used_mb = 0.0f;
        float ram_total_mb = 0.0f;
        float process_ram_mb = 0.0f;
        float vram_used_mb = 0.0f;
        float vram_budget_mb = 0.0f;
        std::vector<float> frame_ms_history;
        std::vector<float> fps_history;
        std::vector<float> core_cpu_usage;
    };

    void LoadProject(const std::filesystem::path& root);
    void EnsureProjectLayout() const;
    void SaveAll();
    void SyncSceneResources();
    void LoadRecentProjects();
    void SaveRecentProjects() const;
    void AddRecentProject(const std::filesystem::path& root);
    void RefreshProjectLauncherEntries();
    void DrawProjectLauncher();
    [[nodiscard]] std::filesystem::path RecentProjectsPath() const;
    [[nodiscard]] std::filesystem::path BrowseForFolder(const std::string& title) const;
    [[nodiscard]] std::filesystem::path BrowseForFile(const std::string& title) const;
    bool TryOpenProject(const std::filesystem::path& root);
    void CreateNewProject();
    [[nodiscard]] std::filesystem::path ExportPackagePath() const;
    void BuildAndRunGamePackage();
    void RunPackagedGame(const std::filesystem::path& executable) const;

    void LoadCurrentScript();
    void SaveCurrentScript();
    void LoadSelectedResource();
    void SaveSelectedResource();
    void OpenCodeEditorFile(const std::filesystem::path& path);
    void DeleteBrowserResource(BrowserSelectionKind kind, const std::string& relative_path);
    [[nodiscard]] bool IsEditableTextFile(const std::filesystem::path& path) const;
    void RefreshCodeEditorLanguage();

    void DrawHierarchyPanel();
    void DrawPropertiesPanel();
    void DrawTriggerPanel();
    void DrawLayersPanel();
    void DrawAssetBrowser();
    void DrawResourceInspector();
    void DrawProjectSettingsPanel();
    void DrawMenuDesignerPanel();
    void DrawUiEditorPanel();
    void DrawShaderPanel();
    void DrawAudioMixerPanel();
    void DrawScriptPanel();
    void DrawViewport();
    void DrawToolbar();
    void DrawAiBuilderPanel();
    void DrawPerformancePanel();
    void DrawMessagesPanel();
    void DrawSpriteEditorWindow();
    void DrawAnimationEditorWindow();
    void DrawSpriteEditor();
    void DrawHistoryPanel();
    void ConvertCurrentFrameToEditable();
    void ImportImageIntoCurrentFrame(const std::filesystem::path& source);

    void SelectBrowserResource(BrowserSelectionKind kind, const std::string& relative_path);
    void CreateSpriteResource();
    void CreateObjectResource();
    void CreateTriggerResource();
    void CreateAnimationResource();
    void CreateParticleResource();
    void CreateObjectFromTexture(const std::string& texture_path);
    void CreateSpriteFromTexture(const std::string& texture_path);
    void ExportGamePackage();

    void PlaceTextureEntity(const std::string& texture_path, glm::vec2 world_position);
    void PlaceObjectEntity(const std::string& object_path, glm::vec2 world_position);
    void PlaceTriggerZone(const std::string& trigger_path, glm::vec2 world_position);
    void PlaceLight(glm::vec2 world_position, std::string_view light_type = "point");
    void PlaceAudioSource(const std::string& audio_path, glm::vec2 world_position);
    void PlaceAudioPak(glm::vec2 world_position);
    void PlaceSceneAnimation(const std::string& animation_path, glm::vec2 world_position);
    void PlaceParticleEmitter(const std::string& particle_path, glm::vec2 world_position);
    void HandleViewportSelectionAndPlacement(glm::vec2 world_position);
    bool ViewportMouseToWorld(glm::vec2& out_world) const;
    void ApplyProjectPresentation();
    void StartAiLevelBuild();
    void CancelAiLevelBuild();
    void ClearAiGeneratedContent();
    void ApplyAiBuildOperation(const AiBuildOperation& operation);
    void EnsureLocalizationFiles() const;
    void EnsurePresetScripts() const;
    void EnsureProjectShaders() const;
    void EnsureParticleResources() const;
    void CreateDefaultHtmlUiFiles();
    void LoadEditorLocalization();
    [[nodiscard]] std::string Tr(std::string_view key, std::string_view fallback) const;
    bool RenameSelectedResourceFile();
    [[nodiscard]] assets::SpriteFrame* CurrentSpriteFrame();
    [[nodiscard]] const assets::SpriteFrame* CurrentSpriteFrame() const;

    [[nodiscard]] std::filesystem::path ActiveShaderRoot() const;
    const std::vector<std::filesystem::path>& EnumerateRelativeFiles(const std::filesystem::path& folder, const std::string& extension = {});
    void InvalidateAssetBrowserCache();
    std::string MakeUniqueRelativePath(const std::filesystem::path& folder, const std::string& stem, const std::string& extension) const;
    void EnsureProjectListContains(std::vector<std::string>& list, const std::string& value);
    [[nodiscard]] assets::MenuDefinition& EditedMenuDefinition();
    [[nodiscard]] const assets::MenuDefinition& EditedMenuDefinition() const;
    void DeleteSelection();
    void ClearHierarchySelection();
    void SetHierarchySelectionSingle(SelectionKind kind, int index);
    [[nodiscard]] bool IsHierarchyItemSelected(SelectionKind kind, int index) const;
    void PruneHierarchySelection();
    [[nodiscard]] std::string HierarchyItemLabel(SelectionKind kind, int index) const;
    void CopyHierarchySelection();
    void PasteHierarchySelection();
    void DuplicateHierarchySelection();
    void DeleteHierarchySelection();
    void RenameHierarchySelection();
    void CombineHierarchySelection();
    void MergeHierarchySelectionToTexture();
    void RemoveHierarchySelectionGaps();
    void ApplyHierarchySelection(const std::vector<HierarchySelectionItem>& items);
    void RecordHistorySnapshot(const std::string& label, bool force = false);
    void SyncHistorySnapshot();
    void RestoreHistoryIndex(int index);
    void PushToast(std::string message, glm::vec4 color, float duration = 4.0f);
    void SyncEditorNotifications();
    void DrawToastNotifications();

    void LoadSpriteCanvas();
    void SaveSpriteCanvas();

    std::filesystem::path project_root_;
    assets::ProjectData project_;
    assets::AssetManager asset_manager_;
    scripting::PythonScripting scripting_;
    entities::Scene scene_;
    renderer::Renderer2D renderer_;
    renderer::PostProcessStack post_stack_;
    renderer::Framebuffer scene_target_;
    SelectionKind selection_kind_ = SelectionKind::None;
    int selection_index_ = -1;
    BrowserSelectionKind browser_selection_kind_ = BrowserSelectionKind::None;
    std::string browser_selection_relative_;
    GLuint viewport_texture_ = 0;
    glm::ivec2 scene_render_size_{1600, 900};
    float elapsed_time_ = 0.0f;
    bool live_preview_ = true;
    bool draw_debug_ = true;
    bool script_dirty_ = false;
    bool resource_dirty_ = false;
    bool viewport_hovered_ = false;
    bool editor_audio_enabled_ = true;
    bool script_panel_hovered_ = false;
    std::string current_script_relative_;
    std::string current_code_relative_;
    std::string import_source_path_;
    std::string sprite_import_path_;
    std::string status_message_;
    std::string last_toasted_status_message_;
    std::vector<ToastNotification> toast_notifications_;
    TextEditor python_editor_;
    std::unordered_map<std::string, std::vector<std::filesystem::path>> asset_browser_file_cache_;
    bool asset_browser_cache_dirty_ = true;
    assets::SpriteAsset selected_sprite_;
    assets::ObjectAsset selected_object_;
    assets::TriggerAsset selected_trigger_;
    assets::ObjectAnimationAsset selected_animation_;
    assets::ParticleEffectAsset selected_particle_;
    int active_tile_layer_index_ = 0;
    int selected_tileset_index_ = -1;
    bool tile_paint_mode_ = false;
    int sprite_animation_index_ = 0;
    int sprite_frame_index_ = 0;
    int animation_path_drag_index_ = -1;
    PixelCanvas pixel_canvas_;
    glm::vec4 paint_color_{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec2 viewport_image_min_{0.0f, 0.0f};
    glm::vec2 viewport_image_size_{1.0f, 1.0f};
    nlohmann::json editor_locale_ = nlohmann::json::object();
    bool project_loaded_ = false;
    std::vector<std::filesystem::path> recent_projects_;
    std::vector<ProjectLauncherEntry> project_launcher_entries_;
    std::string launcher_open_path_;
    std::string launcher_create_parent_;
    std::string launcher_create_name_ = "NewProject";
    BrowserSelectionKind placement_kind_ = BrowserSelectionKind::None;
    std::string placement_relative_;
    ViewportMode viewport_mode_ = ViewportMode::Edit;
    bool snap_to_grid_ = true;
    bool dragging_selection_ = false;
    bool resizing_selection_ = false;
    bool marquee_selecting_ = false;
    bool constructor_dragging_ = false;
    ResizeHandle resize_handle_ = ResizeHandle::None;
    ResizeHandle constructor_handle_ = ResizeHandle::None;
    glm::vec2 drag_grab_offset_{0.0f, 0.0f};
    glm::vec2 resize_origin_position_{0.0f, 0.0f};
    glm::vec2 resize_origin_size_{0.0f, 0.0f};
    glm::vec2 marquee_start_world_{0.0f, 0.0f};
    glm::vec2 marquee_current_world_{0.0f, 0.0f};
    glm::vec2 constructor_origin_position_{0.0f, 0.0f};
    glm::vec2 constructor_origin_size_{0.0f, 0.0f};
    std::vector<DragSelectionSnapshot> drag_selection_snapshots_;
    std::vector<HierarchySelectionItem> constructor_source_selection_;
    std::unordered_set<int> constructor_applied_steps_;
    bool panning_camera_ = false;
    glm::vec2 camera_pan_origin_mouse_{0.0f, 0.0f};
    glm::vec2 camera_pan_origin_position_{0.0f, 0.0f};
    SpriteTool sprite_tool_ = SpriteTool::Brush;
    MenuEditTarget menu_edit_target_ = MenuEditTarget::Main;
    std::unordered_set<std::string> paint_stamps_;
    bool show_menu_designer_ = true;
    bool show_ui_editor_ = true;
    bool show_shader_panel_ = true;
    bool show_audio_mixer_ = true;
    bool show_hierarchy_panel_ = true;
    bool show_properties_panel_ = true;
    bool show_trigger_panel_ = true;
    bool show_layers_panel_ = true;
    bool show_asset_browser_ = true;
    bool show_resource_inspector_ = true;
    bool show_project_settings_ = true;
    bool show_code_editor_ = true;
    bool show_viewport_ = true;
    bool show_toolbar_ = true;
    bool show_ai_builder_ = true;
    bool show_performance_panel_ = true;
    bool show_output_panel_ = true;
    bool show_sprite_editor_ = true;
    bool show_animation_editor_ = true;
    bool show_history_panel_ = true;
    bool focus_trigger_panel_ = false;
    bool open_create_project_popup_ = false;
    std::vector<HierarchySelectionItem> hierarchy_selection_;
    SelectionClipboard selection_clipboard_;
    int clipboard_paste_serial_ = 0;
    int hierarchy_anchor_flat_index_ = -1;
    bool open_hierarchy_rename_popup_ = false;
    bool open_hierarchy_combine_popup_ = false;
    std::string hierarchy_name_buffer_;
    std::vector<HistoryEntry> history_entries_;
    int history_cursor_ = -1;
    std::string last_history_snapshot_;
    std::string pending_history_label_ = "Scene edit";
    float history_last_commit_time_ = 0.0f;
    bool suppress_history_capture_ = false;
    GraphNodeSelectionKind trigger_graph_selection_kind_ = GraphNodeSelectionKind::None;
    int trigger_graph_selection_index_ = -1;
    bool dragging_trigger_graph_node_ = false;
    glm::vec2 trigger_graph_drag_offset_{0.0f, 0.0f};
    AiBuilderSettings ai_builder_settings_;
    std::vector<AiBuildOperation> ai_build_queue_;
    std::size_t ai_build_cursor_ = 0;
    float ai_build_accumulator_ = 0.0f;
    bool ai_build_running_ = false;
    std::vector<std::string> ai_generated_post_effects_;
    float build_progress_ = 0.0f;
    bool build_in_progress_ = false;
    std::vector<std::string> build_log_;
    bool preview_paused_ = false;
    bool preview_trace_enabled_ = false;
    bool preview_step_requested_ = false;
    PerformanceSnapshot performance_;
    double update_cpu_ms_ = 0.0;
    double render_cpu_ms_ = 0.0;
    double gui_cpu_ms_ = 0.0;
    GLuint gpu_time_queries_[2]{0, 0};
    bool gpu_time_query_ready_[2]{false, false};
    int gpu_time_query_index_ = 0;
};

}  // namespace novaiso::editor
