#pragma once

#include "assets/Project.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace novaiso::ui {

class HtmlUiRuntime {
public:
    struct DynamicContext {
        std::unordered_map<std::string, std::string> values;
    };

    using TextResolver = std::function<std::string(const std::string&)>;
    using ActionHandler = std::function<void(const std::string&)>;

    bool Load(const std::filesystem::path& project_root, const assets::HtmlUiSettings& settings);
    void Clear();

    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] bool HasScreen(std::string_view screen_id) const;
    void RenderScreen(std::string_view screen_id, const DynamicContext& context, const TextResolver& text_resolver, const ActionHandler& action_handler) const;

    struct Style {
        std::optional<glm::vec4> background;
        std::optional<glm::vec4> accent;
        std::optional<glm::vec4> color;
        std::optional<glm::vec4> title_color;
        std::optional<glm::vec4> subtitle_color;
        std::optional<glm::vec4> hover_background;
        std::optional<float> width;
        std::optional<float> height;
        std::optional<float> padding;
        std::optional<float> gap;
        std::optional<float> radius;
        std::optional<float> scale;
        std::optional<glm::vec2> anchor;
        std::optional<glm::vec2> position;
        std::optional<glm::vec2> size;
        std::optional<glm::vec2> panel_size;
    };

    struct Node {
        std::string tag;
        std::string id;
        std::vector<std::string> classes;
        std::unordered_map<std::string, std::string> attributes;
        std::string text;
        std::vector<std::unique_ptr<Node>> children;
    };

    struct CssRule {
        std::string selector;
        Style style;
    };

private:
    [[nodiscard]] Style ResolveStyle(const Node& node) const;

    assets::HtmlUiSettings settings_;
    Node root_;
    std::unordered_map<std::string, const Node*> screens_;
    std::unordered_map<std::string, std::string> script_globals_;
    std::unordered_map<std::string, std::string> css_variables_;
    std::vector<CssRule> css_rules_;
    bool loaded_ = false;
};

}  // namespace novaiso::ui
