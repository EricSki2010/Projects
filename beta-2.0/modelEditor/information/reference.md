# modelEditor Reference

## Scenes

### 3dModeler (`src/scenes/3dModeler.cpp`)
Main editor scene with two modes: Build and Paint.

**Entry:** Receives model name via `void* data` (std::string*). Loads `.mdl` file, restores palette and placements.

#### Sidebar UI
- Right panel: x=0.6, width=0.4, full height
- **Block selector grid** (5×3, 15 slots): Bottom of panel, build mode only. Slots 0-1 are cube/wedge. Slots 2-14 load from `assets/saves/vectorMeshes/slot_N.mesh`. Click any slot to select it (empty slots select without changing the current mesh). Hovering an unselected slot swaps its base icon from the `+` texture to the `~` hover texture; the selected slot always shows the `-` (yellow-outline) texture.
- **Edit Object / Edit Color button**: Switches label based on editor mode.
- **Editor Mode dropdown**: Build / Paint toggle.
- **Color Mode dropdown** (paint mode, inside color edit panel): RGB / Hex.

#### Build Mode (sEditorMode = 0)
- Place blocks with current mesh selection
- Right-drag: Rectangle face selection
- Tab: Toggle block-select mode (purple)
- Ctrl+D: Delete selected blocks
- Ctrl+A (block mode): Replace with current mesh
- Ctrl+A (face mode): Extrude from selected faces
- R + 1/2/3: Rotate selected blocks by 90° on X/Y/Z
- Ctrl+Z: Undo (50-step history)
- Ctrl+Tab: Edit current slot in vectorMesh editor (skips prefabs)
- Scroll: Cycle through mesh slots

#### Paint Mode (sEditorMode = 1)
- Left-drag: Brush select triangles/faces (immediate)
- Right-click: Select single triangle/face
- Right-drag: Plane select (coplanar triangles, commit on release)
- Ctrl+A: Paint selection with current palette color
- Ctrl+Z: Undo (50-step history)
- Scroll: Cycle through 16 color slots
- Shift: Additive selection. Ctrl: Subtractive selection.

#### Color Wheel
- 16 triangular slices in bottom-right corner (quarter circle, center at screen corner)
- Radius: 0.6 NDC
- Only visible in paint mode
- Selected slice: yellow outline at line width 3, drawn on top
- Hovered slice: grey (0.7) outline at line width 3; suppressed when hovered == selected
- Click a slice to select it (aspect-corrected hit test, skipped if cursor is over an interactive UI element as reported by `isPointOverUI`)

#### Color Editing
- Click "Edit Color" → hides editor mode dropdown, shows color edit panel
- Color Mode dropdown: RGB or Hex
- RGB: 4 inputs (R, G, B, Bri) 0-255. Color = (R/255, G/255, B/255) × (Bri/255)
- Hex: 2 inputs (hex code, Bri). Hex is 6 chars (RRGGBB).
- "Done" button: Saves color to palette, updates palette texture, triggers rebuild, closes panel
- Pre-fills inputs with current color (empty if default grey)

#### Pause Menu
- ESC toggles. Blocks all input except ESC and menu clicks.
- Camera forced out of looking mode while paused.
- Buttons: Continue / Export As GLB / Save & Exit.

#### Export Dialog
- Opened from pause menu "Export As GLB" button.
- **Name** input (pre-filled with current model name).
- **Path** input (optional, defaults to `assets/exports`).
- **Done**: writes `{path}/{name}.glb` via `exportModelToGlb`.
- **Cancel**: closes dialog without exporting.

#### Auto-Save
- Current model is saved on scene exit / window close via the `onExit` handler.

#### Model Save/Load
- **Save:** Palette → `sCurrentModel.palette`. Colliders → placements (position, rotation, meshName, triColors). Writes `.mdl` v3.
- **Load:** Reads `.mdl`, restores palette to color wheel, registers block types (VN meshes loaded from `.mesh` files), places blocks with rotation, restores triColors onto colliders.

#### VectorMesh Editor Return
- `sPendingSlotUpdate` / `sPendingSlotMesh` set before switching to vectorMesh scene.
- On return: checks for `.mesh` file, re-registers mesh, creates preview, rebuilds selector icons.

---

### vectorMesh (`src/scenes/vectorMesh.cpp`)
Editor for creating custom meshes from dots, lines, and triangles.

**Entry:** Receives `VectorMeshEditData` with meshName, slotIndex, modelName.

#### Modes
- **Vector (dot) mode**: Place 3D points with snap-to-edge. Snaps to cube corners, existing dots, and interpolated points along edges/lines. Snap count configurable. Clicking a placed dot selects it (magenta highlight) and opens a "Selected Vertex" panel in the right sidebar with live-editable X/Y/Z text inputs (`numericOnly`, prefilled with current coords). Each frame the inputs are parsed and written back to `sPlacedDots[sSelectedDotIndex]`; one undo snapshot per focus session. Index-based references in lines/triangles follow the move automatically. Ctrl+D on the *selected* dot clears the selection; on a *different* dot, the selection's index is remapped across the shift.
- **Line mode**: Select 2 dots → Ctrl+A to create edge. Ctrl+D to delete hovered line.
- **Plane mode**: Select 3 dots → Ctrl+A to create triangle. Triangle multi-select: plain click replaces, Ctrl+click toggles, Shift+click adds, click empty space clears. "Flip Normal(s)" and "Subdivide" buttons act on the whole selection (label includes count when >1). Subdivide replaces each selected tri with 4 children (midpoint-vertex sharing via `subdivideTriangleIndices`, AI tool `sd` calls the same helper); originals stay at their old index, 3 new appended each, and the new children are added to the selection. Ctrl+D deletes the selection if non-empty, else the hovered triangle.

#### Triangle Preview
- Triangles render with the winding order as placed (A→B→C). No auto
  normal-detection heuristic. Use "Flip Normal" to override manually.
- Front and back both drawn so triangles are visible from any angle.

#### File Formats
- **.vmesh**: Binary. Dots (vec3 array), lines (index pairs), triangles (index triples + flipped flag).
- **.mesh export**: Converts triangles to pos3+uv2+normal3 vertices. "VC" magic header (VN + culling data). Normals flipped (`-cross`) on save so the default outward direction matches the user's CW click convention. Each triangle gets 3 unique vertices. Per-triangle `faceDir` and per-side `faceState[6]` computed at export time:
  - A triangle with all 3 verts on a cardinal side plane (±0.5) and inside the 1×1 face bounds is tagged with that side (0..5).
  - Each side's state is derived from summed projected areas of its flat triangles: `≥0.99` → state 2 (solid), `>0` → state 1 (partial), else 0 (open).
  - Non-flat triangles are tagged with the nearest still-open side, or -1 if no open side exists. These are never culled at runtime.
  - This data feeds directly into `RegisteredMesh.triFaceDir` / `faceState` so custom shapes participate in face-pair culling like prefab cubes.

#### Pause Menu
- "Continue", "Save & Exit" (saves .vmesh + exports .mesh + returns to 3dModeler or menu in one button).

#### Shortcuts
- Ctrl+Z: Undo (50-step history across dots, lines, triangles, flips)
- Ctrl+Tab: Save + export .mesh + return to 3dModeler

---

### menu (`src/scenes/menu.cpp`)
Main menu with Create New, Load (file browser with delete/duplicate), and Exit.

### createScene (`src/scenes/createScene.cpp`)
New model wizard. Type dropdown (3D Model), name input, template selection (None / Starting Cube with size 1-64).

---

## Mechanics

### Highlight (`src/mechanics/interaction/Highlight.cpp`)
Handles all selection rendering and input for both Build and Paint modes.

#### Selection Rendering
- **Block select (purple)**: Full block outline, visible faces only
- **Paint select (green)**: Individual triangles or AABB faces
- **Face select (blue)**: AABB face quads
- **Hover (yellow)**: Single face/triangle under cursor

#### Face Index Mapping
Face state indices: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
Raycast returns face indices in the same order.
Each triangle is tagged with a `faceDir` (0-5 or -1=none) at mesh registration.
Rotation is handled by `rotMap`: each local face direction is rotated to world space during the face-pair collection phase.

#### Paint Plane Selection
Stores starting triangle's normal and plane distance (`dot(normal, center)`). Only selects triangles with matching normal (dot product > 0.95) and plane distance (within 0.1f).

### Selection (`src/mechanics/interaction/Selection.cpp`)
Simple vector storage of `SelectedFace { blockPos (ivec3), faceIndex (int) }`. Deduplicates on add.

### GltfExporter (`src/mechanics/export/GltfExporter.cpp`)
`exportModelToGlb(outPath)` — exports the current scene as a glTF Binary file.

**One primitive, one material, per-vertex colors.** Single unlit double-sided
material; colors travel on the `COLOR_0` vertex attribute. No per-color
buckets, no per-model textures.

**Three-phase pipeline:**

1. **Face-pair cull** — calls `computeFaceCullSet()` (shared with the editor's
   live renderer) to compute `(pos, worldFace)` pairs hidden by adjacent
   block faces. Applies to any mesh whose `reg.faceState[f] > 0`, which now
   includes vectorMesh shapes saved in VC format.
2. **Greedy mesh (rectangular axis-aligned cubes only)** — collects each
   cube's visible face cells into per-plane grids keyed by
   `(worldFace, planeIntCoord)`. Runs 2D greedy meshing per plane: walks
   cells in (v, u) order, extends the largest same-color rectangle starting
   at each unprocessed cell (width first, then height), emits each rectangle
   as one merged quad (4 verts + 6 indices with intra-quad vertex sharing).
   Winding is auto-corrected by comparing the RH cross to the desired face
   normal. Cubes that aren't 90°-aligned or on integer grid fall through to
   the next phase.
3. **Per-triangle emit** — wedges, vectorMesh custom shapes, rotated/off-grid
   cubes. Transforms verts into world space, looks up per-triangle face
   direction via `reg.triFaceDir`, skips culled triangles, emits each
   triangle individually. VN meshes (fpv == 8) use their stored per-vertex
   normals (rotated by the collider's rotation). Prefabs (fpv == 5) compute
   normals from the winding via `cross(v1-v0, v2-v0)`.

**Color lookup:** rectangular meshes index `col.triColors[fd]` (cardinal face
direction); non-rectangular meshes index `col.triColors[t]` (triangle index).
Unpainted (palette index < 0) falls back to `kUnpaintedColor = grey(0.8)`.

**Output:** single `.glb` file (12-byte header + JSON chunk + BIN chunk). Four
accessors per primitive: POSITION, NORMAL, COLOR_0, indices. Uses
nlohmann/json (bundled in `beta-2.0/thirdparty/json.hpp`). Logs
`[gltf] Exported N tris (M merged quads, K culled), V verts -> path`.

---

## Prefabs

### prefab_cube.h
24 vertices (4 per face), 5 floats each (pos3+uv2). Centered at origin, 1×1×1.
Index format: `v0, v1, v2, faceDir` per triangle.
All faces assigned to their direction (0=+X through 5=-Z).
Face states: all 6 faces = state 2 (solid).
Plain indices version for ghost block rendering.

### prefab_wedge.h
18 vertices, 8 triangles. Wedge/ramp shape.
Face states: bottom(-Y)=2, back(-Z)=2, left(-X)=1, right(+X)=1, slope/top=0.
Slope and triangular side faces have faceDir=none (0xFFFFFFFF) — invisible to culling.
Left/right partial faces (state 1) are culled when adjacent to a state 2 face.

### EmbeddedSelectors.h
Embedded 32×32 PNG data for three block-selector icons: `+` (unselected, unhovered), `-` (selected, yellow outline), and `~` (hovered, unselected).

### templates.cpp
`createCubeTemplate(size)`: Fills size³ volume with cube blocks.

---

## Shared Data

### SceneData.h
```
VectorMeshEditData { meshName, slotIndex, modelName }
```
Passed from 3dModeler to vectorMesh scene on "Edit Object".

---

## Controls Summary

| Key/Mouse | Build Mode | Paint Mode |
|-----------|-----------|------------|
| Right-drag | Rectangle face select | Plane select (coplanar) |
| Left-drag | — | Brush select triangles |
| Right-click | Select single face | Select single triangle |
| Tab | Toggle block select (purple) | — |
| Ctrl+A | Replace (block) / Extrude (face) | Paint with current color |
| Ctrl+D | Delete selected blocks | — |
| Ctrl+Z | Undo (50-step history) | Undo (50-step history) |
| Ctrl+Tab | Edit current slot in vectorMesh | — |
| R + 1/2/3 | Rotate 90° on X/Y/Z | — |
| Scroll | Cycle mesh slots | Cycle color slots |
| ESC | Pause menu | Pause menu |
| Q | Toggle FPS camera | Toggle FPS camera |
| WASD | Move (when Q active) | Move (when Q active) |
| Space/Shift | Up/Down (when Q active) | Up/Down (when Q active) |

Q and Space (save+transition) are both suppressed while any text input is
focused, so typing those characters in a prompt never triggers the camera
or save action.

---

## AI Agent (`src/mechanics/AiHandling/`)

Optional Gemini-powered assistant. Off by default; toggle from the main
menu. When off, none of the AI code runs and no UI is shown.

### C++ layer

| File | Role |
|---|---|
| `AiHandling.h/.cpp` | Public facade. `init`, `shutdown`, `update(dt)`, `submitPrompt`, `submitPromptWithContext`, `isEnabled` / `setEnabled`, status accessors. Polls `getActiveSceneName()` each frame and emits `scene_change` events to the sidecar when the scene changes (outside OR inside a tool call). |
| `AiProcess.h/.cpp` | Windows subprocess wrapper. `CreateProcessA` with redirected stdin/stdout. A reader thread blocks on stdout, pushes each line onto a mutex-protected inbox; main thread drains via `drainInbox(cb)` once per frame. |
| `AiTools.h/.cpp` | Tool registry. `registerTool(name, lambda)` — scenes register during their `register*Scene()` so lambdas capture file-scope statics. `dispatchTool(name, args)` looks up, invokes, catches exceptions into `{ok:false, error:"..."}`. |

### Python sidecar (`agent.py`)

Runs the Gemini tool-calling loop with `google-genai`. Maintains history
manually (no `chats.create()`) so per-request `tools=[...]` can follow the
active scene. Features:

- **Scene-filtered tools** — `SCENE_TOOLS` map per scene; Send + Info adds `CONTEXT_TOOLS` for the current scene.
- **Short-name aliases** — `SHORT_NAMES` (e.g. `vmesh_add_vertices` → `av`) rewrites tool names on the wire. Translation back to long names happens in `run_prompt` before calling C++.
- **Retries** — 1/3/7 s backoff on transient errors (503, 429, UNAVAILABLE, RESOURCE_EXHAUSTED); up to 2 silent retries on MALFORMED_FUNCTION_CALL / OTHER / FINISH_REASON_UNSPECIFIED.
- **Cost accounting** — accumulates `prompt_token_count`, `candidates_token_count`, `cached_content_token_count` across every generate_content call in the prompt. `[$0.00057 | 1840->124 tok]` appended to each `final` message. Pricing in `MODEL_PRICING`.
- **Output cap** — `max_output_tokens = 32768` so flash doesn't bail out of large `bd` calls.

### Protocol (JSONL over stdio)

```
host -> sidecar:
  {"type":"prompt", "text":"...", "with_context":bool}
  {"type":"tool_result", "call_id":"...", "name":"...", "ok":bool, "result":<any> | "error":"..."}
  {"type":"scene_change", "scene":"3dModeler" | "vectorMesh" | "menu"}
  {"type":"shutdown"}

sidecar -> host:
  {"type":"ready"}
  {"type":"tool_call", "call_id":"...", "name":"<short alias>", "args":{...}}
  {"type":"final", "text":"..."}
  {"type":"log", "text":"..."}
  {"type":"error", "text":"..."}
```

Scene changes are emitted both by the per-frame poll in `AI::update` and
by `handleLine` immediately after any tool dispatch that changes the scene
(so the change lands in the host → sidecar stream before the `tool_result`
that would trigger the next request).

### Persistent files (next to the exe)

| File | Contents |
|---|---|
| `ai_enabled.txt` | `1` (on) or `0` (off). Missing ⇒ OFF. Managed by `AI::setEnabled`. |
| `.gemini_key` | User's Gemini API key. Read by `run.bat` into `GEMINI_API_KEY` on launch, OR set at runtime via the Set API Key menu (which also calls `_putenv_s` + sidecar restart so the change is live). |

### Menu integration (`src/scenes/menu.cpp`)

Two new buttons above Exit:
- **AI: ON / AI: OFF** — `AI::setEnabled(!AI::isEnabled())` + `showMainMenu()` to refresh the label. Live-applies: turns the sidecar off/on immediately.
- **Set API Key** — opens a sub-view with a text input + Save + Back. Save trims trailing whitespace, writes `.gemini_key`, sets env var via `_putenv_s`, cycles the sidecar (`AI::shutdown()` + `AI::init()`), returns to main menu.

### Scene integration

Both 3dModeler and vectorMesh, when `AI::isEnabled()`, construct a left-side
`ai_panel` UI group with:
- Dark background panel (full-height on the left, mirror of the right sidebar)
- `ai_response` framed box containing 8 transparent `ai_line_i` label rows
- Multi-line prompt input (`multiline = true`; grows downward from anchor y=0.48)
- **Send** button (always) and **Send + Info** button (3dModeler only)

Per frame in `onInput`:
1. `AI::update(dt)` — drain sidecar, dispatch tool calls.
2. Recompute prompt height from `inputWrappedLineCount(*input)`, anchor top at 0.48, slide Send (and Send + Info in 3dModeler) below.
3. Word-wrap the current response (or status) into the 8 rows; truncate with "..." on overflow.

### Tool catalogue

**Always available in 3dModeler:**
- `gs` — get_current_scene
- `sp` — set_palette_color(slot 0..15, r/g/b 0..1)
- `gp` — get_palette
- `ps` — select_color_slot
- `es` — edit_slot (transitions into vectorMesh)

**Send + Info adds (3dModeler only):**
- `cp` — get_camera_position (x, y, z, yaw, pitch)
- `cf` — get_camera_forward (unit vector)
- `ab` — get_aimed_block (ray-AABB pick, returns `{hit, x, y, z, face, distance}`)
- `lb` — list_blocks

**Always available in vectorMesh:**
- `bd` — vmesh_build (one-shot: clear + add verts + add tris + auto-fix normals + save)
- `av`, `at` — batch add vertices / triangles
- `dt`, `dv` — batch delete triangles / vertices (vertex delete cascades to triangles)
- `sd` — subdivide_triangles (4 children per triangle, shared midpoints, watertight)
- `pv` — perturb_vertices (random offset × magnitude)
- `sf` — scale_from (origin + factor, optional indices)
- `ft` — flip_triangle
- `lv`, `lt` — list vertices / triangles (rendered-normal convention)
- `sv` — save mesh
- `cl` — clear
- `af` — auto_fix_normals (union-find components, centroid heuristic, airtight check)
- `fn` — finish (save + transition back to 3dModeler)

All tools register with a `require3dModeler` / `requireVMesh` scene guard
that throws if called from the wrong scene.

### Normal direction contract

`vmesh_list_triangles` (`lt`) reports normals using `cross(C-A, B-A)` with
`flipped` inverting, which matches the export's blanket flip in
`exportVMeshToMesh`. The AI's idea of "outward" agrees with what gets
drawn — flipping a triangle the AI reports as inward actually makes it
outward in the render.
