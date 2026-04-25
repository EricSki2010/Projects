# VisualEngine + Model Editor

A custom OpenGL 4.3 voxel-style 3D model editor built from scratch in C++.
The engine (VisualEngine) handles rendering, input, collision, and UI.
The application (modelEditor) is a tool for building, painting, and editing
3D models made of blocks and custom shapes.

## What the Engine Can Do (VisualEngine)

- OpenGL 4.3 rendering with Phong lighting (diffuse, specular, ambient)
- Mesh registration from code or binary .mesh files
- Per-instance mesh placement with position and rotation (degrees, any axis)
- Face-pair culling: per-face states (solid/partial/open), rotation-aware, O(n) matching
- Per-triangle collision detection via Moller-Trumbore ray-triangle intersection
- AABB fast-path raycasting for rectangular meshes
- FPS camera with mouse look (Q toggle) and WASD movement
- Scene system with lifecycle hooks (enter, exit, input, update, render)
- UI system with buttons, panels, text inputs, dropdowns, images. Inputs
  use a GLFW character callback for layout-aware printable-ASCII input,
  support an in-input caret (Left/Right/Backspace operate at the caret),
  and an opt-in `numericOnly` filter for digit/`.`/`-` fields.
- Text rendering via FreeType font rasterization
- Line and dot rendering for wireframes and debug visualization
- Triangle overlay system for selection highlights
- Render-to-texture for preview thumbnails
- Gradient background that follows camera direction
- Binary save/load system with RLE compression
- Model file format (.mdl) with block types, placements, rotations,
  per-face paint colors, and 16-color palette

## What the Model Editor Can Do

### Build Mode
- Place blocks on a 3D grid (cubes, wedges, or custom shapes)
- Select faces by clicking or drag-selecting rectangles of faces
- Extrude new blocks from selected faces (Ctrl+A)
- Delete blocks (Ctrl+D)
- Replace blocks with a different mesh type (Ctrl+A in block select)
- Rotate placed blocks in 90-degree increments on any axis (R + 1/2/3) with rotation-aware face culling
- 15 mesh slots: 2 built-in (cube, wedge) + 13 custom slots
- Scroll wheel to cycle through available meshes
- Tab to toggle full-block selection mode
- Ctrl+Z undo with 50-step history
- Ctrl+Tab shortcut to edit the current slot in the vectorMesh editor

### Paint Mode
- Select individual triangles on any mesh face
- Three selection methods:
  - Left-drag brush: paint over triangles as you drag
  - Right-click: select single triangle
  - Right-drag: select all coplanar triangles (same surface)
- 16-color palette displayed as a fan widget in the corner
- Set colors via RGB (0-255) or Hex input with brightness control
- Apply selected color to selected faces (Ctrl+A)
- Colors render in real-time on the 3D model
- Palette and per-face colors save/load with the model file

### Custom Mesh Editor (Vector Mesh)
- Create custom 3D shapes from scratch
- Dot mode: place vertices with snap-to-edge (configurable snap count)
  - Click a placed vertex to select it (magenta highlight) and live-edit
    its X/Y/Z coordinates from the right sidebar — edits are written to
    the vertex every frame, so all triangles/lines that reference it
    follow the move automatically (indices stay intact)
- Line mode: connect dots into edges
- Plane mode: create triangles from 3 dots
  - Multi-select triangles: plain click replaces, Ctrl+click toggles,
    Shift+click adds, click empty space clears
  - **Flip Normal(s)** acts on the whole selection
  - **Subdivide** splits each selected triangle into 4 children with
    shared midpoint vertices; selection extends to include the new
    children so you can keep subdividing
- Saves as .vmesh (editable) + .mesh (renderable with pre-computed normals)
- Custom meshes appear in the block selector for placement
- Single **Save & Exit** button in the pause menu (saves .vmesh, exports
  .mesh, returns to 3dModeler / menu)
- Ctrl+Z undo with 50-step history (one snapshot per coord-edit focus
  session, so typing keystrokes don't flood the stack)
- Ctrl+Tab to save and return to the 3D modeler
- Built-in prefab meshes (cube, wedge) are protected from editing

### File Management
- Create new models (empty or from cube template, configurable size)
- Save and load models (.mdl binary format)
- Duplicate and delete model files from the menu
- Models store: block types, placements, rotations, paint data, palette
- Backward-compatible file format (v1, v2, v3)
- Auto-save on window close or scene switch

### Export
- Export models to glTF Binary (.glb) via the pause menu
- Per-color primitives using the KHR_materials_unlit extension
- Baked world-space vertices with recomputed normals
- Ready to load into any glTF-compatible engine for your own lighting
- Texture packing utility: rasterize 2D triangle groups (with per-triangle colors) into square PNG buffers with conservative coverage — no missing pixels at edges

### AI Agent (optional, opt-in)
- Built-in Gemini-powered assistant that drives the editor through tool calls
- Main-menu **AI: ON / AI: OFF** toggle (default OFF). Off means zero
  Python spawning, no sidecar, no AI UI — identical to a no-AI build.
- **Set API Key** screen inside the menu — paste a Gemini key, click Save;
  written to local `.gemini_key`, env var updated in-process, sidecar
  hot-restarted, no shell needed. Friend-shippable.
- Left-side prompt panel in 3dModeler and vectorMesh: multi-line input
  that grows downward as text wraps, 8-row response display, **Send** and
  (3dModeler only) **Send + Info** buttons. Per-prompt USD cost printed
  alongside every response.
- **3dModeler tools**: palette set/get/select, transition into vectorMesh.
  *Send + Info* adds: camera position/forward, ray-AABB block pick,
  list-blocks.
- **vectorMesh tools**: one-shot `vmesh_build` (clear + add verts + add
  tris + auto-fix normals + save in a single call), batch add / delete
  vertices and triangles, subdivide-triangles, perturb-vertices,
  scale-from-origin, auto-fix-normals, list / save / clear / finish.
- **Token economy**: scene-filtered tool sets, 2-letter wire aliases, slim
  schemas, batch + macro tools, retry on transient errors, accurate cost
  accounting. A full pyramid build runs ~$0.0005 on `gemini-2.5-flash`.
- **Architecture**: small Python sidecar (`src/mechanics/AiHandling/agent.py`)
  using `google-genai` runs the Gemini chat loop; the C++ side
  (`src/mechanics/AiHandling/`) spawns it as a subprocess and routes
  function calls onto the main thread.
- **Auto-install**: on first launch with AI enabled, `setupAiDependencies()`
  runs `pip install google-genai` if the import isn't found.

### General
- Pause menu with save & exit
- Camera locked during pause
- Ghost block at origin shows where first block goes
- Rotating 3D previews of each mesh in the selector grid
- All rendering, collision, and UI built from scratch (no game engine)

## File Formats

| Format | Description |
|--------|-------------|
| `.mdl` | Model file (block types + placements + rotations + paint + palette) |
| `.mesh` | Renderable mesh (vertices + indices + optional normals + optional texture) |
| `.vmesh` | Vector mesh editor data (dots + lines + triangles, editable) |
| `.glb` | Exported glTF Binary for use in other engines |
| `.gemini_key` | User-local Gemini API key (gitignored, optional, only when AI is on) |
| `ai_enabled.txt` | User-local AI on/off preference (gitignored, default OFF) |

## Tech Stack

- C++17
- OpenGL 4.3 Core Profile
- GLFW (windowing + input)
- GLM (math)
- FreeType (font rendering)
- stb_image (texture loading)
- stb_image_write (PNG writing for texture export)
- nlohmann/json (single-header, bundled in `beta-2.0/thirdparty/`)
- CMake build system
- MSVC compiler (Windows)
- Python 3 + `google-genai` (only if the optional AI agent is enabled)

## Project Structure

```
beta-2.0/
├── VisualEngine/               — Engine library
│   ├── VisualEngine.h/.cpp     — Public API
│   ├── EngineGlobals.h         — Global state
│   ├── renderingManagement/
│   │   ├── (core)              — Shader, Scene, Texture, Mesh, RenderLoop
│   │   ├── meshing/            — ChunkMesh (face-pair culling), Overlay
│   │   ├── primitives/         — LineRenderer, DotRenderer
│   │   └── effects/            — GradientBackground, RenderToTexture
│   ├── inputManagement/        — Camera, Collision, Raycasting
│   ├── uiManagement/           — UI system, text rendering
│   ├── sceneManagement/        — Scene lifecycle
│   └── memoryManagement/       — Save/load, model files
├── modelEditor/                — Editor application
│   ├── src/
│   │   ├── scenes/             — 3dModeler, vectorMesh, menu, createScene
│   │   ├── mechanics/
│   │   │   ├── interaction/    — Highlight, Selection
│   │   │   ├── export/         — GltfExporter, TexturePacking
│   │   │   └── AiHandling/     — Gemini agent: AiHandling, AiProcess,
│   │   │                         AiTools (C++) + agent.py (Python sidecar)
│   │   └── prefabs/            — Built-in meshes (cube, wedge)
│   ├── setUpAPI.bat            — one-shot API key setup from a shell
│   └── dist_run.bat            — shipped as build/Release/run.bat for
│                                 double-click launch
└── thirdparty/                 — Bundled headers (json.hpp, stb_image_write.h)
```
