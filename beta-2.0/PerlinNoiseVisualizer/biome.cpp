#include "biome.h"
#include "perlinNoiseManagement.h"

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

static const int   TEMP_SEED_OFFSET = 1000;
static const float HEIGHT_FREQ_MULT = 0.05f;
static const float TEMP_FREQ_MULT   = 0.12f;

Biome getBiome(float x, float z) {
    float height = PerlinNoise::sample2D(x * HEIGHT_FREQ_MULT, z * HEIGHT_FREQ_MULT);
    float temp   = PerlinNoise::sample2D(x * TEMP_FREQ_MULT,   z * TEMP_FREQ_MULT,
                                          PerlinNoise::getSeed() + TEMP_SEED_OFFSET);

    if (height < -0.2 && temp < -0.2) {
        return Biome::Sand;     // low elevation, cold → frozen coast
    } else if (height < 0.1 && temp < -0.3) {
        return Biome::Sand;     // low mid, very cold
    } else if (height >= 0.1 && temp < -0.1) {
        return Biome::SnowRock; // mid+ elevation, cold → snowy peaks
    } else if (height < 0.1 && temp < 0.2) {
        return Biome::Grass;    // mid elevation, temperate
    } else if (height < 0.2) {
        return Biome::Forest;   // mid-high, warmer
    } else {
        return Biome::Rock;     // high elevation, otherwise
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
    switch (b) {
        case Biome::Sand:     return  4.0f;
        case Biome::Grass:    return 10.0f;
        case Biome::Shrub:    return 15.0f;
        case Biome::Forest:   return 25.0f;
        case Biome::SnowRock: return 40.0f;
        case Biome::Rock:     return 50.0f;
    }
    return 10.0f;
}

// ── Shared, thread-safe biome cache ──────────────────────────────────────
// Keyed on packed (x, z) world coords. Reads use a shared lock so many workers
// can query concurrently; only newly-discovered cells take the exclusive lock
// to insert.
static std::unordered_map<int64_t, Biome> sBiomeCache;
static std::shared_mutex sBiomeCacheMutex;

// Hard cap on cache size — when exceeded we clear wholesale. Misses are cheap
// (3 perlin samples + threshold cascade) and the working set rebuilds quickly,
// so this bounds RAM without needing per-entry LRU bookkeeping.
static const size_t BIOME_CACHE_MAX_ENTRIES = 500000; // ~30 MB at ~64 B/entry

Biome getCachedBiome(int x, int z) {
    int64_t key = ((int64_t)x << 32) | (uint32_t)z;
    {
        std::shared_lock<std::shared_mutex> rlk(sBiomeCacheMutex);
        auto it = sBiomeCache.find(key);
        if (it != sBiomeCache.end()) return it->second;
    }
    Biome b = getBiome((float)x, (float)z);
    {
        std::unique_lock<std::shared_mutex> wlk(sBiomeCacheMutex);
        if (sBiomeCache.size() >= BIOME_CACHE_MAX_ENTRIES) {
            sBiomeCache.clear();
            sBiomeCache.reserve(BIOME_CACHE_MAX_ENTRIES);
        }
        sBiomeCache.emplace(key, b); // emplace skips overwrite if another worker raced us
    }
    return b;
}

void clearBiomeCache() {
    std::unique_lock<std::shared_mutex> wlk(sBiomeCacheMutex);
    sBiomeCache.clear();
}

void reserveBiomeCache(size_t expectedEntries) {
    std::unique_lock<std::shared_mutex> wlk(sBiomeCacheMutex);
    sBiomeCache.reserve(expectedEntries);
}

size_t biomeCacheSize() {
    std::shared_lock<std::shared_mutex> rlk(sBiomeCacheMutex);
    return sBiomeCache.size();
}
