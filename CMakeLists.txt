cmake_minimum_required(VERSION 3.26)

project(NovaIsoEngine VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# РЎС‚Р°С‚РёС‡РµСЃРєР°СЏ Р»РёРЅРєРѕРІРєР° CRT РґР»СЏ MSVC:
# Release -> /MT
# Debug   -> /MTd
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(FetchContent)

option(BUILD_SHARED_LIBS "Build shared libraries when supported." ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VENDORED ON CACHE BOOL "" FORCE)
set(SDLMIXER_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_DEPS_SHARED ON CACHE BOOL "" FORCE)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)

find_package(OpenGL REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter Development)

file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" NOVAISO_PYTHON_EXECUTABLE_PATH)
get_filename_component(NOVAISO_PYTHON_HOME_PATH "${NOVAISO_PYTHON_EXECUTABLE_PATH}" DIRECTORY)
set(NOVAISO_PYTHON_STDLIB_PATH "${NOVAISO_PYTHON_HOME_PATH}/Lib")
set(NOVAISO_PYTHON_DLLS_PATH "${NOVAISO_PYTHON_HOME_PATH}/DLLs")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scripting/PythonRuntimeConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/PythonRuntimeConfig.h
    @ONLY
)

FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.11
)
FetchContent_Declare(
    SDL2_mixer
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
    GIT_TAG release-2.8.1
)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v3.0.0
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.6
)
FetchContent_Declare(
    imgui_text_editor
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG master
)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
)

FetchContent_MakeAvailable(SDL2 SDL2_mixer glm pybind11 nlohmann_json imgui stb)
FetchContent_GetProperties(glad)
if(NOT glad_POPULATED)
    FetchContent_Populate(glad)
endif()
add_subdirectory(${glad_SOURCE_DIR}/cmake ${glad_BINARY_DIR})
FetchContent_GetProperties(imgui_text_editor)
if(NOT imgui_text_editor_POPULATED)
    FetchContent_Populate(imgui_text_editor)
endif()

glad_add_library(glad_api REPRODUCIBLE EXCLUDE_FROM_ALL API gl:core=4.6)

add_library(imgui_support STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_text_editor_SOURCE_DIR}/TextEditor.cpp
)
target_include_directories(imgui_support
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${imgui_text_editor_SOURCE_DIR}
)
target_compile_definitions(imgui_support PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)

set(NOVAISO_SDL_TARGET "")
foreach(candidate SDL2::SDL2 SDL2::SDL2-static SDL2-static SDL2)
    if(TARGET ${candidate})
        set(NOVAISO_SDL_TARGET ${candidate})
        break()
    endif()
endforeach()
if(NOT NOVAISO_SDL_TARGET)
    message(FATAL_ERROR "Unable to locate an SDL2 target after FetchContent.")
endif()

set(NOVAISO_SDL_MAIN_TARGET "")
foreach(candidate SDL2::SDL2main SDL2main)
    if(TARGET ${candidate})
        set(NOVAISO_SDL_MAIN_TARGET ${candidate})
        break()
    endif()
endforeach()

set(NOVAISO_SDL_MIXER_TARGET "")
foreach(candidate SDL2_mixer::SDL2_mixer SDL2_mixer::SDL2_mixer-static SDL2_mixer SDL2_mixer-static)
    if(TARGET ${candidate})
        set(NOVAISO_SDL_MIXER_TARGET ${candidate})
        break()
    endif()
endforeach()
if(NOT NOVAISO_SDL_MIXER_TARGET)
    message(FATAL_ERROR "Unable to locate an SDL2_mixer target after FetchContent.")
endif()

target_link_libraries(imgui_support PUBLIC ${NOVAISO_SDL_TARGET} OpenGL::GL glad_api)

set(NOVAISO_ENGINE_SOURCES
    src/core/Application.cpp
    src/core/EmbeddedResources.cpp
    src/core/FileIO.cpp
    src/core/ImageIO.cpp
    src/core/Input.cpp
    src/core/ThreadPool.cpp
    src/core/VulkanBootstrap.cpp
    src/camera/Camera2D.cpp
    src/assets/Project.cpp
    src/assets/AssetManager.cpp
    src/renderer/ShaderProgram.cpp
    src/renderer/Texture2D.cpp
    src/renderer/Framebuffer.cpp
    src/renderer/Renderer2D.cpp
    src/renderer/PostProcessStack.cpp
    src/entities/Scene.cpp
    src/physics/PlatformerPhysics.cpp
    src/triggers/TriggerSystem.cpp
    src/scripting/PythonScripting.cpp
    src/ui/HtmlUiRuntime.cpp
    src/editor/EditorApp.cpp
    src/runtime/RuntimeApp.cpp
)

add_library(novaiso_engine STATIC ${NOVAISO_ENGINE_SOURCES})
target_include_directories(novaiso_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${imgui_text_editor_SOURCE_DIR}
        ${stb_SOURCE_DIR}
)
target_compile_definitions(novaiso_engine PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)
target_link_libraries(novaiso_engine
    PUBLIC
        ${NOVAISO_SDL_TARGET}
        ${NOVAISO_SDL_MIXER_TARGET}
        glad_api
        OpenGL::GL
        Vulkan::Vulkan
        glm::glm
        imgui_support
        nlohmann_json::nlohmann_json
        pybind11::embed
        Python3::Python
)
if(WIN32)
    target_link_libraries(novaiso_engine PUBLIC ole32 shell32 dxgi psapi pdh)
endif()

add_executable(NovaIsoEditor WIN32 editor_main.cpp)
target_link_libraries(NovaIsoEditor PRIVATE novaiso_engine ${NOVAISO_SDL_MAIN_TARGET})

add_executable(NovaIsoRuntime WIN32 runtime_main.cpp)
target_link_libraries(NovaIsoRuntime PRIVATE novaiso_engine ${NOVAISO_SDL_MAIN_TARGET})

function(novaiso_copy_content target_name)
    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders
            $<TARGET_FILE_DIR:${target_name}>/shaders
    )
    if(MSVC)
        add_custom_command(
            TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target_name}>
                $<TARGET_FILE_DIR:${target_name}>
            COMMAND_EXPAND_LISTS
        )
    endif()
endfunction()

novaiso_copy_content(NovaIsoEditor)
novaiso_copy_content(NovaIsoRuntime)


