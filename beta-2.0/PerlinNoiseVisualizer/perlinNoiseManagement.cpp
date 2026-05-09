#include "perlinNoiseManagement.h"

#define STB_PERLIN_IMPLEMENTATION
#include "stb_perlin.h"

namespace PerlinNoise {

static int   sSeed  = 0;
static float sScale = 0.1f;

void setSeed(int seed)     { sSeed = seed; }
void setScale(float scale) { sScale = scale; }
int  getSeed()             { return sSeed; }

float sample2D(float x, float y) {
    return stb_perlin_noise3_seed(x * sScale, y * sScale, 0.0f, 0, 0, 0, sSeed);
}

float sample3D(float x, float y, float z) {
    return stb_perlin_noise3_seed(x * sScale, y * sScale, z * sScale, 0, 0, 0, sSeed);
}

float sample3D(float x, float y, float z, int seed) {
    return stb_perlin_noise3_seed(x * sScale, y * sScale, z * sScale, 0, 0, 0, seed);
}

float fbm2D(float x, float y, int octaves, float lacunarity, float gain) {
    return stb_perlin_fbm_noise3(x * sScale, y * sScale, 0.0f, lacunarity, gain, octaves);
}

}
