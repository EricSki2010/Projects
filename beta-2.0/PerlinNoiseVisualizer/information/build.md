# Build & Run

## Prerequisites

Inherits everything from [VisualEngine/information/requirements.md](../../VisualEngine/information/requirements.md):
- MSVC (Visual Studio 2022) + CMake 3.20+
- vcpkg at `C:/vcpkg` with `glm`, `stb`, `freetype` on the `x64-windows-static` triplet
- GLAD at `E:/glad/`

The CMake config ([CMakeLists.txt](../CMakeLists.txt)) pins:
- vcpkg toolchain file: `C:/vcpkg/scripts/buildsystems/vcpkg.cmake`
- Triplet: `x64-windows-static`
- C++17, `MultiThreaded` MSVC runtime

## Build

From `PerlinNoiseVisualizer/`:

```
build.bat
```

Equivalent to:

```
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

Output: `build/Release/PerlinNoiseVisualizer.exe`.

## Run

```
run.bat
```

Runs the `.exe` from its build directory so its working directory is `build/Release/` (any relative asset paths resolve from there).

## Window

Initialized in [main.cpp](../main.cpp): 1280×800, maximized, title "Perlin Noise Visualizer". Starts on the `noise2D` scene with world seed `10`.

## Controls

| Input | Effect |
| --- | --- |
| `WASD` + mouse | Standard engine camera (colliders disabled) |
| `Ctrl + 2` | Cycle to the next registered scene (`noise2D` ↔ `visualizer`) |
| `Ctrl + 1` | Toggle wireframe (both scenes) |
| `E` | Toggle "super speed" (5× move speed) — `visualizer` scene only |

`Ctrl + 2` is bound via `VE::setSceneCycleHotkey(GLFW_KEY_2, GLFW_KEY_LEFT_CONTROL)` in `main.cpp`. Adding more scenes extends the cycle automatically.

## Scenes

**`noise2D`** — Bird's-eye view of a 512×512 biome heatmap quad spanning [-256, +256] in X/Z. Built once in `onEnter`, static thereafter. Camera starts at (0, 320, 240) pitched -55°. Move speed 60.

**`visualizer`** — Streamed 3D voxel world. Camera starts at (150, 230, 150) pitched -25°. Move speed 25 (or 125 with `E` toggled). See [architecture.md](architecture.md) for the streaming pipeline.

## Changing the World Seed

Edit the last argument to `setScene` in [main.cpp](../main.cpp):

```cpp
setScene("noise2D", 10);  // 10 → any int
```

The local `setScene` wrapper calls `PerlinNoise::setSeed` before `VE::setScene`, so the seed is locked in before any scene's `onEnter` runs. Calling `VE::setScene` directly would skip that and leave the noise system out of sync.

## Tweaking Generation

Constants live at the top of each source file:
- 3D world rules + bounds + view distances: [scene.cpp](../scene.cpp) (see constants table in [architecture.md](architecture.md))
- Biome thresholds + amplitudes: [biome.cpp](../biome.cpp)
- Heatmap resolution + span: [noise2D.cpp](../noise2D.cpp)
- Perlin scale: `main.cpp` calls `PerlinNoise::setScale(0.050f)` — lower = bigger features
