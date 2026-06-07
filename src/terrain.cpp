#include "terrain.h"

#include "raymath.h"

#include <algorithm>
#include <cmath>

namespace
{
uint32_t worldSeed = 0;

float smooth(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

float valueNoise(float x, float z, float scale)
{
    x /= scale;
    z /= scale;
    const int x0 = static_cast<int>(std::floor(x));
    const int z0 = static_cast<int>(std::floor(z));
    const float tx = smooth(x - static_cast<float>(x0));
    const float tz = smooth(z - static_cast<float>(z0));
    const float a = hash2(x0, z0);
    const float b = hash2(x0 + 1, z0);
    const float c = hash2(x0, z0 + 1);
    const float d = hash2(x0 + 1, z0 + 1);
    return Lerp(Lerp(a, b, tx), Lerp(c, d, tx), tz);
}

float fbmScaled(float x, float z, float baseScale, int octaves)
{
    float amp = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    float scale = baseScale;
    for (int i = 0; i < octaves; ++i)
    {
        sum += valueNoise(x, z, scale) * amp;
        norm += amp;
        amp *= 0.5f;
        scale *= 0.5f;
    }
    return sum / norm;
}

float smoothStep(float edge0, float edge1, float x)
{
    const float t = Clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float ridgeNoise(float x, float z, float scale)
{
    const float n = valueNoise(x, z, scale) * 2.0f - 1.0f;
    const float ridge = 1.0f - std::abs(n);
    return ridge * ridge;
}
}

void SetWorldSeed(uint32_t seed)
{
    worldSeed = seed;
}

uint32_t GetWorldSeed()
{
    return worldSeed;
}

int floorDiv(int v, int d)
{
    int q = v / d;
    int r = v % d;
    return (r != 0 && ((r < 0) != (d < 0))) ? q - 1 : q;
}

int floorMod(int v, int d)
{
    int r = v % d;
    return r < 0 ? r + d : r;
}

float hash2(int x, int z)
{
    uint32_t h = worldSeed ^ (static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u);
    h = (h ^ (h >> 13u)) * 1274126177u;
    return static_cast<float>(h ^ (h >> 16u)) / 4294967295.0f;
}

int terrainHeight(int x, int z)
{
    const float fx = static_cast<float>(x);
    const float fz = static_cast<float>(z);
    const float warpX = (fbmScaled(fx + 900.0f, fz - 350.0f, 260.0f, 3) - 0.5f) * 44.0f;
    const float warpZ = (fbmScaled(fx - 520.0f, fz + 740.0f, 260.0f, 3) - 0.5f) * 44.0f;
    const float wx = fx + warpX;
    const float wz = fz + warpZ;

    const float continent = fbmScaled(wx, wz, 540.0f, 4);
    const float mountainMask = smoothStep(0.44f, 0.72f, continent);
    const float mountainMass = smoothStep(0.42f, 0.82f, fbmScaled(wx - 700.0f, wz + 250.0f, 310.0f, 4));
    const float foothills = fbmScaled(wx + 180.0f, wz - 440.0f, 165.0f, 4);
    const float ridgeA = ridgeNoise(wx + 40.0f, wz - 120.0f, 150.0f);
    const float ridgeB = ridgeNoise(wx - 310.0f, wz + 270.0f, 82.0f) * 0.55f;
    const float alpine = std::pow(std::max(ridgeA, ridgeB), 0.82f);
    const float erosion = fbmScaled(wx + 1200.0f, wz + 800.0f, 32.0f, 3) - 0.5f;

    const float lowlands = 31.0f + continent * 31.0f + foothills * 13.0f;
    const float mountainLift = mountainMask * (mountainMass * 46.0f + foothills * 22.0f);
    const float peaks = mountainMask * mountainMass * alpine * 34.0f;
    const int h = static_cast<int>(lowlands + mountainLift + peaks + erosion * (4.0f + mountainMask * 8.0f));
    return std::clamp(h, 8, WorldHeight - 5);
}

int terrainSlope(int x, int z)
{
    const int h = terrainHeight(x, z);
    const int dx = std::max(std::abs(h - terrainHeight(x - 1, z)), std::abs(h - terrainHeight(x + 1, z)));
    const int dz = std::max(std::abs(h - terrainHeight(x, z - 1)), std::abs(h - terrainHeight(x, z + 1)));
    return std::max(dx, dz);
}

uint8_t surfaceBlockFor(int x, int z, int h, int slope)
{
    if (h <= SeaLevel + 2) return Sand;

    const float snowNoise = valueNoise(static_cast<float>(x) + 37.0f, static_cast<float>(z) - 91.0f, 38.0f);
    const int localSnowLine = SnowLine + static_cast<int>((snowNoise - 0.5f) * 9.0f);
    if (h >= localSnowLine) return Snow;

    if (slope >= 5 || (h > 74 && slope >= 3) || h > SnowLine - 5) return Stone;
    return Grass;
}

uint8_t columnBlockAt(int x, int y, int z, int h, int slope)
{
    if (y > h)
    {
        return y <= SeaLevel ? Water : Air;
    }

    const uint8_t surface = surfaceBlockFor(x, z, h, slope);
    if (y == h) return surface;

    const int depth = h - y;
    if (surface == Sand && depth < 5) return Sand;
    if (surface == Snow && depth < 2) return Snow;
    if (surface == Grass && depth < 4) return Dirt;
    if (surface == Stone && depth < 2 && h < SnowLine - 2 && slope < 7) return Dirt;
    return Stone;
}

bool treeAt(int x, int z)
{
    if ((x & 3) != 0 || (z & 3) != 0) return false;
    const int h = terrainHeight(x, z);
    if (h <= SeaLevel + 3 || h >= SnowLine - 4 || terrainSlope(x, z) > 3) return false;
    return hash2(floorDiv(x, 4), floorDiv(z, 4)) > 0.968f;
}

uint8_t generatedBlockAt(int x, int y, int z)
{
    if (y < 0 || y >= WorldHeight) return Air;

    const int h = terrainHeight(x, z);
    const int slope = terrainSlope(x, z);
    const uint8_t terrain = columnBlockAt(x, y, z, h, slope);
    if (terrain != Air && terrain != Water) return terrain;
    if (terrain == Water) return Water;

    for (int dz = -2; dz <= 2; ++dz)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int tx = x + dx;
            const int tz = z + dz;
            if (!treeAt(tx, tz)) continue;
            const int base = terrainHeight(tx, tz) + 1;
            if (base <= SeaLevel + 2 || base > SnowLine - 3) continue;
            if (x == tx && z == tz && y >= base && y < base + 5) return Wood;
            const int crown = y - (base + 4);
            if (crown >= -1 && crown <= 2 && std::abs(dx) + std::abs(dz) + std::max(0, crown) <= 4) return Leaves;
        }
    }

    return Air;
}
