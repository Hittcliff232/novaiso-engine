#include "ui/HtmlUiRuntime.h"

#include "core/FileIO.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <optional>
#include <sstream>

namespace novaiso::ui {

namespace {

std::string Trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string ToLower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string CollapseWhitespace(std::string_view value) {
    std::string result;
    bool previous_space = true;
    for (char character : value) {
        const bool is_space = std::isspace(static_cast<unsigned char>(character)) != 0;
        if (is_space) {
            if (!previous_space) {
                result.push_back(' ');
            }
        } else {
            result.push_back(character);
        }
        previous_space = is_space;
    }
    return Trim(result);
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        const std::string part = Trim(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        if (!part.empty()) {
            result.push_back(part);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::vector<std::string> SplitClasses(std::string_view value) {
    std::vector<std::string> result;
    std::string token;
    for (char character : value) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!token.empty()) {
                result.push_back(ToLower(token));
                token.clear();
            }
        } else {
            token.push_back(character);
        }
    }
    if (!token.empty()) {
        result.push_back(ToLower(token));
    }
    return result;
}

std::unordered_map<std::string, std::string> ParseAttributes(std::string_view text) {
    std::unordered_map<std::string, std::string> attributes;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text.size()) {
            break;
        }

        const std::size_t name_start = cursor;
        while (cursor < text.size()) {
            const char current = text[cursor];
            if (std::isspace(static_cast<unsigned char>(current)) != 0 || current == '=') {
                break;
            }
            ++cursor;
        }
        std::string name = ToLower(text.substr(name_start, cursor - name_start));
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }

        std::string value = "true";
        if (cursor < text.size() && text[cursor] == '=') {
            ++cursor;
            while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
                ++cursor;
            }
            if (cursor < text.size() && (text[cursor] == '"' || text[cursor] == '\'')) {
                const char quote = text[cursor++];
                const std::size_t value_start = cursor;
                while (cursor < text.size() && text[cursor] != quote) {
                    ++cursor;
                }
                value = std::string(text.substr(value_start, cursor - value_start));
                if (cursor < text.size()) {
                    ++cursor;
                }
            } else {
                const std::size_t value_start = cursor;
                while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) == 0) {
                    ++cursor;
                }
                value = std::string(text.substr(value_start, cursor - value_start));
            }
        }

        if (!name.empty()) {
            attributes[name] = value;
        }
    }
    return attributes;
}

std::optional<float> ParseFloat(std::string_view value) {
    std::string text = Trim(value);
    if (text.empty()) {
        return std::nullopt;
    }

    float multiplier = 1.0f;
    if (text.ends_with("px")) {
        text.erase(text.size() - 2);
    } else if (text.ends_with('%')) {
        text.pop_back();
        multiplier = 0.01f;
    }

    try {
        return std::stof(text) * multiplier;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<glm::vec2> ParseVec2(std::string_view value) {
    std::string normalized = Trim(value);
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::stringstream stream(normalized);
    std::string first;
    std::string second;
    stream >> first >> second;
    if (first.empty() || second.empty()) {
        return std::nullopt;
    }
    const auto x = ParseFloat(first);
    const auto y = ParseFloat(second);
    if (!x.has_value() || !y.has_value()) {
        return std::nullopt;
    }
    return glm::vec2(*x, *y);
}

glm::vec4 HexToColor(std::string_view value) {
    auto hex_to_int = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return 10 + character - 'a';
        }
        if (character >= 'A' && character <= 'F') {
            return 10 + character - 'A';
        }
        return 0;
    };

    std::string text = std::string(value);
    if (!text.empty() && text.front() == '#') {
        text.erase(text.begin());
    }

    if (text.size() == 3 || text.size() == 4) {
        std::string expanded;
        expanded.reserve(text.size() * 2);
        for (char character : text) {
            expanded.push_back(character);
            expanded.push_back(character);
        }
        text = expanded;
    }

    if (text.size() != 6 && text.size() != 8) {
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }

    const auto channel = [&](std::size_t offset) {
        const int high = hex_to_int(text[offset]);
        const int low = hex_to_int(text[offset + 1]);
        return static_cast<float>((high << 4) | low) / 255.0f;
    };

    return {
        channel(0),
        channel(2),
        channel(4),
        text.size() == 8 ? channel(6) : 1.0f
    };
}

std::optional<glm::vec4> ParseColor(std::string_view value) {
    const std::string text = ToLower(Trim(value));
    if (text.empty()) {
        return std::nullopt;
    }
    if (text == "transparent") {
        return glm::vec4(0.0f);
    }
    if (text == "white") {
        return glm::vec4(1.0f);
    }
    if (text == "black") {
        return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    if (!text.empty() && text.front() == '#') {
        return HexToColor(text);
    }

    auto parse_function = [&](std::string_view prefix) -> std::optional<glm::vec4> {
        if (!text.starts_with(prefix) || text.back() != ')') {
            return std::nullopt;
        }
        const std::string inner = std::string(text.substr(prefix.size(), text.size() - prefix.size() - 1));
        auto parts = Split(inner, ',');
        if ((prefix == "rgb(" && parts.size() != 3) || (prefix == "rgba(" && parts.size() != 4)) {
            return std::nullopt;
        }
        const auto red = ParseFloat(parts[0]);
        const auto green = ParseFloat(parts[1]);
        const auto blue = ParseFloat(parts[2]);
        if (!red.has_value() || !green.has_value() || !blue.has_value()) {
            return std::nullopt;
        }
        auto normalize_channel = [](float channel_value) {
            return channel_value > 1.0f ? std::clamp(channel_value / 255.0f, 0.0f, 1.0f) : std::clamp(channel_value, 0.0f, 1.0f);
        };
        float alpha = 1.0f;
        if (parts.size() == 4) {
            alpha = ParseFloat(parts[3]).value_or(1.0f);
        }
        return glm::vec4(
            normalize_channel(*red),
            normalize_channel(*green),
            normalize_channel(*blue),
            std::clamp(alpha, 0.0f, 1.0f));
    };

    if (const auto rgba = parse_function("rgba("); rgba.has_value()) {
        return rgba;
    }
    if (const auto rgb = parse_function("rgb("); rgb.has_value()) {
        return rgb;
    }
    return std::nullopt;
}

void ApplyStyle(HtmlUiRuntime::Style& destination, const HtmlUiRuntime::Style& source) {
    auto copy_optional = [](auto& dst, const auto& src) {
        if (src.has_value()) {
            dst = src;
        }
    };

    copy_optional(destination.background, source.background);
    copy_optional(destination.accent, source.accent);
    copy_optional(destination.color, source.color);
    copy_optional(destination.title_color, source.title_color);
    copy_optional(destination.subtitle_color, source.subtitle_color);
    copy_optional(destination.hover_background, source.hover_background);
    copy_optional(destination.width, source.width);
    copy_optional(destination.height, source.height);
    copy_optional(destination.padding, source.padding);
    copy_optional(destination.gap, source.gap);
    copy_optional(destination.radius, source.radius);
    copy_optional(destination.scale, source.scale);
    copy_optional(destination.anchor, source.anchor);
    copy_optional(destination.position, source.position);
    copy_optional(destination.size, source.size);
    copy_optional(destination.panel_size, source.panel_size);
}

void ApplyStyleProperty(HtmlUiRuntime::Style& style, std::string_view raw_name, std::string_view raw_value) {
    const std::string name = ToLower(Trim(raw_name));
    const std::string value = Trim(raw_value);
    if (name.empty() || value.empty()) {
        return;
    }

    if (name == "background") {
        style.background = ParseColor(value);
    } else if (name == "accent") {
        style.accent = ParseColor(value);
    } else if (name == "color") {
        style.color = ParseColor(value);
    } else if (name == "title-color") {
        style.title_color = ParseColor(value);
    } else if (name == "subtitle-color") {
        style.subtitle_color = ParseColor(value);
    } else if (name == "hover-background") {
        style.hover_background = ParseColor(value);
    } else if (name == "width") {
        style.width = ParseFloat(value);
    } else if (name == "height") {
        style.height = ParseFloat(value);
    } else if (name == "padding") {
        style.padding = ParseFloat(value);
    } else if (name == "gap") {
        style.gap = ParseFloat(value);
    } else if (name == "radius" || name == "border-radius") {
        style.radius = ParseFloat(value);
    } else if (name == "scale" || name == "font-scale") {
        style.scale = ParseFloat(value);
    } else if (name == "anchor") {
        style.anchor = ParseVec2(value);
    } else if (name == "position") {
        style.position = ParseVec2(value);
    } else if (name == "size") {
        style.size = ParseVec2(value);
    } else if (name == "panel-size") {
        style.panel_size = ParseVec2(value);
    }
}

HtmlUiRuntime::Style ParseStyleBlock(std::string_view block, const std::unordered_map<std::string, std::string>& variables) {
    HtmlUiRuntime::Style style;
    std::size_t cursor = 0;
    while (cursor < block.size()) {
        const std::size_t separator = block.find(':', cursor);
        if (separator == std::string_view::npos) {
            break;
        }
        const std::size_t end = block.find(';', separator);
        const std::string name = Trim(block.substr(cursor, separator - cursor));
        std::string value = Trim(block.substr(separator + 1, end == std::string_view::npos ? block.size() - separator - 1 : end - separator - 1));
        for (const auto& [variable_name, variable_value] : variables) {
            const std::string token = "var(" + variable_name + ")";
            std::size_t position = 0;
            while ((position = value.find(token, position)) != std::string::npos) {
                value.replace(position, token.size(), variable_value);
                position += variable_value.size();
            }
        }
        ApplyStyleProperty(style, name, value);
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1;
    }
    return style;
}

HtmlUiRuntime::Style DefaultStyleForTag(std::string_view tag_name) {
    const std::string tag = ToLower(tag_name);
    HtmlUiRuntime::Style style;
    if (tag == "screen") {
        style.background = glm::vec4(0.03f, 0.05f, 0.09f, 0.76f);
        style.position = glm::vec2(0.5f, 0.5f);
    } else if (tag == "panel") {
        style.background = glm::vec4(0.07f, 0.09f, 0.14f, 0.93f);
        style.accent = glm::vec4(0.26f, 0.76f, 1.0f, 1.0f);
        style.color = glm::vec4(0.95f, 0.98f, 1.0f, 1.0f);
        style.title_color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        style.subtitle_color = glm::vec4(0.70f, 0.82f, 0.96f, 1.0f);
        style.padding = 24.0f;
        style.gap = 10.0f;
        style.radius = 24.0f;
        style.panel_size = glm::vec2(560.0f, 0.0f);
    } else if (tag == "button") {
        style.background = glm::vec4(0.16f, 0.23f, 0.34f, 1.0f);
        style.hover_background = glm::vec4(0.26f, 0.35f, 0.50f, 1.0f);
        style.color = glm::vec4(1.0f);
        style.width = 320.0f;
        style.height = 56.0f;
        style.radius = 18.0f;
    } else if (tag == "widget") {
        style.background = glm::vec4(0.05f, 0.08f, 0.12f, 0.74f);
        style.accent = glm::vec4(0.26f, 0.76f, 1.0f, 1.0f);
        style.color = glm::vec4(0.95f, 0.98f, 1.0f, 1.0f);
        style.padding = 14.0f;
        style.gap = 6.0f;
        style.radius = 18.0f;
        style.anchor = glm::vec2(0.02f, 0.02f);
        style.size = glm::vec2(300.0f, 70.0f);
    } else if (tag == "row") {
        style.gap = 12.0f;
        style.padding = 14.0f;
        style.radius = 18.0f;
    } else if (tag == "stack" || tag == "group" || tag == "card") {
        style.gap = 10.0f;
        style.padding = 14.0f;
        style.radius = 18.0f;
    } else if (tag == "divider") {
        style.height = 1.0f;
        style.color = glm::vec4(0.42f, 0.55f, 0.72f, 0.45f);
    } else if (tag == "spacer") {
        style.height = 12.0f;
    } else if (tag == "title") {
        style.scale = 1.65f;
        style.color = glm::vec4(1.0f);
    } else if (tag == "subtitle") {
        style.scale = 1.0f;
        style.color = glm::vec4(0.70f, 0.82f, 0.96f, 1.0f);
    } else if (tag == "label") {
        style.scale = 0.92f;
        style.color = glm::vec4(0.78f, 0.90f, 1.0f, 1.0f);
    } else if (tag == "text" || tag == "value") {
        style.scale = 1.0f;
        style.color = glm::vec4(1.0f);
    }
    return style;
}

struct SelectorParts {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
};

SelectorParts ParseSelector(std::string_view selector_text) {
    SelectorParts selector;
    const std::string selector_value = ToLower(Trim(selector_text));
    std::size_t cursor = 0;
    while (cursor < selector_value.size()) {
        if (selector_value[cursor] == '#') {
            ++cursor;
            const std::size_t start = cursor;
            while (cursor < selector_value.size() && selector_value[cursor] != '.' && selector_value[cursor] != '#') {
                ++cursor;
            }
            selector.id = selector_value.substr(start, cursor - start);
        } else if (selector_value[cursor] == '.') {
            ++cursor;
            const std::size_t start = cursor;
            while (cursor < selector_value.size() && selector_value[cursor] != '.' && selector_value[cursor] != '#') {
                ++cursor;
            }
            selector.classes.push_back(selector_value.substr(start, cursor - start));
        } else {
            const std::size_t start = cursor;
            while (cursor < selector_value.size() && selector_value[cursor] != '.' && selector_value[cursor] != '#') {
                ++cursor;
            }
            selector.tag = selector_value.substr(start, cursor - start);
        }
    }
    return selector;
}

bool MatchesSelector(const HtmlUiRuntime::Node& node, std::string_view selector_text) {
    const std::string selector = ToLower(Trim(selector_text));
    if (selector.empty() || selector == ":root") {
        return false;
    }

    const SelectorParts parts = ParseSelector(selector);
    if (!parts.tag.empty() && parts.tag != node.tag) {
        return false;
    }
    if (!parts.id.empty() && parts.id != node.id) {
        return false;
    }
    for (const auto& required_class : parts.classes) {
        if (std::find(node.classes.begin(), node.classes.end(), required_class) == node.classes.end()) {
            return false;
        }
    }
    return true;
}

std::string ReplacePlaceholders(
    std::string text,
    const std::unordered_map<std::string, std::string>& values) {
    for (const auto& [key, value] : values) {
        for (const std::string& token : {std::string("${") + key + "}", std::string("{{") + key + "}}", std::string("{") + key + "}"}) {
            std::size_t position = 0;
            while ((position = text.find(token, position)) != std::string::npos) {
                text.replace(position, token.size(), value);
                position += value.size();
            }
        }
    }
    return text;
}

std::string NormalizeAction(std::string value) {
    value = Trim(value);
    if (value.empty()) {
        return value;
    }

    if (value.starts_with("engine.") && value.ends_with("()")) {
        value = value.substr(7, value.size() - 9);
    } else if (value.ends_with("()")) {
        value.erase(value.size() - 2);
    }

    std::string normalized;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char character = value[i];
        if (character == '-' || character == ' ' || character == '.') {
            normalized.push_back('_');
            continue;
        }
        if (std::isupper(static_cast<unsigned char>(character)) != 0) {
            if (i > 0 && normalized.back() != '_') {
                normalized.push_back('_');
            }
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        } else {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
    }

    while (!normalized.empty() && normalized.back() == '_') {
        normalized.pop_back();
    }
    return normalized;
}

void CollectScreens(HtmlUiRuntime::Node& node, std::unordered_map<std::string, const HtmlUiRuntime::Node*>& screens) {
    if (node.tag == "screen" && !node.id.empty()) {
        screens[node.id] = &node;
    }
    for (auto& child : node.children) {
        if (child) {
            CollectScreens(*child, screens);
        }
    }
}

std::unordered_map<std::string, std::string> MergeVariables(
    const std::unordered_map<std::string, std::string>& base,
    const HtmlUiRuntime::DynamicContext& context) {
    std::unordered_map<std::string, std::string> merged = base;
    for (const auto& [key, value] : context.values) {
        merged[key] = value;
    }
    return merged;
}

}  // namespace

bool HtmlUiRuntime::Load(const std::filesystem::path& project_root, const assets::HtmlUiSettings& settings) {
    Clear();
    settings_ = settings;
    if (!settings_.enabled || settings_.html_path.empty()) {
        return false;
    }

    const std::filesystem::path html_path = project_root / settings_.html_path;
    if (!core::FileIO::Exists(html_path)) {
        return false;
    }

    const std::string html_source = core::FileIO::ReadText(html_path);
    std::string css_source;
    if (!settings_.css_path.empty() && core::FileIO::Exists(project_root / settings_.css_path)) {
        css_source = core::FileIO::ReadText(project_root / settings_.css_path);
    }
    std::string js_source;
    if (!settings_.script_path.empty() && core::FileIO::Exists(project_root / settings_.script_path)) {
        js_source = core::FileIO::ReadText(project_root / settings_.script_path);
    }

    root_.tag = "root";
    std::vector<Node*> stack{&root_};
    std::string inline_css;
    std::string inline_js;

    std::size_t cursor = 0;
    while (cursor < html_source.size()) {
        const std::size_t next_tag = html_source.find('<', cursor);
        const std::string text = next_tag == std::string::npos
            ? html_source.substr(cursor)
            : html_source.substr(cursor, next_tag - cursor);
        const std::string collapsed = CollapseWhitespace(text);
        if (!collapsed.empty()) {
            if (!stack.back()->text.empty()) {
                stack.back()->text += ' ';
            }
            stack.back()->text += collapsed;
        }
        if (next_tag == std::string::npos) {
            break;
        }

        if (html_source.compare(next_tag, 4, "<!--") == 0) {
            const std::size_t comment_end = html_source.find("-->", next_tag + 4);
            cursor = comment_end == std::string::npos ? html_source.size() : comment_end + 3;
            continue;
        }

        const std::size_t tag_end = html_source.find('>', next_tag + 1);
        if (tag_end == std::string::npos) {
            break;
        }

        std::string tag_text = Trim(html_source.substr(next_tag + 1, tag_end - next_tag - 1));
        const bool closing = !tag_text.empty() && tag_text.front() == '/';
        const bool self_closing = !tag_text.empty() && tag_text.back() == '/';
        if (closing) {
            if (stack.size() > 1) {
                stack.pop_back();
            }
            cursor = tag_end + 1;
            continue;
        }

        if (self_closing) {
            tag_text.pop_back();
            tag_text = Trim(tag_text);
        }

        const std::size_t name_end = tag_text.find_first_of(" \t\r\n");
        const std::string tag_name = ToLower(tag_text.substr(0, name_end));
        const std::string attributes_text = name_end == std::string::npos ? std::string{} : tag_text.substr(name_end + 1);

        if (tag_name == "style" || tag_name == "script") {
            const std::string close_tag = "</" + tag_name + ">";
            const std::size_t close_position = html_source.find(close_tag, tag_end + 1);
            const std::string payload = close_position == std::string::npos
                ? html_source.substr(tag_end + 1)
                : html_source.substr(tag_end + 1, close_position - tag_end - 1);
            if (tag_name == "style") {
                inline_css += payload;
                inline_css.push_back('\n');
            } else {
                inline_js += payload;
                inline_js.push_back('\n');
            }
            cursor = close_position == std::string::npos ? html_source.size() : close_position + close_tag.size();
            continue;
        }

        auto child = std::make_unique<Node>();
        child->tag = tag_name;
        child->attributes = ParseAttributes(attributes_text);
        if (const auto id_it = child->attributes.find("id"); id_it != child->attributes.end()) {
            child->id = ToLower(id_it->second);
        }
        if (const auto class_it = child->attributes.find("class"); class_it != child->attributes.end()) {
            child->classes = SplitClasses(class_it->second);
        }

        stack.back()->children.push_back(std::move(child));
        if (!self_closing) {
            stack.push_back(stack.back()->children.back().get());
        }
        cursor = tag_end + 1;
    }

    css_source += '\n';
    css_source += inline_css;
    js_source += '\n';
    js_source += inline_js;

    std::size_t css_cursor = 0;
    while (css_cursor < css_source.size()) {
        const std::size_t selector_end = css_source.find('{', css_cursor);
        if (selector_end == std::string::npos) {
            break;
        }
        const std::size_t block_end = css_source.find('}', selector_end + 1);
        if (block_end == std::string::npos) {
            break;
        }
        const std::string selector_text = Trim(css_source.substr(css_cursor, selector_end - css_cursor));
        const std::string block = css_source.substr(selector_end + 1, block_end - selector_end - 1);
        for (const auto& selector : Split(selector_text, ',')) {
            if (ToLower(selector) == ":root") {
                std::size_t declaration_cursor = 0;
                while (declaration_cursor < block.size()) {
                    const std::size_t separator = block.find(':', declaration_cursor);
                    if (separator == std::string::npos) {
                        break;
                    }
                    const std::size_t end = block.find(';', separator);
                    const std::string name = Trim(block.substr(declaration_cursor, separator - declaration_cursor));
                    const std::string value = Trim(block.substr(separator + 1, end == std::string::npos ? block.size() - separator - 1 : end - separator - 1));
                    if (name.starts_with("--")) {
                        css_variables_[name] = value;
                    }
                    if (end == std::string::npos) {
                        break;
                    }
                    declaration_cursor = end + 1;
                }
            } else if (!selector.empty()) {
                css_rules_.push_back({
                    .selector = selector,
                    .style = ParseStyleBlock(block, css_variables_)
                });
            }
        }
        css_cursor = block_end + 1;
    }

    std::stringstream script_stream(js_source);
    std::string line;
    while (std::getline(script_stream, line)) {
        const std::string trimmed = Trim(line);
        if (!trimmed.starts_with("window.") && !trimmed.starts_with("ui.")) {
            continue;
        }
        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        std::string name = Trim(trimmed.substr(0, separator));
        std::string value = Trim(trimmed.substr(separator + 1));
        if (!value.empty() && value.back() == ';') {
            value.pop_back();
            value = Trim(value);
        }
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (name.starts_with("window.")) {
            name = name.substr(7);
        }
        name = Trim(name);
        if (!name.empty()) {
            script_globals_[name] = value;
        }
    }

    CollectScreens(root_, screens_);
    loaded_ = !screens_.empty();
    return loaded_;
}

void HtmlUiRuntime::Clear() {
    root_ = {};
    root_.tag = "root";
    screens_.clear();
    script_globals_.clear();
    css_variables_.clear();
    css_rules_.clear();
    loaded_ = false;
}

bool HtmlUiRuntime::IsEnabled() const {
    return settings_.enabled && loaded_;
}

bool HtmlUiRuntime::HasScreen(std::string_view screen_id) const {
    if (!IsEnabled()) {
        return false;
    }
    return screens_.contains(ToLower(screen_id));
}

HtmlUiRuntime::Style HtmlUiRuntime::ResolveStyle(const Node& node) const {
    Style style = DefaultStyleForTag(node.tag);
    for (const auto& rule : css_rules_) {
        if (MatchesSelector(node, rule.selector)) {
            ApplyStyle(style, rule.style);
        }
    }
    if (const auto inline_style = node.attributes.find("style"); inline_style != node.attributes.end()) {
        ApplyStyle(style, ParseStyleBlock(inline_style->second, css_variables_));
    }

    const std::array<std::string, 14> attribute_names{
        "background", "accent", "color", "title-color", "subtitle-color", "hover-background",
        "width", "height", "padding", "gap", "radius", "scale", "anchor", "position"
    };
    for (const auto& name : attribute_names) {
        if (const auto it = node.attributes.find(name); it != node.attributes.end()) {
            ApplyStyleProperty(style, name, it->second);
        }
    }
    if (const auto it = node.attributes.find("size"); it != node.attributes.end()) {
        ApplyStyleProperty(style, "size", it->second);
    }
    if (const auto it = node.attributes.find("panel-size"); it != node.attributes.end()) {
        ApplyStyleProperty(style, "panel-size", it->second);
    }
    return style;
}

void HtmlUiRuntime::RenderScreen(
    std::string_view raw_screen_id,
    const DynamicContext& context,
    const TextResolver& text_resolver,
    const ActionHandler& action_handler) const {
    if (!IsEnabled()) {
        return;
    }

    const auto screen_it = screens_.find(ToLower(raw_screen_id));
    if (screen_it == screens_.end() || screen_it->second == nullptr) {
        return;
    }

    const Node& screen = *screen_it->second;
    const Style screen_style = ResolveStyle(screen);
    const bool hud_mode = screen.id == "hud";
    const auto variables = MergeVariables(script_globals_, context);
    auto resolve_text = [&](const Node& node) {
        std::string value = node.text;
        if (value.empty()) {
            if (const auto it = node.attributes.find("text"); it != node.attributes.end()) {
                value = it->second;
            }
        }
        value = ReplacePlaceholders(value, variables);
        if (text_resolver) {
            value = text_resolver(value);
        }
        return value;
    };

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowBgAlpha(0.0f);
    const std::string window_name = "HtmlUiScreen###" + std::string(raw_screen_id);
    ImGui::Begin(window_name.c_str(), nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNavFocus);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!hud_mode) {
        const glm::vec4 overlay = screen_style.background.value_or(glm::vec4(0.03f, 0.05f, 0.09f, 0.76f));
        draw_list->AddRectFilled(
            viewport->Pos,
            {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y},
            IM_COL32(
                static_cast<int>(overlay.r * 255.0f),
                static_cast<int>(overlay.g * 255.0f),
                static_cast<int>(overlay.b * 255.0f),
                static_cast<int>(overlay.a * 255.0f)));
        draw_list->AddRectFilledMultiColor(
            viewport->Pos,
            {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y * 0.55f},
            IM_COL32(16, 30, 48, 72),
            IM_COL32(0, 0, 0, 0),
            IM_COL32(0, 0, 0, 0),
            IM_COL32(8, 20, 36, 60));
    }

    auto render_text_node = [&](const Node& node, const Style& style, const glm::vec4& fallback_color) {
        const std::string value = resolve_text(node);
        if (value.empty()) {
            return;
        }
        const glm::vec4 color = style.color.value_or(fallback_color);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.r, color.g, color.b, color.a));
        ImGui::SetWindowFontScale(style.scale.value_or(1.0f));
        ImGui::TextWrapped("%s", value.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    };

    auto render_panel = [&](const Node& panel, int panel_index, bool hud_mode) {
        const Style style = ResolveStyle(panel);
        const glm::vec4 background = style.background.value_or(glm::vec4(0.07f, 0.09f, 0.14f, 0.93f));
        const glm::vec4 accent = style.accent.value_or(glm::vec4(0.26f, 0.76f, 1.0f, 1.0f));
        const float radius = style.radius.value_or(hud_mode ? 18.0f : 24.0f);
        const float padding = style.padding.value_or(hud_mode ? 14.0f : 24.0f);
        const float gap = style.gap.value_or(hud_mode ? 6.0f : 10.0f);
        const float base_line_height = ImGui::GetTextLineHeight();
        const float default_button_width = hud_mode ? 180.0f : 320.0f;
        const float default_button_height = hud_mode ? 44.0f : 56.0f;

        std::function<float(const Node&, const Style&)> estimate_node_height;
        estimate_node_height = [&](const Node& child, const Style& child_style) {
            if (child.tag == "button") {
                return child_style.height.value_or(default_button_height);
            }
            if (child.tag == "divider") {
                return std::max(child_style.height.value_or(1.0f), 1.0f) + 8.0f;
            }
            if (child.tag == "spacer") {
                if (child_style.size.has_value() && child_style.size->y > 0.0f) {
                    return child_style.size->y;
                }
                return std::max(child_style.height.value_or(12.0f), 1.0f);
            }
            if (child.tag == "row" || child.tag == "stack" || child.tag == "group" || child.tag == "card" ||
                child.tag == "panel" || child.tag == "widget") {
                const float child_padding = child_style.padding.value_or(0.0f);
                const float child_gap = child_style.gap.value_or(8.0f);
                const bool horizontal = child.tag == "row";
                float inner = 0.0f;
                bool has_content = false;
                for (const auto& nested_child : child.children) {
                    if (!nested_child) {
                        continue;
                    }
                    const Style nested_style = ResolveStyle(*nested_child);
                    const float nested_height = estimate_node_height(*nested_child, nested_style);
                    if (horizontal) {
                        inner = std::max(inner, nested_height);
                    } else {
                        if (has_content) {
                            inner += child_gap;
                        }
                        inner += nested_height;
                    }
                    has_content = true;
                }
                if (!has_content) {
                    inner = default_button_height;
                }
                float total = child_padding * 2.0f + inner;
                if (child_style.size.has_value() && child_style.size->y > 0.0f) {
                    total = std::max(total, child_style.size->y);
                }
                if (child_style.height.has_value()) {
                    total = std::max(total, *child_style.height);
                }
                return total;
            }

            float scale = child_style.scale.value_or(1.0f);
            if (child.tag == "title") {
                scale = child_style.scale.value_or(1.65f);
            } else if (child.tag == "subtitle") {
                scale = child_style.scale.value_or(1.0f);
            } else if (child.tag == "label") {
                scale = child_style.scale.value_or(0.92f);
            }

            float height = base_line_height * std::max(scale, 0.65f);
            if (child.tag == "title") {
                height *= 1.35f;
            } else if (child.tag == "subtitle") {
                height *= 1.15f;
            } else if (child.tag == "text" || child.tag == "value") {
                height *= 1.08f;
            }
            return height;
        };

        const auto estimate_panel_height = [&]() {
            float total = padding * 2.0f;
            bool has_content = false;
            for (const auto& child_ptr : panel.children) {
                if (!child_ptr) {
                    continue;
                }
                const Style child_style = ResolveStyle(*child_ptr);
                if (has_content) {
                    total += gap;
                }
                total += estimate_node_height(*child_ptr, child_style);
                has_content = true;
            }
            return std::max(total, padding * 2.0f + default_button_height);
        };

        ImVec2 position{};
        ImVec2 size{};
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings;

        if (hud_mode) {
            const glm::vec2 anchor = style.anchor.value_or(glm::vec2(0.02f, 0.02f));
            const glm::vec2 widget_size = style.size.value_or(glm::vec2(300.0f, 70.0f));
            position = {
                viewport->Pos.x + viewport->Size.x * anchor.x,
                viewport->Pos.y + viewport->Size.y * anchor.y
            };
            size = {widget_size.x, widget_size.y};
        } else {
            const glm::vec2 panel_size = style.panel_size.value_or(glm::vec2(560.0f, 0.0f));
            const glm::vec2 anchor = style.position.value_or(screen_style.position.value_or(glm::vec2(0.5f, 0.5f)));
            const glm::vec2 resolved_size{
                panel_size.x > 0.0f ? panel_size.x : 560.0f,
                panel_size.y > 0.0f ? panel_size.y : estimate_panel_height()
            };
            position = {
                viewport->Pos.x + viewport->Size.x * anchor.x - resolved_size.x * 0.5f,
                viewport->Pos.y + viewport->Size.y * anchor.y - resolved_size.y * 0.5f + static_cast<float>(panel_index) * 26.0f
            };
            size = {resolved_size.x, resolved_size.y};
        }

        ImGui::SetNextWindowPos(position);
        ImGui::SetNextWindowSize({size.x, size.y});
        ImGui::SetNextWindowBgAlpha(0.0f);
        const std::string panel_name = "HtmlUiPanel###" + std::string(raw_screen_id) + "_" + std::to_string(panel_index) + "_" + panel.id;
        ImGui::Begin(panel_name.c_str(), nullptr, flags);

        ImDrawList* panel_draw_list = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetWindowPos();
        const ImVec2 max{min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y};
        panel_draw_list->AddRectFilled(min, max, IM_COL32(
            static_cast<int>(background.r * 255.0f),
            static_cast<int>(background.g * 255.0f),
            static_cast<int>(background.b * 255.0f),
            static_cast<int>(background.a * 255.0f)), radius);
        panel_draw_list->AddRectFilled(
            min,
            {min.x + (hud_mode ? 4.0f : 6.0f), max.y},
            IM_COL32(
                static_cast<int>(accent.r * 255.0f),
                static_cast<int>(accent.g * 255.0f),
                static_cast<int>(accent.b * 255.0f),
                static_cast<int>(accent.a * 255.0f)),
            radius);
        panel_draw_list->AddRect(min, max, IM_COL32(140, 180, 220, hud_mode ? 42 : 56), radius, 0, 1.0f);

        std::function<void(const Node&, const Style&, std::string_view)> render_node;
        render_node = [&](const Node& child, const Style& parent_style, std::string_view node_path) {
            const Style child_style = ResolveStyle(child);
            const std::string text_value = resolve_text(child);

            if (child.tag == "button") {
                const glm::vec4 button_color = child_style.background.value_or(glm::vec4(0.16f, 0.23f, 0.34f, 1.0f));
                const glm::vec4 hover_color = child_style.hover_background.value_or(glm::vec4(0.26f, 0.35f, 0.50f, 1.0f));
                const glm::vec4 text_color = child_style.color.value_or(glm::vec4(1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, child_style.radius.value_or(18.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(button_color.r, button_color.g, button_color.b, button_color.a));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(hover_color.r, hover_color.g, hover_color.b, hover_color.a));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(hover_color.r, hover_color.g, hover_color.b, hover_color.a));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(text_color.r, text_color.g, text_color.b, text_color.a));
                ImGui::SetWindowFontScale(child_style.scale.value_or(1.0f));
                const glm::vec2 style_size = child_style.size.value_or(glm::vec2(0.0f));
                const float button_width = child_style.width.value_or(style_size.x > 0.0f ? style_size.x : default_button_width);
                const float button_height = child_style.height.value_or(style_size.y > 0.0f ? style_size.y : default_button_height);
                if (ImGui::Button(text_value.empty() ? "Button" : text_value.c_str(), {button_width, button_height})) {
                    std::string action;
                    if (const auto it = child.attributes.find("action"); it != child.attributes.end()) {
                        action = it->second;
                    } else if (const auto it = child.attributes.find("onclick"); it != child.attributes.end()) {
                        action = it->second;
                    }
                    if (!action.empty() && action_handler) {
                        action_handler(NormalizeAction(action));
                    }
                }
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
                return;
            }

            if (child.tag == "divider") {
                const glm::vec4 divider_color = child_style.color.value_or(glm::vec4(0.42f, 0.55f, 0.72f, 0.45f));
                const float thickness = std::max(child_style.height.value_or(1.0f), 1.0f);
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
                ImGui::GetWindowDrawList()->AddLine(
                    {cursor.x, cursor.y + thickness * 0.5f},
                    {cursor.x + width, cursor.y + thickness * 0.5f},
                    IM_COL32(
                        static_cast<int>(divider_color.r * 255.0f),
                        static_cast<int>(divider_color.g * 255.0f),
                        static_cast<int>(divider_color.b * 255.0f),
                        static_cast<int>(divider_color.a * 255.0f)),
                    thickness);
                ImGui::Dummy({width, thickness + 6.0f});
                return;
            }

            if (child.tag == "spacer") {
                const glm::vec2 style_size = child_style.size.value_or(glm::vec2(1.0f, child_style.height.value_or(12.0f)));
                ImGui::Dummy({std::max(style_size.x, 1.0f), std::max(style_size.y, 1.0f)});
                return;
            }

            if (child.tag == "row" || child.tag == "stack" || child.tag == "group" || child.tag == "card" ||
                child.tag == "panel" || child.tag == "widget") {
                const bool horizontal = child.tag == "row";
                const float child_padding = child_style.padding.value_or(0.0f);
                const float child_gap = child_style.gap.value_or(horizontal ? 12.0f : 8.0f);
                const float child_radius = child_style.radius.value_or(18.0f);
                const glm::vec4 child_background = child_style.background.value_or(glm::vec4(0.0f));
                const glm::vec4 child_accent = child_style.accent.value_or(parent_style.accent.value_or(glm::vec4(0.26f, 0.76f, 1.0f, 1.0f)));
                const glm::vec2 style_size = child_style.size.value_or(glm::vec2(0.0f));
                const float estimated_height = estimate_node_height(child, child_style);
                const float child_width = child_style.width.value_or(style_size.x > 0.0f ? style_size.x : std::max(ImGui::GetContentRegionAvail().x, 1.0f));
                const float child_height = child_style.height.value_or(style_size.y > 0.0f ? style_size.y : estimated_height);

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(child_background.r, child_background.g, child_background.b, child_background.a));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, child_radius);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(child_padding, child_padding));
                const std::string container_name = "HtmlUiNode###" + std::string(node_path) + "_" + child.tag + "_" + child.id;
                ImGui::BeginChild(container_name.c_str(), {child_width, child_height}, false,
                    ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse |
                    ImGuiWindowFlags_NoSavedSettings);

                if (child_background.a > 0.001f) {
                    ImDrawList* child_draw_list = ImGui::GetWindowDrawList();
                    const ImVec2 child_min = ImGui::GetWindowPos();
                    const ImVec2 child_max{child_min.x + ImGui::GetWindowSize().x, child_min.y + ImGui::GetWindowSize().y};
                    child_draw_list->AddRectFilled(
                        child_min,
                        child_max,
                        IM_COL32(
                            static_cast<int>(child_background.r * 255.0f),
                            static_cast<int>(child_background.g * 255.0f),
                            static_cast<int>(child_background.b * 255.0f),
                            static_cast<int>(child_background.a * 255.0f)),
                        child_radius);
                    child_draw_list->AddRectFilled(
                        child_min,
                        {child_min.x + 4.0f, child_max.y},
                        IM_COL32(
                            static_cast<int>(child_accent.r * 255.0f),
                            static_cast<int>(child_accent.g * 255.0f),
                            static_cast<int>(child_accent.b * 255.0f),
                            static_cast<int>(child_accent.a * 255.0f)),
                        child_radius);
                }

                for (std::size_t nested_index = 0; nested_index < child.children.size(); ++nested_index) {
                    if (!child.children[nested_index]) {
                        continue;
                    }
                    if (horizontal && nested_index > 0) {
                        ImGui::SameLine(0.0f, child_gap);
                    } else if (!horizontal && nested_index > 0) {
                        ImGui::Dummy({1.0f, child_gap});
                    }
                    render_node(
                        *child.children[nested_index],
                        child_style,
                        std::string(node_path) + "/" + child.tag + "[" + std::to_string(nested_index) + "]");
                }

                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                return;
            }

            if (child.tag == "title") {
                render_text_node(child, child_style, parent_style.title_color.value_or(glm::vec4(1.0f)));
            } else if (child.tag == "subtitle") {
                render_text_node(child, child_style, parent_style.subtitle_color.value_or(glm::vec4(0.70f, 0.82f, 0.96f, 1.0f)));
            } else if (child.tag == "label" || child.tag == "text" || child.tag == "value") {
                render_text_node(child, child_style, parent_style.color.value_or(glm::vec4(0.95f, 0.98f, 1.0f, 1.0f)));
            } else if (!text_value.empty()) {
                render_text_node(child, child_style, parent_style.color.value_or(glm::vec4(0.95f, 0.98f, 1.0f, 1.0f)));
            }
        };

        ImGui::SetCursorPos({padding, padding});
        for (std::size_t child_index = 0; child_index < panel.children.size(); ++child_index) {
            if (!panel.children[child_index]) {
                continue;
            }
            render_node(
                *panel.children[child_index],
                style,
                std::string(raw_screen_id) + "/panel[" + std::to_string(panel_index) + "]/child[" + std::to_string(child_index) + "]");
            if (child_index + 1 < panel.children.size()) {
                ImGui::Dummy({1.0f, gap});
            }
        }

        ImGui::End();
    };

    int panel_index = 0;
    for (const auto& child : screen.children) {
        if (child != nullptr &&
            ((hud_mode && (child->tag == "widget" || child->tag == "panel")) ||
             (!hud_mode && child->tag == "panel"))) {
            render_panel(*child, panel_index++, hud_mode);
        }
    }

    ImGui::End();
}

}  // namespace novaiso::ui
