#include "biome.h"
#include "perlinNoiseManagement.h"

static const int   TEMP_SEED_OFFSET = 1000;
static const int   VEG_SEED_OFFSET  = 2000;
static const float HEIGHT_FREQ_MULT = 0.5f;
static const float TEMP_FREQ_MULT   = 0.3f;
static const float VEG_FREQ_MULT    = 0.2f;

Biome getBiome(float x, float z) {
    float height     = PerlinNoise::sample2D(x * HEIGHT_FREQ_MULT, z * HEIGHT_FREQ_MULT);
    float temp       = PerlinNoise::sample2D(x * TEMP_FREQ_MULT,   z * TEMP_FREQ_MULT,
                                              PerlinNoise::getSeed() + TEMP_SEED_OFFSET);
    float vegetation = PerlinNoise::sample2D(x * VEG_FREQ_MULT,    z * VEG_FREQ_MULT,
                                              PerlinNoise::getSeed() + VEG_SEED_OFFSET);

    if (height < -0.2 && temp < -0.2){
        return Biome::Sand; // low elevation, very sparse vegetation
    } else if (height < 0.1 && temp < -0.3){
        return Biome::Sand; // low elevation, cold, sparse vegetation
    } else if (height >= 0.1 && temp < -0.1){
        return Biome::SnowRock; // mid elevation, moderate temperature, dense vegetation
    } else if (height < 0.1 && temp < 0.2){
        return Biome::Grass; // mid elevation, moderate temperature, open vegetation
    } else if (height < 0.2){
        return Biome::Forest; // higher elevation, warmer, bare rock
    } else {
        return Biome::Rock; // high elevation, cold, bare rock
    }
}

const char* biomeName(Biome b) {
    switch (b) {
        case Biome::Sand:     return "Sand";
        case Biome::Shrub:    return "Shrub";
        case Biome::Forest:   return "Forest";
        case Biome::Grass:    return "Grass";
        case Biome::Rock:     return "Rock";
        case Biome::SnowRock: return "SnowRock";
    }
    return "Unknown";
}

float biomeTopRampDepth(Biome b) {
    // Blocks the surface can reach above Y_TOP_RAMP_START in this biome.
    switch (b) {
        case Biome::Sand:     return  4.0f; // flat beach/desert
        case Biome::Grass:    return 10.0f; // gentle plains
        case Biome::Shrub:    return 15.0f; // rolling shrubland
        case Biome::Forest:   return 25.0f; // forested hills
        case Biome::SnowRock: return 40.0f; // snow-capped mountain
        case Biome::Rock:     return 50.0f; // tall bare mountain
    }
    return 10.0f;
}
