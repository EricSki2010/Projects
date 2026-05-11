#pragma once

// Biome classification for the Perlin world. The cascade combines three
// independent noise fields (height, temp, vegetation); thresholds and the
// per-field frequency / seed-offset constants live in biome.cpp. Anything that
// needs to know "what's at world (x, z)?" — renderers, prop placement,
// gameplay rules — should call getBiome.

enum class Biome {
    Sand,      // very low elevation
    Shrub,     // cold mid/low
    Forest,    // dense vegetation
    Grass,     // open mid elevation
    Rock,      // bare/warm mountain
    SnowRock,  // cold high mountain
};

// Returns the biome at world coordinates (x, z) using the noise fields and
// thresholds defined in biome.cpp. Pure function — same input always returns
// the same output for a given PerlinNoise seed/scale.
Biome getBiome(float x, float z);

// Human-readable name for a biome, e.g. "Forest". Returned pointer has static
// lifetime and is safe to compare with == against literals.
const char* biomeName(Biome b);

// Maximum elevation above the base ground level the surface can reach in this
// biome. Higher = taller peaks. The 3D scene IDW-blends this between
// neighboring biomes so amplitude transitions are smooth across boundaries.
float biomeTopRampDepth(Biome b);
