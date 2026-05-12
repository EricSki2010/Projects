# PerlinNoiseVisualizer API Reference

A two-scene demo built on top of the VisualEngine that uses Perlin noise to drive a 3D voxel world and a 2D biome heatmap.

## Folder Structure

```
PerlinNoiseVisualizer/
├── main.cpp                      — Entry point, scene registration, window setup
├── scene.h / scene.cpp           — "visualizer" scene: 3D voxel world + 3-stage worker pipeline
├── noise2D.h / noise2D.cpp       — "noise2D" scene: top-down biome heatmap quad
├── biome.h / biome.cpp           — Biome classification, cached lookup, amplitude lookup
├── perlinNoiseManagement.h/.cpp  — Thin namespace wrapper around stb_perlin
├── stb_perlin.h                  — Vendored stb_perlin (impl defined in perlinNoiseManagement.cpp)
├── CMakeLists.txt                — Build config, vcpkg toolchain
├── build.bat / run.bat           — Convenience scripts
└── information/                  — This folder
```

## ENTRY POINT
**File:** [main.cpp](../main.cpp)

`int main()`
  Initializes the window at 1280×800, disables colliders, reserves 100×100×100 voxel block capacity, registers the `cube` mesh, switches the engine to `VE::CHUNK_VOXEL` mode, sets Perlin scale to 0.050, registers both scenes, binds `Ctrl+2` as the scene-cycle hotkey, and starts on the `noise2D` scene with seed `10`.

`static void setScene(const char* name, int seed)`
  Local wrapper that **always** sets the PerlinNoise seed before calling `VE::setScene`. Use this — never `VE::setScene` directly — so the noise system stays consistent with the active scene.

## PerlinNoise NAMESPACE
**Header:** [perlinNoiseManagement.h](../perlinNoiseManagement.h)
**Implementation:** [perlinNoiseManagement.cpp](../perlinNoiseManagement.cpp)

Thin wrapper around `stb_perlin_noise3_seed` and `stb_perlin_fbm_noise3`. All inputs are pre-multiplied by the current scale before sampling, so callers pass raw world coordinates.

`PerlinNoise::setSeed(int seed)`
  Sets the global seed used by every sample call that doesn't pass an explicit seed override.

`PerlinNoise::setScale(float scale)`
  Sets the global frequency multiplier applied to every coordinate before sampling. Lower = larger/smoother features. Default is 0.1; `main.cpp` sets 0.050.

`PerlinNoise::getSeed() -> int`
  Returns the current global seed. Used by [biome.cpp](../biome.cpp) to derive a temperature seed offset.

`PerlinNoise::sample2D(float x, float y) -> float`
  Returns Perlin noise in roughly [-1, 1] at the 2D point `(x, y)` using the global seed.

`PerlinNoise::sample2D(float x, float y, int seed) -> float`
  Same as above but overrides the seed for this call. Used to sample multiple independent fields (e.g. temperature) at the same point.

`PerlinNoise::sample3D(float x, float y, float z) -> float`
  3D Perlin sample using the global seed.

`PerlinNoise::sample3D(float x, float y, float z, int seed) -> float`
  3D Perlin sample with a per-call seed override.

`PerlinNoise::fbm2D(float x, float y, int octaves = 4, float lacunarity = 2.0f, float gain = 0.5f) -> float`
  Fractal Brownian motion at `(x, y)` — octave-summed Perlin. Defined but not currently called by any scene.

## BIOME API
**Header:** [biome.h](../biome.h)
**Implementation:** [biome.cpp](../biome.cpp)

Six-biome classifier driven by two independent noise fields (height, temperature). The temperature field uses `getSeed() + 1000` as its seed so the two fields are uncorrelated.

```cpp
enum class Biome { Sand, Shrub, Forest, Grass, Rock, SnowRock };
```

`Biome getBiome(float x, float z)`
  Pure function. Samples the height and temperature fields at world `(x, z)` and runs the threshold cascade in [biome.cpp](../biome.cpp). Same input always returns the same output for a given seed/scale.

`const char* biomeName(Biome b)`
  Returns a static C-string (`"Sand"`, `"Forest"`, …). Safe to compare with `==` against literals or store as a key.

`float biomeTopRampDepth(Biome b)`
  Maximum elevation (in world units) above `Y_TOP_RAMP_START` the surface can reach for this biome. Sand caps at 4, Rock at 50. The 3D scene IDW-blends this between neighboring biomes so amplitude transitions are smooth across boundaries (see [architecture.md](architecture.md)).

`Biome getCachedBiome(int x, int z)`
  Thread-safe cached `getBiome` keyed on integer world coords. Reads take a shared lock; new entries take an exclusive lock to insert. Cache is hard-capped at 500,000 entries (~30 MB) and cleared wholesale on overflow — misses are cheap (3 Perlin samples + cascade) so LRU bookkeeping isn't worth the cost.

`void clearBiomeCache()`
  Drops every entry. Called from `scene.cpp::onExit` and on seed change.

`void reserveBiomeCache(size_t expectedEntries)`
  Pre-allocates hash-map buckets so a streaming scene doesn't rehash mid-load. `scene.cpp::onEnter` calls this with 50,000.

`size_t biomeCacheSize()`
  Cheap shared-locked load of the current entry count. Surfaced in the per-100-chunk benchmark line.

## SCENE REGISTRATION
**Headers:** [scene.h](../scene.h), [noise2D.h](../noise2D.h)

`void registerVisualizerScene()`
  Registers the `"visualizer"` scene with the engine — the 3D voxel world. See [architecture.md](architecture.md) for the streaming pipeline.

`void registerNoise2DScene()`
  Registers the `"noise2D"` scene — a 512×512 biome heatmap textured onto a single quad with a bird's-eye camera. Built once in `onEnter`, static thereafter.
