#include "editor/EditorApp.h"

#include <filesystem>

int main(int argc, char** argv) {
    std::filesystem::path project_root;
    if (argc > 1) {
        project_root = argv[1];
    }

    novaiso::editor::EditorApp app(project_root);
    return app.Run();
}
