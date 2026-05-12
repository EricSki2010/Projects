# VisualEngine API Reference

## Folder Structure

```
VisualEngine/
├── VisualEngine.h / .cpp          — Public API
├── EngineGlobals.h                — Global context struct
├── renderingManagement/
│   ├── render.h                   — Shader, Scene, Texture, Mesh classes
│   ├── DefaultShaders.h           — Default textured/solid shader + vertex-colored shader
│   ├── Shader.cpp, Scene.cpp, Texture.cpp, Mesh.cpp
│   ├── RenderLoop.cpp/h           — Frame loop: input → update → render
│   ├── meshing/
│   │   ├── ChunkMesh.cpp/h        — Face-pair culling, mesh merging
│   │   └── Overlay.cpp/h          — Triangle overlays for selection
│   ├── primitives/
│   │   ├── LineRenderer.cpp/h     — 3D line drawing
│   │   └── DotRenderer.cpp/h      — Billboard point drawing
│   └── effects/
│       ├── GradientBackground.cpp/h   — Screen-space gradient backdrop
│       └── RenderToTexture.cpp/h      — Framebuffer utilities
├── inputManagement/               — Camera, Collision, Raycasting
├── uiManagement/                  — UI system, text rendering
├── sceneManagement/               — Scene lifecycle
└── memoryManagement/              — Save/load, model files
```

## PUBLIC API
**Header:** `VisualEngine/VisualEngine.h`
**Implementation:** `VisualEngine/VisualEngine.cpp`

`VE::initWindow(width, height, title, maximized = false) -> bool`
  Creates a window with OpenGL 4.3 context. Initializes shader, scene, and input. Pass `true` for maximized to fill the screen.

`VE::setCamera(x, y, z, yaw, pitch)`
  Sets camera position and orientation.

`VE::loadMesh(name, MeshDef)`
  Registers a mesh from vertex/index data in memory.

`VE::loadMesh(name, meshFilePath)`
  Registers a mesh from a .mesh binary file on disk.

`VE::loadMeshDir(dirPath)`
  Scans a directory for all `.mesh` files and registers each one. The filename (without extension) becomes the mesh name.

`VE::setMode(mode)`
  Sets mesh build mode. SINGLE = one merged mesh with face culling. CHUNK = reserved for future chunked meshing.

`VE::draw(meshName, x, y, z, rx = 0, ry = 0, rz = 0)`
  Places a named mesh at a world position with optional rotation in degrees. Rotation is applied around the mesh's local origin before translation. Also creates a collider (rotated colliders use triangle-based raycasting).

`VE::undraw(x, y, z)`
  Removes the mesh and collider at a world position.

`VE::clearDraws()`
  Removes all placed meshes and colliders.

`VE::hasBlockAt(x, y, z) -> bool`
  Returns true if a collider exists at the given grid position.

`VE::rebuild()`
  Forces a mesh rebuild. Normally happens automatically when needsRebuild is set.

`VE::setBrightness(brightness)`
  Sets global brightness multiplier for the 3D shader. 1.0 = normal, lower = darker, higher = brighter. Does not affect UI.

`VE::setSpecularStrength(strength)`
  Sets the specular highlight strength for the 3D shader. Default is 0.5 (set in `Scene` constructor); lower values reduce glare/shininess, 0.0 gives a fully matte look. Does not affect UI. Like `setBrightness`, this writes directly to the active shader uniform — `RenderLoop::render()` does not reset it each frame, so the value persists until changed.

`VE::setGradientBackground(enable, top = {0,0,0}, bottom = {0.7,0.7,0.7})`
  Enables/disables a 3D gradient background that follows the camera view direction. Top color when looking up, bottom color when looking down.

`VE::setFarPlane(farPlane)`
  Sets the perspective projection's far clip distance for the active scene and rebuilds `ctx.scene->projection` immediately. Default is 500 (set in `Scene` constructor). The value is also re-applied on framebuffer resize and is read by `Camera::getProjectionMatrix`. Persists across scene swaps (each scene that cares should set its own value in `onEnter`, mirroring the `setFog` / `setBrightness` pattern).

`VE::registerScene(name, onEnter, onExit, onInput, onUpdate, onRender)`
  Registers a named scene with lifecycle hooks. onEnter receives `void* data`. See Scene Management section.

`VE::setScene(name, data = nullptr)`
  Switches to a registered scene. Optional `void* data` is passed to the new scene's onEnter.

`VE::setSceneCycleHotkey(key, modKey = 0)`
  Engine-managed hotkey that cycles through registered scenes on rising edge. See SCENE MANAGEMENT section for details.

`VE::run()`
  Starts the main loop: processInput -> update -> render -> swap. Blocks until window closes. Calls active scene's onExit before cleanup.


## ENGINE CONTEXT
**Header:** `VisualEngine/EngineGlobals.h`
**Defined in:** `VisualEngine/VisualEngine.cpp`

`EngineContext ctx` — Single global struct holding all engine state:
- `ctx.window` — GLFWwindow pointer
- `ctx.shader` — default Shader (textured / solid-color path)
- `ctx.vcShader` — vertex-colored Shader (pos+normal+color path, no texture/uniform color)
- `ctx.scene` — Scene (projection, view, lighting)
- `ctx.width`, `ctx.height` — current framebuffer dimensions
- `ctx.needsRebuild` — flag set by draw/undraw/setMode, consumed by update
- `ctx.mode` — SINGLE or CHUNK
- `ctx.mergedMeshes` — built mesh data ready for rendering
- `ctx.scrollDelta` — mouse scroll delta for the current frame (reset each frame)

Both shaders share the same static uniforms (light/view/brightness) uploaded
once at `run()` start. Vertex-colored meshes must be drawn with `ctx.vcShader`
bound, textured/solid meshes with `ctx.shader`.


## RENDERING
**Header:** `VisualEngine/renderingManagement/render.h`
**Implementations:** `Shader.cpp`, `Scene.cpp`, `Texture.cpp`, `Mesh.cpp`

`Shader(vertexSrc, fragmentSrc)`
  Compiles and links a shader program.
  `shader.loc(name) -> int` — returns cached uniform location.

`Scene(aspectRatio)`
  Holds projection matrix, view matrix, a PointLight, a Fog, and `farPlane` (perspective far clip, default 500). Mutate `farPlane` via `VE::setFarPlane()` so the projection matrix is rebuilt to match.
  `scene.uploadStaticUniforms(shader)` — uploads light properties and default brightness (once).
  `scene.uploadFrameUniforms(shader, model)` — uploads model/projection/normal matrices (per frame).

`Texture(filepath)`
  Loads an image via stb_image and creates a GL texture.

`Texture(pixels, width, height, channels)`
  Creates a GL texture from raw pixel data (NEAREST filtering, CLAMP_TO_EDGE).

  `texture.bind(unit)` — binds to a texture unit.

`Mesh(vertices, vertexCount, indices, indexCount)`
  Creates VAO/VBO/EBO from vertex data (position3 + uv2 = 5 floats, normals computed automatically). Default color is grey (0.8).

`Mesh(verticesWithNormals, vertexCount, indices, indexCount, hasNormals)`
  Creates VAO/VBO/EBO from vertex data with pre-computed normals (position3 + uv2 + normal3 = 8 floats). Skips normal computation.

`Mesh::createVertexColored(verts, vertexCount, indices, indexCount) -> Mesh*`
  Static factory. Input is interleaved pos3 + normal3 + color3 (9 floats per
  vertex). The returned mesh has `vertexColored = true` and must be drawn
  with `ctx.vcShader`. Used by any pipeline that ships color as a per-vertex
  stream (e.g. loading vertex-colored GLBs or future chunk meshes with
  COLOR_0). Caller owns the returned pointer.

`Mesh::createArenaSlice(sharedVAO, indexByteOffset, indexCount, baseVertex) -> Mesh*`
  Static factory for callers that manage their own pooled VAO/VBO/EBO (e.g.
  chunk streamers that want a single arena holding many meshes). The returned
  Mesh allocates **no** GL resources — it just records the shared VAO and the
  byte/vertex offsets into the arena, and `draw()` issues
  `glDrawElementsBaseVertex(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT,
  indexByteOffset, baseVertex)`. The destructor skips `glDelete*`. Caller
  must keep the underlying arena alive at least as long as the slice.
  `mesh.arenaSlice` is true for these; `texture`/`color`/`vertexColored`
  still work the same way.

  `mesh.setTexture(tex)` — assigns a texture.
  `mesh.setColor(color)` — sets a solid color.
  `mesh.vertexColored` — true for meshes built via `createVertexColored`;
    `draw()` skips the texture/objectColor uniforms entirely when set.
  `mesh.draw(shader)` — uploads alpha + (if not vertex-colored) texture/
    color uniforms, binds VAO, draws. Caller is responsible for having the
    matching shader bound.


## OVERLAY
**Header:** `VisualEngine/renderingManagement/meshing/Overlay.h`
**Implementation:** `VisualEngine/renderingManagement/meshing/Overlay.cpp`

`initOverlay()`
  Creates a reusable VAO/VBO for drawing single-triangle overlays.

`cleanupOverlay()`
  Deletes the overlay VAO/VBO.

`drawTriangleOverlay(shader, triangle, color, alpha, flatShade = true)`
  Draws a single triangle as a semi-transparent overlay on top of existing geometry. Uses polygon offset to prevent z-fighting. `flatShade = true` forces full ambient (unlit) for bright selection highlights. Pass `false` to use scene lighting (e.g. for mesh preview triangles).

`aabbFaceTriangle(box, faceIndex, half) -> Triangle`
  Converts one half of an AABB face into a triangle. faceIndex: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z. half: 0 or 1 for the two triangles of the quad.


## MESH BUILDING
**Header:** `VisualEngine/renderingManagement/meshing/ChunkMesh.h`
**Implementation:** `VisualEngine/renderingManagement/meshing/ChunkMesh.cpp`

`registerMesh(name, MeshDef)`
  Stores mesh vertex/index data in the internal registry.

`registerMeshFromFile(name, filepath)`
  Loads a .mesh binary and registers it.

`getRegisteredMesh(name) -> RegisteredMesh*`
  Looks up a registered mesh by name.

`addDrawInstance(meshName, x, y, z, rx = 0, ry = 0, rz = 0)`
  Adds a draw instance to the draw list with optional rotation in degrees.

`removeDrawInstance(x, y, z)`
  Removes the draw instance at a position.

`clearDrawInstances()`
  Clears all draw instances.

`buildSingleMeshes() -> vector<MergedMeshEntry>`
  Face-pair culling mesh builder. Calls `computeFaceCullSet()` for phases 1+2,
  then:
  - **Emit**: Walks `gDrawList`, transforms verts by instance rotation +
    position. For each triangle looks up its face direction via
    `reg.triFaceDir`, checks the cull set with O(1) lookup. Paint color for
    rectangular meshes is indexed by cardinal face direction (`fd`), for
    non-rectangular by triangle index (`t`). Painted triangles go to
    per-color buckets; unpainted go to a default bucket.

`computeFaceCullSet() -> FaceCullSet`
  Shared helper used by both the live renderer and the GLB exporter. Walks
  `gDrawList`, builds face entries from each instance's `faceState` (rotated
  to world space via `buildFaceRotMap`), and applies the 2v2 / 1v2 rules to
  produce the set of `(worldPos, worldFaceDir)` pairs that should be hidden.

`buildFaceRotMap(rotation, rotMap[6]) -> bool`
  Fills `rotMap[localFace] = worldFace` for a 90°-aligned rotation. Returns
  false if the rotation isn't a multiple of 90° on each axis.

`FaceKey { pos, face }` / `FaceKeyHash` / `FaceCullSet`
  Public types used by the cull-set API. A `FaceCullSet` is an
  `unordered_set<FaceKey, FaceKeyHash>`.

`buildMergedMeshes() -> vector<MergedMeshEntry>`
  Placeholder for future chunk-based meshing. Currently forwards to buildSingleMeshes.

`clearMeshData()`
  Clears both the mesh registry and draw list.

`registerMeshWithStates(name, vertices, vertexCount, interleavedIndices, triCount, faceStates = nullptr, texturePath = nullptr, isPrefab = false)`
  Registers a mesh with per-triangle face directions and per-face cull states. Index data is interleaved: v0,v1,v2,faceDir for each triangle. faceDir: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z, 0xFFFFFFFF=none. faceStates is a 6-element array of cull states per face direction: 0=open (invisible to culling), 1=partial (culled by neighbor's 2), 2=solid (culls neighbor's 1 or 2). Pass `isPrefab = true` to flag built-in meshes.

`setPaintPalette(palette[16])`
  Sets the 16-color paint palette. Creates/updates a palette texture used for per-face coloring. Painted triangles are rendered as separate color-batched meshes using objectColor.

`getPaintPalette() -> const glm::vec3*`
  Returns a pointer to the current 16-color paint palette.

`RegisteredMesh.floatsPerVertex`
  5 = pos3+uv2 (normals computed), 8 = pos3+uv2+normal3 (pre-computed normals).

`RegisteredMesh.isPrefab`
  True for meshes registered with `isPrefab = true`. Used by editors to block editing built-in shapes.

`RegisteredMesh.triFaceDir`
  Per-triangle face direction vector. Maps each triangle to a face (0-5) or -1 (no face).

`RegisteredMesh.faceState[6]`
  Per-face cull state: 0=open, 1=partial, 2=solid. Set by registerMeshWithStates or auto-set to 2 for rectangular meshes.


## COLLISION
**Header:** `VisualEngine/inputManagement/Collision.h`
**Implementation:** `VisualEngine/inputManagement/Collision.cpp`

`Triangle { v0, v1, v2 }`
  Three vertices defining a triangle.

`AABB { min, max }`
  Axis-aligned bounding box.

`BlockCollider { position, rotation, bounds, triangles, meshName, isRectangular, triColors }`
  A collider placed in the world. Rectangular colliders use AABB-only raycasting (disabled when rotated). Non-rectangular store individual triangles. triColors stores per-face/triangle paint palette indices (-1 = unpainted).

`CollisionHit { hit, point, normal, distance, collider, triangleIndex }`
  Result of a raycast. triangleIndex is an index into collider->triangles (mesh) or a face index 0-5 (AABB).

`isMeshRectangular(vertices, vertexCount) -> bool`
  Returns true if all vertices lie on the faces of their bounding box.

`addCollider(meshName, vertices, vertexCount, indices, indexCount, rectangular, x, y, z, floatsPerVertex = 5, rx = 0, ry = 0, rz = 0)`
  Creates a collider at a world position with optional rotation. Supports 5-float (pos+uv) or 8-float (pos+uv+normal) vertex stride. Rotated rectangular meshes fall back to triangle collision. Initializes triColors with -1 (unpainted).

`removeCollider(x, y, z)`
  Removes the collider at a position.

`clearColliders()`
  Removes all colliders.

`hasColliderAt(x, y, z) -> bool`
  Returns true if a collider exists at the grid position.

`getColliderAt(x, y, z) -> BlockCollider*`
  Returns the collider at a position, or null.

`getAllColliders() -> vector<BlockCollider>&`
  Returns all active colliders.

`raycast(origin, direction, maxDist=50) -> CollisionHit`
  Casts a ray against all colliders. Returns the closest hit. Respects forceRectangular mode.

`setForceRectangularRaycast(force)`
  When true, all colliders use AABB raycasting regardless of mesh shape. Used for build mode.

`isForceRectangularRaycast() -> bool`
  Returns whether force rectangular mode is active.


## RAYCASTING
**Header:** `VisualEngine/inputManagement/Raycasting.h`
**Implementation:** `VisualEngine/inputManagement/Raycasting.cpp`

`Ray { origin, direction }`
  A world-space ray.

`screenToRay(mouseX, mouseY, screenWidth, screenHeight, view, projection) -> Ray`
  Converts a screen-space mouse position to a world-space ray using unProject.

`LineHit { hit, point, distanceToLine, screenDistance, t }`
  Result of a ray-to-line test. point = exact 3D position on the line. t = 0-1 parameter along the line. screenDistance = pixel distance on screen.

`rayToLine(ray, lineFrom, lineTo, mouseX, mouseY, screenW, screenH, view, projection, threshold = 5.0) -> LineHit`
  Tests if a mouse click is near a 3D line segment. Uses 3D ray-to-segment closest point with screen-space pixel threshold. Returns the exact hit point on the line.

`TriangleHit { hit, distance, point }`
  Result of a ray-to-triangle test. point = exact 3D hit position on the triangle.

`rayToTriangle(ray, v0, v1, v2) -> TriangleHit`
  Möller-Trumbore ray-triangle intersection. Returns hit point and distance if the ray passes through the triangle.


## CAMERA
**Header:** `VisualEngine/inputManagement/Camera.h`
**Implementation:** `VisualEngine/inputManagement/Camera.cpp`

`CameraMode` enum: `CAMERA_FPS` (perspective + movement), `CAMERA_FLAT` (orthographic, no input).

`Camera { mode, position, yaw, pitch, sensitivity, moveSpeed, looking, ... }`
  FPS-style camera with mouse look and WASD movement. Supports two modes.

`camera.setMode(newMode)`
  Switches between FPS and FLAT mode. FLAT resets position to origin.

`camera.updateDir()`
  Recalculates the look direction from yaw/pitch.

`camera.processKeyboard(window, dt)`
  Handles WASD movement and Q to toggle mouse look. Disabled in FLAT mode. Movement only when Q is active. The Q toggle is suppressed while any UI text input is focused (via `isAnyInputFocused()`) so typing a literal "q" in a prompt doesn't grab the mouse.

`camera.getViewMatrix() -> mat4`
  Returns the current view matrix. Identity in FLAT mode.

`camera.getProjectionMatrix(aspectRatio) -> mat4`
  Returns perspective (FPS) or orthographic (FLAT) projection.

`Camera::mouseCallback(window, xpos, ypos)`
  GLFW mouse callback for look rotation. Disabled in FLAT mode.

`getGlobalCamera() -> Camera*`
  Returns the single global camera instance.


## RENDER LOOP
**Header:** `VisualEngine/renderingManagement/RenderLoop.h`
**Implementation:** `VisualEngine/renderingManagement/RenderLoop.cpp`

`processInput(dt)`
  Handles camera keyboard input and calls active scene's onInput.

`update()`
  Triggers rebuild if needed. Updates view matrix from camera. Calls active scene's onUpdate.

`render()`
  Re-binds 3D shader, resets brightness to 1.0, clears screen, draws gradient background if enabled, uploads uniforms, draws all meshes, calls active scene's onRender. Scenes that want dimming set brightness in their onRender each frame.


## SCENE MANAGEMENT
**Header:** `VisualEngine/sceneManagement/SceneManager.h`
**Implementation:** `VisualEngine/sceneManagement/SceneManager.cpp`

`SceneDef { onEnter(void*), onExit, onInput(float dt), onUpdate, onRender }`
  Defines a scene's lifecycle hooks. All are optional (nullable). onEnter receives data from setScene.

`VE::registerScene(name, onEnter, onExit, onInput, onUpdate, onRender)`
  Registers a named scene with its hooks. Nothing runs until setScene is called.

`VE::setScene(name, data = nullptr)`
  Switches to a scene. Calls onExit on the current scene, then onEnter on the new one with the provided data pointer.

`getActiveScene() -> SceneDef*`
  Returns the currently active scene definition, or nullptr.

`getActiveSceneName() -> string&`
  Returns the name of the currently active scene.

`cycleToNextScene()`
  Cycles to the next scene in registration order (wraps). No-op if fewer than 2 scenes registered or no scene is active. Used internally by `VE::setSceneCycleHotkey`.

`VE::setSceneCycleHotkey(key, modKey = 0)`
  Registers an engine-managed hotkey (`modKey + key`, both GLFW key codes) that cycles through registered scenes on rising edge — a held key only fires once. Pass `key=0` to disable. `modKey=0` requires no modifier. Passing a left/right variant of CONTROL/SHIFT/ALT accepts either side. The engine polls this once per frame in `processInput` before the active scene's `onInput`, so per-scene swap statics aren't needed (and would re-trigger across scenes anyway because each scene's static is only updated while that scene is active).


## MEMORY MANAGEMENT
**Header:** `VisualEngine/memoryManagement/memory.h`
**Implementation:** `VisualEngine/memoryManagement/memory.cpp`

`setMemoryPath(dir)`
  Sets the base directory for saving/loading binary files.

`saveToMemory(name, formatPath, data) -> bool`
  Packs a vector of comma-separated strings into a compact binary file using a format definition. Auto-detects 32 or 64 bit mode based on total bit width. RLE compressed. Saves to `{memoryPath}/{name}.bin`.

`loadFromMemory(name, formatPath) -> vector<string>`
  Reads a `.bin` file, RLE decompresses, unpacks each entry, and returns comma-separated strings matching the original data.

Format files define the bit layout per entry, one field per line:
```
x -> 4
y -> 4
z -> 4
id -> 10
rx -> 3
ry -> 3
rz -> 3
```


## MODEL FILES
**Header:** `VisualEngine/memoryManagement/ModelData.h`

`FaceColor { color }`
  RGB color for a triangle face. Default grey (0.8).

`BlockTypeDef { name, vertices, vertexCount, floatsPerVertex, indices, indexCount, faceColors }`
  Defines a custom block shape with per-triangle colors. floatsPerVertex = 5 (pos+uv) or 8 (pos+uv+normal).

`BlockPlacement { x, y, z, typeId, rx, ry, rz, triColors }`
  A placed block: position, which block type, rotation, and per-face/triangle paint palette indices.

`ModelFile { blockTypes, placements, palette[16] }`
  Complete model file containing block type definitions, all placements, and a 16-color paint palette.

`saveModel(name, model) -> bool`
  Saves a ModelFile to `{memoryPath}/{name}.mdl`. Version 3 format with magic header, floatsPerVertex per block type, block types section (vertices, indices, face colors), placements section (with triColors), and palette.

`loadModel(name, model) -> bool`
  Loads a `.mdl` file back into a ModelFile struct. Supports v1 (5-float), v2 (variable floatsPerVertex), and v3 (triColors + palette).

### Mesh File Formats

**.mesh (legacy):** `[vertexCount:u32][indexCount:u32][texPathLen:u32][vertices: vertexCount*5 floats][indices: indexCount u32s][texPath]`

**.mesh (VN):** `"VN"[vertexCount:u32][indexCount:u32][texPathLen:u32][vertices: vertexCount*8 floats (pos3+uv2+normal3)][indices][texPath]`
  Includes pre-computed normals. Detected by "VN" magic header.

**.mesh (VC):** `"VC"[vertexCount:u32][indexCount:u32][texPathLen:u32][vertices: vertexCount*8 floats][indices][texPath][triFaceDir: triCount int32s][faceState: 6 int32s]`
  VN layout + per-triangle face direction + 6 face cull states appended.
  Produced by the vectorMesh editor on save; lets custom meshes participate
  in face-pair culling the same way prefab cubes/wedges do. Detected by
  "VC" magic header.

**.vmesh:** Binary format for vector mesh editor data. Stores dots (vec3 array), lines (index pairs), triangles (index triples + flipped flag).


## UI SYSTEM
**Header:** `VisualEngine/uiManagement/UIElement.h`

`UIElement { id, position, size, color, textureId, label, labelScale, labelColor, onClick, visible, hoverable, isTextInput, focused, inputText, placeholder, maxLength, onUnfocus, multiline, numericOnly, caretPos, selAnchor, requireConfirm, confirmId }`
  UI element struct. Supports buttons, panels, images, and text inputs. Optional confirmation system via requireConfirm/confirmId. `hoverable` (default false) opts into the grow/lighten hover effect — when true, the element inflates by 5% and lightens by 0.1 RGB on hover, and click bounds match the inflated rect.

  **Text input fields:** `caretPos` (0..inputText.size()) is the insertion point. `selAnchor` is the other end of an active selection or `-1` if none — the selected range is `[min(selAnchor, caretPos), max(selAnchor, caretPos)]`. `multiline` enables word-wrapped rendering. `numericOnly` restricts input to digits, `.`, and `-`. `maxLength` caps inputText length.

**Header:** `VisualEngine/uiManagement/UIRenderer.h`
**Implementation:** `VisualEngine/uiManagement/UIRenderer.cpp`

`initUIRenderer()`
  Creates quad VAO/VBO and compiles the UI shader.

`cleanupUIRenderer()`
  Deletes UI GL resources and shader.

`drawUIElement(element)`
  Draws a single UI element as a colored/textured quad. Disables depth test, enables blending.

`getUIShader() -> Shader*`
  Returns the UI shader for custom UI drawing (e.g. color wheel triangles).

**Header:** `VisualEngine/uiManagement/UIManager.h`
**Implementation:** `VisualEngine/uiManagement/UIManager.cpp`

`UIGroup { id, visible, elements }`
  Named container of UI elements with shared visibility.

`addUIGroup(groupId, visible = true)`
  Creates a new UI group.

`removeUIGroup(groupId)`
  Removes a group and all its elements.

`addToGroup(groupId, element)`
  Adds a UI element to an existing group.

`removeFromGroup(groupId, elementId)`
  Removes a specific element from a group.

`setGroupVisible(groupId, visible)`
  Shows or hides all elements in a group.

`getUIElement(groupId, elementId) -> UIElement*`
  Returns a pointer to a specific element, or nullptr.

`clearUI()`
  Removes all groups and elements.

`renderUI()`
  Draws all visible groups/elements. Renders labels centered on buttons. Renders text input content with a solid-white blinking caret drawn as an overlay glyph (does not displace surrounding characters between blink phases). Active selections render as a soft-blue translucent rect behind the selected text; multi-line selections split per wrapped line. Highlights focused inputs and pending confirm buttons (+0.15 RGB). Hoverable elements under the cursor are drawn inflated by 5% with +0.1 RGB and 1.05× label scale; this stacks with the focus/pending tint if both apply.

`processUIInput()`
  Updates hover state, handles left-click detection (with click-position caret hit-testing on text inputs), tracks left-button drags inside focused inputs to extend selection, and processes keyboard input for focused text fields.

  **Text input keys:**
  - **Printable ASCII** (typed via GLFW char callback): inserts at caret. Replaces any active selection.
  - **Backspace**: deletes the active selection if any, else deletes the char left of caret. Held: 400ms initial delay then ~30Hz repeat.
  - **Left / Right**: moves the caret. With selection and no shift, collapses to the corresponding end. Held: same key-repeat cadence as Backspace.
  - **Shift + Left / Right / Home / End**: extends selection (anchors at current caret if no selection yet).
  - **Home / End**: jumps caret to start / end.
  - **Ctrl + A**: selects all.
  - **Ctrl + C**: copies the selected text via `glfwSetClipboardString` (no-op if nothing selected).
  - **Ctrl + X**: copies selection then deletes it.
  - **Ctrl + V**: deletes any selection, then pastes `glfwGetClipboardString` filtered through the same per-char rules as typing (printable ASCII, `numericOnly`, `maxLength`). CR/LF are stripped. The just-pasted span is left selected so the user can immediately retype to replace or arrow-away to deselect.

`handleUIClick(mouseX, mouseY, screenWidth, screenHeight) -> bool`
  Hit tests all visible elements back-to-front. Hoverable elements use the inflated (5%) bounds so clicks match the hover visual; non-hoverable elements use their raw bounds. Handles focus, confirmation system, and onClick. Clicking a text input also hit-tests the click x (and y for multi-line) to position the caret at the clicked column, leaves `selAnchor = -1`, and arms the drag tracker so subsequent mouse motion extends a selection. Returns true if something was hit.

`isPointOverUI(mouseX, mouseY, screenWidth, screenHeight) -> bool`
  Returns true if the screen point is over any visible interactive element. Decorative elements (no onClick, not a text input, no confirmId/requireConfirm) are skipped — so full-panel backgrounds don't block clicks on raw-GL UI like the color wheel.

`getInputText(groupId, elementId) -> string`
  Returns the current text in a text input element.

`isAnyInputFocused() -> bool`
  True if any visible text input in any group currently has focus. Scenes use this to gate hotkeys (Q for camera look, Space for save+transition) so typing those characters in a prompt doesn't trigger the action.

`inputWrappedLineCount(element) -> int`
  For a `multiline` text input, returns how many lines the renderer will draw given the element's current `inputText`, width (from `size.x`, window-size aware), and `labelScale`. Scenes use this to size a growable input box in sync with the renderer's word-wrap.

`hasPendingConfirm() -> bool`
  Returns true if a button is waiting for confirmation.

`getPendingConfirmId() -> string`
  Returns the confirmId of the pending action.

`cancelPendingConfirm()`
  Cancels any pending confirmation.

**Header:** `VisualEngine/uiManagement/UIPrefabs.h`
**Implementation:** `VisualEngine/uiManagement/UIPrefabs.cpp`

`createButton(id, x, y, w, h, color, label, onClick) -> UIElement`
  Creates a button with centered text label. `hoverable = true`.

`createPanel(id, x, y, w, h, color) -> UIElement`
  Creates a solid colored rectangle. Not hoverable — treated as decorative by `isPointOverUI`.

`createImage(id, x, y, w, h, textureId) -> UIElement`
  Creates a textured rectangle. Not hoverable by default; set `hoverable = true` on the returned element to opt in. Assigning an `onClick` promotes it to interactive for `isPointOverUI` purposes.

`createTextInput(id, x, y, w, h, color, placeholder, maxLength) -> UIElement`
  Creates a clickable text input field with placeholder text. Text inputs are always excluded from hover (they keep their own focused-lighter state). Supports caret positioning by click, click-and-drag selection, shift+arrow selection extension, Ctrl+A/C/X/V, Home/End, and arrow/backspace key repeat — see `processUIInput()` for the full key list. Set `element.multiline = true` after creation to enable word-wrapped rendering: text wraps by pixel width, lines stack top-down within the element. Set `element.numericOnly = true` to restrict input to digits, `.`, and `-`. Scenes that want the box to grow with content poll `inputWrappedLineCount(element)` each frame and set `size.y` accordingly.

`createSubButton(id, parentX, parentY, parentW, parentH, anchorX, anchorY, widthRatio, heightRatio, padding, color, label, onClick) -> UIElement`
  Creates a button positioned relative to a parent element. anchorX/anchorY (0-1) control alignment. widthRatio/heightRatio are fractions of parent size. padding is fraction of parent height. `hoverable = true`.

`createDropdown(...)` (see UIManager.h)
  Creates a toggle button whose onClick shows/hides a group of option buttons. Both the toggle and option buttons are `hoverable = true`.


## LINE RENDERER
**Header:** `VisualEngine/renderingManagement/primitives/LineRenderer.h`
**Implementation:** `VisualEngine/renderingManagement/primitives/LineRenderer.cpp`

`initLineRenderer()`
  Creates VAO/VBO and compiles the line shader.

`cleanupLineRenderer()`
  Deletes line GL resources and shader.

`drawLine(from, to, color, width = 1.0)`
  Draws a single line between two 3D world-space points.

`drawLines(points, color, width = 1.0, loop = false)`
  Draws connected lines through a list of points. loop = true connects last to first.


## DOT RENDERER
**Header:** `VisualEngine/renderingManagement/primitives/DotRenderer.h`
**Implementation:** `VisualEngine/renderingManagement/primitives/DotRenderer.cpp`

`initDotRenderer()`
  Creates a billboard quad VAO/VBO and compiles the dot shader.

`cleanupDotRenderer()`
  Deletes dot GL resources and shader.

`drawDot(position, size, color)`
  Draws a billboard quad at a 3D position that always faces the camera. size is radius in world units. Draws on top of everything (depth test disabled).


## RENDER TO TEXTURE
**Header:** `VisualEngine/renderingManagement/effects/RenderToTexture.h`
**Implementation:** `VisualEngine/renderingManagement/effects/RenderToTexture.cpp`

`RenderTarget { fbo, textureId, depthRbo, width, height }`
  Framebuffer object with color texture and depth buffer.

`createRenderTarget(width, height) -> RenderTarget`
  Creates a framebuffer with color texture and depth renderbuffer.

`bindRenderTarget(rt)`
  Switches rendering to the render target's framebuffer.

`unbindRenderTarget(screenWidth, screenHeight)`
  Switches back to the default framebuffer (screen).

`destroyRenderTarget(rt)`
  Deletes the framebuffer, texture, and depth buffer.


## GRADIENT BACKGROUND
**Header:** `VisualEngine/renderingManagement/effects/GradientBackground.h`
**Implementation:** `VisualEngine/renderingManagement/effects/GradientBackground.cpp`

`initGradientBackground()`
  Creates fullscreen quad and compiles gradient shader. Called automatically by engine init.

`cleanupGradientBackground()`
  Deletes gradient GL resources. Called automatically on engine shutdown.

`setGradientColors(top, bottom)`
  Sets the gradient top and bottom colors.

`enableGradientBackground(enable)`
  Toggles the gradient on/off.

`isGradientBackgroundEnabled() -> bool`
  Returns whether the gradient is currently enabled.

`drawGradientBackground()`
  Draws the gradient as a fullscreen quad behind everything. Called automatically each frame in render(). Uses inverse view-projection to map screen pixels to world-space view directions.


## TEXT RENDERING
**Header:** `VisualEngine/uiManagement/TextRenderer.h`
**Implementation:** `VisualEngine/uiManagement/TextRenderer.cpp`

`initTextRenderer(fontPath, fontSize) -> bool`
  Loads a .ttf font via FreeType, rasterizes ASCII glyphs into textures.

`cleanupTextRenderer()`
  Deletes all glyph textures and the text shader.

`drawText(text, x, y, scale, color)`
  Draws a string at pixel coordinates (top-left origin). Each character is a textured quad.

`measureText(text, scale) -> float`
  Returns the width in pixels of a rendered string.

`measureTextHeight(scale) -> float`
  Returns the height in pixels of capital letters at the given scale.
