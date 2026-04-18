# NovaIso Engine

`NovaIso Engine` is a compact but fully integrated 2D platformer engine stack split into:

- `NovaIsoEditor`: a visual editor with docking, live preview, trigger editing, parallax editing, asset importing, and Python script editing.
- `NovaIsoRuntime`: a lean runtime that loads a project folder and executes Python-driven gameplay.
<img width="3436" height="1440" alt="{5598F5E3-7061-47FA-9E60-1A20BE5BCCBD}" src="https://github.com/user-attachments/assets/e1f9d0b7-ebf2-44ff-8c72-fea66f1821a1" />
<img width="1596" height="931" alt="{845C7144-2EC2-44F5-8FAC-7C4EECCFD6CC}" src="https://github.com/user-attachments/assets/beeec38b-9746-49e5-83c8-72b37c9507a8" />

## Features

- SDL2 windowing/input with an OpenGL 4.6 core renderer.
- Dear ImGui editor UI with docking and viewports enabled.
- Embedded Python 3.12 via pybind11 for gameplay, triggers, and conditions/actions.
- JSON project + `.niso` level format.
- Side-view and isometric camera modes with runtime toggling.
- Custom platformer AABB physics with gravity, slopes, and one-way platforms.
- Sprite/tile rendering, unlimited parallax layers, trigger zones, and post-processing.
- Shader hot-reload with built-in `bloom`, `pixelate`, and `outline` effects.

## Build

Prerequisites:

- CMake 3.26+
- A C++20 compiler
- Python 3.12 with development headers/libraries

Configure and build:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Executables are emitted under `build/bin`.

## Run

Editor:

```powershell
./build/bin/NovaIsoEditor.exe
```

Runtime with the demo project:

```powershell
./build/bin/NovaIsoRuntime.exe ./build/bin/demo_project
```

If no project path is passed, the runtime falls back to the copied `demo_project` folder next to the executable.
