#include "raylib.h"
#include "raymath.h"
#include "resource_dir.h"
#include "rlgl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr int ChunkSize = 16;
constexpr int WorldHeight = 128;
constexpr int SeaLevel = 46;
constexpr int LoadRadius = 8;
constexpr int UnloadRadius = LoadRadius + 2;
constexpr int ChunkGenerationsPerFrame = 1;
constexpr int MeshesPerFrame = 1;
constexpr int AtlasTileSize = 1024;
constexpr int AtlasTileCount = 4;
constexpr float PlayerHeight = 1.75f;
constexpr float PlayerRadius = 0.28f;

enum Block : uint8_t
{
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Sand,
    Snow,
    Water,
    Wood,
    Leaves
};

struct ChunkCoord
{
    int x = 0;
    int z = 0;
};

struct BlockPos
{
    int x = 0;
    int y = 0;
    int z = 0;
};

struct Chunk
{
    std::vector<uint8_t> blocks;
    Mesh mesh{};
    bool meshUploaded = false;
    bool dirty = true;
    int faceCount = 0;
};

struct MeshBuilder
{
    std::vector<float> vertices;
    std::vector<float> texcoords;
    std::vector<float> normals;
    std::vector<unsigned char> colors;
};

struct Hit
{
    bool found = false;
    BlockPos block{};
    BlockPos previous{};
};

struct ChunkKeyHash
{
    size_t operator()(const ChunkCoord& c) const
    {
        const uint64_t ux = static_cast<uint32_t>(c.x);
        const uint64_t uz = static_cast<uint32_t>(c.z);
        return static_cast<size_t>((ux << 32) ^ uz);
    }
};

struct ChunkKeyEq
{
    bool operator()(const ChunkCoord& a, const ChunkCoord& b) const
    {
        return a.x == b.x && a.z == b.z;
    }
};

struct BlockPosHash
{
    size_t operator()(const BlockPos& p) const
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](int v) {
            h ^= static_cast<uint32_t>(v);
            h *= 1099511628211ull;
        };
        mix(p.x);
        mix(p.y);
        mix(p.z);
        return static_cast<size_t>(h);
    }
};

struct BlockPosEq
{
    bool operator()(const BlockPos& a, const BlockPos& b) const
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

std::unordered_map<ChunkCoord, Chunk, ChunkKeyHash, ChunkKeyEq> chunks;
std::unordered_map<BlockPos, uint8_t, BlockPosHash, BlockPosEq> edits;
Material voxelMaterial{};
Texture2D voxelAtlas{};

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
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return static_cast<float>(h ^ (h >> 16u)) / 4294967295.0f;
}

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

float fbm(float x, float z)
{
    float amp = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    float scale = 96.0f;
    for (int i = 0; i < 5; ++i)
    {
        sum += valueNoise(x, z, scale) * amp;
        norm += amp;
        amp *= 0.5f;
        scale *= 0.48f;
    }
    return sum / norm;
}

int terrainHeight(int x, int z)
{
    const float broad = fbm(static_cast<float>(x), static_cast<float>(z));
    const float ridges = std::abs(fbm(static_cast<float>(x) + 260.0f, static_cast<float>(z) - 120.0f) * 2.0f - 1.0f);
    const int h = static_cast<int>(34.0f + broad * 45.0f + ridges * ridges * 18.0f);
    return std::clamp(h, 8, WorldHeight - 8);
}

bool treeAt(int x, int z)
{
    if ((x & 3) != 0 || (z & 3) != 0) return false;
    return hash2(floorDiv(x, 4), floorDiv(z, 4)) > 0.965f;
}

uint8_t generatedBlockAt(int x, int y, int z)
{
    if (y < 0 || y >= WorldHeight) return Air;

    const int h = terrainHeight(x, z);
    if (y <= h)
    {
        if (y == h)
        {
            if (h <= SeaLevel + 1) return Sand;
            if (h > 82) return Snow;
            return Grass;
        }
        if (y > h - 4) return h <= SeaLevel + 1 ? Sand : Dirt;
        return Stone;
    }

    if (y <= SeaLevel) return Water;

    for (int dz = -2; dz <= 2; ++dz)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int tx = x + dx;
            const int tz = z + dz;
            if (!treeAt(tx, tz)) continue;
            const int base = terrainHeight(tx, tz) + 1;
            if (base <= SeaLevel + 2 || base > 84) continue;
            if (x == tx && z == tz && y >= base && y < base + 5) return Wood;
            const int crown = y - (base + 4);
            if (crown >= -1 && crown <= 2 && std::abs(dx) + std::abs(dz) + std::max(0, crown) <= 4) return Leaves;
        }
    }

    return Air;
}

ChunkCoord chunkOf(int worldX, int worldZ)
{
    return {floorDiv(worldX, ChunkSize), floorDiv(worldZ, ChunkSize)};
}

int blockIndex(int x, int y, int z)
{
    return y * ChunkSize * ChunkSize + z * ChunkSize + x;
}

uint8_t blockAt(int x, int y, int z)
{
    const BlockPos pos{x, y, z};
    auto edit = edits.find(pos);
    if (edit != edits.end()) return edit->second;

    const ChunkCoord cc = chunkOf(x, z);
    auto chunk = chunks.find(cc);
    if (chunk != chunks.end() && y >= 0 && y < WorldHeight)
    {
        return chunk->second.blocks[blockIndex(floorMod(x, ChunkSize), y, floorMod(z, ChunkSize))];
    }

    return generatedBlockAt(x, y, z);
}

bool solid(uint8_t block)
{
    return block != Air && block != Water && block != Leaves;
}

bool transparent(uint8_t block)
{
    return block == Air || block == Water || block == Leaves;
}

Color blockColor(uint8_t block, int face)
{
    switch (block)
    {
    case Grass: return WHITE;
    case Dirt: return WHITE;
    case Stone: return WHITE;
    case Sand: return Color{226, 209, 139, 255};
    case Snow: return Color{226, 236, 240, 255};
    case Water: return Color{62, 116, 200, 150};
    case Wood: return Color{128, 88, 50, 255};
    case Leaves: return Color{67, 150, 62, 210};
    default: return WHITE;
    }
}

int atlasTileFor(uint8_t block, int face)
{
    switch (block)
    {
    case Grass:
        if (face == 2) return 2; // grass.png
        if (face == 3) return 1; // dirt.png
        return 0;                // grassblock.png
    case Dirt: return 1;
    case Stone: return 3;        // bedrock.png doubles as the rocky texture in this small set
    case Sand: return 1;
    case Snow: return 2;
    case Water: return 0;
    case Wood: return 1;
    case Leaves: return 2;
    default: return 0;
    }
}

Vector2 atlasUv(int tile, Vector2 localUv)
{
    const float tileWidth = 1.0f / static_cast<float>(AtlasTileCount);
    const float inset = 0.5f / static_cast<float>(AtlasTileSize * AtlasTileCount);
    return {
        tile * tileWidth + localUv.x * tileWidth + (localUv.x == 0.0f ? inset : -inset),
        localUv.y + (localUv.y == 0.0f ? inset : -inset)
    };
}

std::array<Vector2, 4> faceUvs(int tile, int face)
{
    const Vector2 bl = atlasUv(tile, {0.0f, 1.0f});
    const Vector2 tl = atlasUv(tile, {0.0f, 0.0f});
    const Vector2 tr = atlasUv(tile, {1.0f, 0.0f});
    const Vector2 br = atlasUv(tile, {1.0f, 1.0f});

    switch (face)
    {
    case 0: return {br, tr, tl, bl}; // -X, texture up is +Y
    case 1: return {bl, tl, tr, br}; // +X, texture up is +Y
    case 4: return {br, bl, tl, tr}; // -Z, texture up is +Y
    case 5: return {bl, br, tr, tl}; // +Z, texture up is +Y
    case 2: return {bl, tl, tr, br}; // top
    case 3: return {tl, bl, br, tr}; // bottom
    default: return {bl, tl, tr, br};
    }
}

void appendVertex(MeshBuilder& b, Vector3 p, Vector2 uv, Vector3 n, Color c)
{
    b.vertices.push_back(p.x);
    b.vertices.push_back(p.y);
    b.vertices.push_back(p.z);
    b.texcoords.push_back(uv.x);
    b.texcoords.push_back(uv.y);
    b.normals.push_back(n.x);
    b.normals.push_back(n.y);
    b.normals.push_back(n.z);
    b.colors.push_back(c.r);
    b.colors.push_back(c.g);
    b.colors.push_back(c.b);
    b.colors.push_back(c.a);
}

void appendFace(MeshBuilder& b, float x, float y, float z, int face, uint8_t block)
{
    static constexpr Vector3 normals[6] = {
        {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 1}
    };
    const float shade[6] = {0.70f, 0.88f, 1.00f, 0.52f, 0.72f, 0.82f};
    Color c = blockColor(block, face);
    c.r = static_cast<unsigned char>(static_cast<float>(c.r) * shade[face]);
    c.g = static_cast<unsigned char>(static_cast<float>(c.g) * shade[face]);
    c.b = static_cast<unsigned char>(static_cast<float>(c.b) * shade[face]);

    const std::array<Vector3, 4> quad = [&]() {
        switch (face)
        {
        case 0: return std::array<Vector3, 4>{{{x, y, z + 1}, {x, y + 1, z + 1}, {x, y + 1, z}, {x, y, z}}};
        case 1: return std::array<Vector3, 4>{{{x + 1, y, z}, {x + 1, y + 1, z}, {x + 1, y + 1, z + 1}, {x + 1, y, z + 1}}};
        case 2: return std::array<Vector3, 4>{{{x, y + 1, z}, {x, y + 1, z + 1}, {x + 1, y + 1, z + 1}, {x + 1, y + 1, z}}};
        case 3: return std::array<Vector3, 4>{{{x, y, z + 1}, {x, y, z}, {x + 1, y, z}, {x + 1, y, z + 1}}};
        case 4: return std::array<Vector3, 4>{{{x + 1, y, z}, {x, y, z}, {x, y + 1, z}, {x + 1, y + 1, z}}};
        default: return std::array<Vector3, 4>{{{x, y, z + 1}, {x + 1, y, z + 1}, {x + 1, y + 1, z + 1}, {x, y + 1, z + 1}}};
        }
    }();

    const int tile = atlasTileFor(block, face);
    const std::array<Vector2, 4> uv = faceUvs(tile, face);

    appendVertex(b, quad[0], uv[0], normals[face], c);
    appendVertex(b, quad[1], uv[1], normals[face], c);
    appendVertex(b, quad[2], uv[2], normals[face], c);
    appendVertex(b, quad[0], uv[0], normals[face], c);
    appendVertex(b, quad[2], uv[2], normals[face], c);
    appendVertex(b, quad[3], uv[3], normals[face], c);
}

void unloadMesh(Chunk& chunk)
{
    if (chunk.meshUploaded)
    {
        UnloadMesh(chunk.mesh);
        chunk.mesh = {};
        chunk.meshUploaded = false;
    }
}

void setGeneratedLocalBlock(Chunk& chunk, ChunkCoord coord, int worldX, int y, int worldZ, uint8_t block)
{
    if (y < 0 || y >= WorldHeight) return;
    if (chunkOf(worldX, worldZ).x != coord.x || chunkOf(worldX, worldZ).z != coord.z) return;

    const int lx = floorMod(worldX, ChunkSize);
    const int lz = floorMod(worldZ, ChunkSize);
    uint8_t& target = chunk.blocks[blockIndex(lx, y, lz)];
    if (target == Air || target == Water || block == Wood) target = block;
}

void generateChunkBlocks(ChunkCoord coord, Chunk& chunk)
{
    chunk.blocks.assign(ChunkSize * WorldHeight * ChunkSize, Air);
    for (int z = 0; z < ChunkSize; ++z)
    {
        for (int x = 0; x < ChunkSize; ++x)
        {
            const int wx = coord.x * ChunkSize + x;
            const int wz = coord.z * ChunkSize + z;
            const int h = terrainHeight(wx, wz);
            for (int y = 0; y < WorldHeight; ++y)
            {
                uint8_t block = Air;
                if (y <= h)
                {
                    if (y == h)
                    {
                        if (h <= SeaLevel + 1) block = Sand;
                        else if (h > 82) block = Snow;
                        else block = Grass;
                    }
                    else if (y > h - 4)
                    {
                        block = h <= SeaLevel + 1 ? Sand : Dirt;
                    }
                    else
                    {
                        block = Stone;
                    }
                }
                else if (y <= SeaLevel)
                {
                    block = Water;
                }
                chunk.blocks[blockIndex(x, y, z)] = block;
            }
        }
    }

    const int minX = coord.x * ChunkSize - 2;
    const int maxX = coord.x * ChunkSize + ChunkSize + 1;
    const int minZ = coord.z * ChunkSize - 2;
    const int maxZ = coord.z * ChunkSize + ChunkSize + 1;
    for (int tz = minZ; tz <= maxZ; ++tz)
    {
        for (int tx = minX; tx <= maxX; ++tx)
        {
            if (!treeAt(tx, tz)) continue;
            const int base = terrainHeight(tx, tz) + 1;
            if (base <= SeaLevel + 2 || base > 84) continue;

            for (int y = base; y < base + 5; ++y)
            {
                setGeneratedLocalBlock(chunk, coord, tx, y, tz, Wood);
            }

            for (int dz = -2; dz <= 2; ++dz)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    for (int crown = -1; crown <= 2; ++crown)
                    {
                        if (std::abs(dx) + std::abs(dz) + std::max(0, crown) > 4) continue;
                        setGeneratedLocalBlock(chunk, coord, tx - dx, base + 4 + crown, tz - dz, Leaves);
                    }
                }
            }
        }
    }

    for (const auto& edit : edits)
    {
        const BlockPos& p = edit.first;
        if (chunkOf(p.x, p.z).x == coord.x && chunkOf(p.x, p.z).z == coord.z && p.y >= 0 && p.y < WorldHeight)
        {
            chunk.blocks[blockIndex(floorMod(p.x, ChunkSize), p.y, floorMod(p.z, ChunkSize))] = edit.second;
        }
    }
}

void rebuildChunkMesh(ChunkCoord coord, Chunk& chunk)
{
    MeshBuilder builder;
    builder.vertices.reserve(ChunkSize * ChunkSize * 6 * 6);
    builder.texcoords.reserve(ChunkSize * ChunkSize * 6 * 4);
    builder.normals.reserve(builder.vertices.capacity());
    builder.colors.reserve(ChunkSize * ChunkSize * 6 * 4);

    static constexpr int dx[6] = {-1, 1, 0, 0, 0, 0};
    static constexpr int dy[6] = {0, 0, 1, -1, 0, 0};
    static constexpr int dz[6] = {0, 0, 0, 0, -1, 1};

    for (int y = 0; y < WorldHeight; ++y)
    {
        for (int z = 0; z < ChunkSize; ++z)
        {
            for (int x = 0; x < ChunkSize; ++x)
            {
                const uint8_t block = chunk.blocks[blockIndex(x, y, z)];
                if (block == Air) continue;
                const int wx = coord.x * ChunkSize + x;
                const int wz = coord.z * ChunkSize + z;
                for (int face = 0; face < 6; ++face)
                {
                    const uint8_t neighbor = blockAt(wx + dx[face], y + dy[face], wz + dz[face]);
                    if (transparent(neighbor) && neighbor != block)
                    {
                        appendFace(builder, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), face, block);
                    }
                }
            }
        }
    }

    unloadMesh(chunk);
    chunk.faceCount = static_cast<int>(builder.vertices.size() / 18);
    if (builder.vertices.empty())
    {
        chunk.dirty = false;
        return;
    }

    chunk.mesh.vertexCount = static_cast<int>(builder.vertices.size() / 3);
    chunk.mesh.triangleCount = chunk.mesh.vertexCount / 3;
    chunk.mesh.vertices = static_cast<float*>(MemAlloc(builder.vertices.size() * sizeof(float)));
    chunk.mesh.texcoords = static_cast<float*>(MemAlloc(builder.texcoords.size() * sizeof(float)));
    chunk.mesh.normals = static_cast<float*>(MemAlloc(builder.normals.size() * sizeof(float)));
    chunk.mesh.colors = static_cast<unsigned char*>(MemAlloc(builder.colors.size() * sizeof(unsigned char)));
    std::copy(builder.vertices.begin(), builder.vertices.end(), chunk.mesh.vertices);
    std::copy(builder.texcoords.begin(), builder.texcoords.end(), chunk.mesh.texcoords);
    std::copy(builder.normals.begin(), builder.normals.end(), chunk.mesh.normals);
    std::copy(builder.colors.begin(), builder.colors.end(), chunk.mesh.colors);
    UploadMesh(&chunk.mesh, false);
    chunk.meshUploaded = true;
    chunk.dirty = false;
}

void markDirty(int x, int z)
{
    const ChunkCoord cc = chunkOf(x, z);
    auto it = chunks.find(cc);
    if (it != chunks.end()) it->second.dirty = true;
}

void setBlock(BlockPos p, uint8_t block)
{
    if (p.y < 0 || p.y >= WorldHeight) return;
    edits[p] = block;
    const ChunkCoord cc = chunkOf(p.x, p.z);
    auto chunk = chunks.find(cc);
    if (chunk != chunks.end())
    {
        chunk->second.blocks[blockIndex(floorMod(p.x, ChunkSize), p.y, floorMod(p.z, ChunkSize))] = block;
    }
    markDirty(p.x, p.z);
    if (floorMod(p.x, ChunkSize) == 0) markDirty(p.x - 1, p.z);
    if (floorMod(p.x, ChunkSize) == ChunkSize - 1) markDirty(p.x + 1, p.z);
    if (floorMod(p.z, ChunkSize) == 0) markDirty(p.x, p.z - 1);
    if (floorMod(p.z, ChunkSize) == ChunkSize - 1) markDirty(p.x, p.z + 1);
}

void ensureChunksAround(Vector3 position, int maxNewChunks)
{
    const ChunkCoord center = chunkOf(static_cast<int>(std::floor(position.x)), static_cast<int>(std::floor(position.z)));
    std::vector<std::pair<int, ChunkCoord>> missing;
    missing.reserve((LoadRadius * 2 + 1) * (LoadRadius * 2 + 1));

    for (int dz = -LoadRadius; dz <= LoadRadius; ++dz)
    {
        for (int dx = -LoadRadius; dx <= LoadRadius; ++dx)
        {
            if (dx * dx + dz * dz > LoadRadius * LoadRadius) continue;
            const ChunkCoord cc{center.x + dx, center.z + dz};
            if (chunks.find(cc) == chunks.end())
            {
                missing.push_back({dx * dx + dz * dz, cc});
            }
        }
    }
    std::sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    int generated = 0;
    for (const auto& item : missing)
    {
        if (generated >= maxNewChunks) break;
        if (chunks.find(item.second) != chunks.end()) continue;

        Chunk chunk;
        generateChunkBlocks(item.second, chunk);
        chunks.emplace(item.second, std::move(chunk));
        ++generated;
    }

    for (auto it = chunks.begin(); it != chunks.end();)
    {
        const int dx = it->first.x - center.x;
        const int dz = it->first.z - center.z;
        if (dx * dx + dz * dz > UnloadRadius * UnloadRadius)
        {
            unloadMesh(it->second);
            it = chunks.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void rebuildDirtyMeshes(Vector3 position)
{
    std::vector<std::pair<int, ChunkCoord>> dirty;
    dirty.reserve(chunks.size());
    const ChunkCoord center = chunkOf(static_cast<int>(std::floor(position.x)), static_cast<int>(std::floor(position.z)));
    for (const auto& entry : chunks)
    {
        if (!entry.second.dirty) continue;
        const int dx = entry.first.x - center.x;
        const int dz = entry.first.z - center.z;
        dirty.push_back({dx * dx + dz * dz, entry.first});
    }
    std::sort(dirty.begin(), dirty.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    int built = 0;
    for (const auto& item : dirty)
    {
        auto it = chunks.find(item.second);
        if (it != chunks.end() && it->second.dirty)
        {
            rebuildChunkMesh(item.second, it->second);
            if (++built >= MeshesPerFrame) break;
        }
    }
}

bool boxHitsWorld(Vector3 pos)
{
    const int minX = static_cast<int>(std::floor(pos.x - PlayerRadius));
    const int maxX = static_cast<int>(std::floor(pos.x + PlayerRadius));
    const int minY = static_cast<int>(std::floor(pos.y));
    const int maxY = static_cast<int>(std::floor(pos.y + PlayerHeight));
    const int minZ = static_cast<int>(std::floor(pos.z - PlayerRadius));
    const int maxZ = static_cast<int>(std::floor(pos.z + PlayerRadius));

    for (int y = minY; y <= maxY; ++y)
    {
        for (int z = minZ; z <= maxZ; ++z)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                if (solid(blockAt(x, y, z))) return true;
            }
        }
    }
    return false;
}

void moveAxis(Vector3& pos, float delta, int axis)
{
    Vector3 next = pos;
    if (axis == 0) next.x += delta;
    if (axis == 1) next.y += delta;
    if (axis == 2) next.z += delta;
    if (!boxHitsWorld(next)) pos = next;
}

Hit raycastBlocks(Vector3 origin, Vector3 dir, float maxDistance)
{
    Hit hit;
    Vector3 p = origin;
    BlockPos previous{
        static_cast<int>(std::floor(p.x)),
        static_cast<int>(std::floor(p.y)),
        static_cast<int>(std::floor(p.z))
    };

    constexpr float Step = 0.05f;
    for (float d = 0.0f; d < maxDistance; d += Step)
    {
        p = Vector3Add(origin, Vector3Scale(dir, d));
        BlockPos current{
            static_cast<int>(std::floor(p.x)),
            static_cast<int>(std::floor(p.y)),
            static_cast<int>(std::floor(p.z))
        };
        const uint8_t block = blockAt(current.x, current.y, current.z);
        if (block != Air && block != Water)
        {
            hit.found = true;
            hit.block = current;
            hit.previous = previous;
            return hit;
        }
        previous = current;
    }
    return hit;
}

void drawCrosshair()
{
    const int cx = GetScreenWidth() / 2;
    const int cy = GetScreenHeight() / 2;
    DrawLine(cx - 8, cy, cx - 3, cy, Fade(WHITE, 0.75f));
    DrawLine(cx + 3, cy, cx + 8, cy, Fade(WHITE, 0.75f));
    DrawLine(cx, cy - 8, cx, cy - 3, Fade(WHITE, 0.75f));
    DrawLine(cx, cy + 3, cx, cy + 8, Fade(WHITE, 0.75f));
}

void drawAtlasTile(Image& atlas, const char* fileName, int tile)
{
    Image source = LoadImage(fileName);
    if (source.data == nullptr) return;

    const Rectangle src{
        0.0f,
        0.0f,
        static_cast<float>(source.width),
        static_cast<float>(source.height)
    };
    const Rectangle dst{
        static_cast<float>(tile * AtlasTileSize),
        0.0f,
        static_cast<float>(AtlasTileSize),
        static_cast<float>(AtlasTileSize)
    };
    ImageDraw(&atlas, source, src, dst, WHITE);
    UnloadImage(source);
}

Texture2D loadVoxelAtlas()
{
    Image atlas = GenImageColor(AtlasTileSize * AtlasTileCount, AtlasTileSize, MAGENTA);
    drawAtlasTile(atlas, "grassblock.png", 0);
    drawAtlasTile(atlas, "dirt.png", 1);
    drawAtlasTile(atlas, "grass.png", 2);
    drawAtlasTile(atlas, "bedrock.png", 3);

    Texture2D texture = LoadTextureFromImage(atlas);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
    UnloadImage(atlas);
    return texture;
}

} // namespace

int main()
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "Raycraft - chunked voxel terrain");
    DisableCursor();
    SearchAndSetResourceDir("resources");

    voxelAtlas = loadVoxelAtlas();
    voxelMaterial = LoadMaterialDefault();
    voxelMaterial.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    SetMaterialTexture(&voxelMaterial, MATERIAL_MAP_DIFFUSE, voxelAtlas);

    Camera3D camera{};
    Vector3 player{8.0f, static_cast<float>(terrainHeight(8, 8) + 3), 8.0f};
    Vector3 velocity{};
    float yaw = -45.0f * DEG2RAD;
    float pitch = -15.0f * DEG2RAD;
    uint8_t selectedBlock = Dirt;

    ensureChunksAround(player, 64);
    for (int i = 0; i < 24; ++i) rebuildDirtyMeshes(player);

    while (!WindowShouldClose())
    {
        const float dt = std::min(GetFrameTime(), 1.0f / 30.0f);
        if (IsKeyPressed(KEY_TAB))
        {
            if (IsCursorHidden()) EnableCursor();
            else DisableCursor();
        }

        if (IsCursorHidden())
        {
            const Vector2 mouse = GetMouseDelta();
            yaw -= mouse.x * 0.0022f;
            pitch -= mouse.y * 0.0022f;
            pitch = Clamp(pitch, -1.52f, 1.52f);
        }

        Vector3 forward{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
        forward = Vector3Normalize(forward);
        Vector3 flatForward = Vector3Normalize(Vector3{std::sin(yaw), 0.0f, std::cos(yaw)});
        Vector3 right = Vector3Normalize(Vector3{std::cos(yaw), 0.0f, -std::sin(yaw)});

        Vector3 wish{};
        if (IsKeyDown(KEY_W)) wish = Vector3Add(wish, flatForward);
        if (IsKeyDown(KEY_S)) wish = Vector3Subtract(wish, flatForward);
        if (IsKeyDown(KEY_D)) wish = Vector3Subtract(wish, right);
        if (IsKeyDown(KEY_A)) wish = Vector3Add(wish, right);
        if (Vector3LengthSqr(wish) > 0.0f) wish = Vector3Normalize(wish);

        const bool fly = IsKeyDown(KEY_LEFT_CONTROL);
        const float speed = IsKeyDown(KEY_LEFT_SHIFT) ? 14.0f : 7.0f;
        if (fly)
        {
            velocity = Vector3Scale(wish, speed);
            if (IsKeyDown(KEY_SPACE)) velocity.y += speed;
            if (IsKeyDown(KEY_C)) velocity.y -= speed;
        }
        else
        {
            velocity.x = wish.x * speed;
            velocity.z = wish.z * speed;
            velocity.y -= 28.0f * dt;
            if (IsKeyPressed(KEY_SPACE) && boxHitsWorld(Vector3{player.x, player.y - 0.08f, player.z}))
            {
                velocity.y = 9.5f;
            }
        }

        moveAxis(player, velocity.x * dt, 0);
        const float beforeY = player.y;
        moveAxis(player, velocity.y * dt, 1);
        if (player.y == beforeY) velocity.y = 0.0f;
        moveAxis(player, velocity.z * dt, 2);

        for (int i = 0; i < 7; ++i)
        {
            if (IsKeyPressed(KEY_ONE + i)) selectedBlock = static_cast<uint8_t>(Grass + i);
        }

        const Vector3 eye = Vector3Add(player, Vector3{0.0f, 1.62f, 0.0f});
        const Hit hit = raycastBlocks(eye, forward, 6.0f);
        if (hit.found && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) setBlock(hit.block, Air);
        if (hit.found && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !boxHitsWorld(Vector3{hit.previous.x + 0.5f, static_cast<float>(hit.previous.y), hit.previous.z + 0.5f}))
        {
            setBlock(hit.previous, selectedBlock);
        }

        ensureChunksAround(player, ChunkGenerationsPerFrame);
        rebuildDirtyMeshes(player);

        camera.position = eye;
        camera.target = Vector3Add(eye, forward);
        camera.up = Vector3{0.0f, 1.0f, 0.0f};
        camera.fovy = 70.0f;
        camera.projection = CAMERA_PERSPECTIVE;

        BeginDrawing();
        ClearBackground(Color{128, 188, 238, 255});
        BeginMode3D(camera);
        rlDisableBackfaceCulling();
        for (const auto& entry : chunks)
        {
            if (!entry.second.meshUploaded) continue;
            const float wx = static_cast<float>(entry.first.x * ChunkSize);
            const float wz = static_cast<float>(entry.first.z * ChunkSize);
            const Vector3 center{wx + ChunkSize * 0.5f, WorldHeight * 0.45f, wz + ChunkSize * 0.5f};
            if (Vector3DistanceSqr(center, player) > static_cast<float>((LoadRadius * ChunkSize + 32) * (LoadRadius * ChunkSize + 32))) continue;
            DrawMesh(entry.second.mesh, voxelMaterial, MatrixTranslate(wx, 0.0f, wz));
        }
        if (hit.found)
        {
            DrawCubeWires(Vector3{hit.block.x + 0.5f, hit.block.y + 0.5f, hit.block.z + 0.5f}, 1.01f, 1.01f, 1.01f, BLACK);
        }
        EndMode3D();

        DrawRectangle(12, 12, 340, 94, Fade(BLACK, 0.32f));
        DrawText(TextFormat("FPS %d | chunks %d | edits %d", GetFPS(), static_cast<int>(chunks.size()), static_cast<int>(edits.size())), 24, 24, 20, WHITE);
        DrawText("WASD move  Space jump  Ctrl fly  LMB/RMB mine/place", 24, 50, 16, RAYWHITE);
        DrawText(TextFormat("Block %d | position %.1f %.1f %.1f", selectedBlock, player.x, player.y, player.z), 24, 72, 16, RAYWHITE);
        drawCrosshair();
        EndDrawing();
    }

    for (auto& entry : chunks) unloadMesh(entry.second);
    UnloadMaterial(voxelMaterial);
    UnloadTexture(voxelAtlas);
    CloseWindow();
    return 0;
}
