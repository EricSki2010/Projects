#pragma once

namespace PerlinNoise {

void setSeed(int seed);
void setScale(float scale);
int  getSeed();

float sample2D(float x, float y);
float sample2D(float x, float y, int seed);
float sample3D(float x, float y, float z);
float sample3D(float x, float y, float z, int seed);
float fbm2D(float x, float y, int octaves = 4, float lacunarity = 2.0f, float gain = 0.5f);

}
