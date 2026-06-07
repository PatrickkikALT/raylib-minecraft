#include "game.h"
#include "assets.h"
#include "entities.h"
#include "game_types.h"
#include "terrain.h"

#include "raylib.h"
#include "raymath.h"
#include "resource_dir.h"
#include "rlgl.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
std::unordered_map<ChunkCoord, Chunk, ChunkKeyHash, ChunkKeyEq> chunks;
std::unordered_map<BlockPos, uint8_t, BlockPosHash, BlockPosEq> edits;
std::deque<std::future<ChunkBuildResult>> pendingChunkBuilds;
std::unordered_set<ChunkCoord, ChunkKeyHash, ChunkKeyEq> queuedChunkBuilds;
Material voxelMaterial{};
Texture2D voxelAtlas{};
Texture2D pigTexture{};
Model pigModel{};

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
    if (chunk != chunks.end() && y >= 0 && y < WorldHeight && !chunk->second.blocks.empty())
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

bool lightOccluder(uint8_t block)
{
    return block != Air && block != Water;
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
    case Stone: return 4;
    case Sand: return 3;
    case Snow: return 5;
    case Water: return 0;
    case Wood: return 1;
    case Leaves: return 2;
    default: return 0;
    }
}

Vector2 atlasUv(int tile, Vector2 localUv)
{
    const float stride = static_cast<float>(AtlasTileSize + AtlasTilePadding * 2);
    const float atlasWidth = stride * static_cast<float>(AtlasTileCount);
    const float atlasHeight = stride;
    const float u0 = (static_cast<float>(tile) * stride + static_cast<float>(AtlasTilePadding) + 0.5f) / atlasWidth;
    const float v0 = (static_cast<float>(AtlasTilePadding) + 0.5f) / atlasHeight;
    const float u1 = (static_cast<float>(tile) * stride + static_cast<float>(AtlasTilePadding + AtlasTileSize) - 0.5f) / atlasWidth;
    const float v1 = (static_cast<float>(AtlasTilePadding + AtlasTileSize) - 0.5f) / atlasHeight;
    return {
        Lerp(u0, u1, localUv.x),
        Lerp(v0, v1, localUv.y)
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

Color litColor(Color base, float light)
{
    light = Clamp(light, 0.0f, 1.35f);
    return Color{
        static_cast<unsigned char>(std::min(255.0f, static_cast<float>(base.r) * light)),
        static_cast<unsigned char>(std::min(255.0f, static_cast<float>(base.g) * light)),
        static_cast<unsigned char>(std::min(255.0f, static_cast<float>(base.b) * light)),
        base.a
    };
}

float vertexAo(int wx, int y, int wz, Vector3 normal, Vector3 sideA, Vector3 sideB)
{
    const int nx = static_cast<int>(normal.x);
    const int ny = static_cast<int>(normal.y);
    const int nz = static_cast<int>(normal.z);
    const int ax = static_cast<int>(sideA.x);
    const int ay = static_cast<int>(sideA.y);
    const int az = static_cast<int>(sideA.z);
    const int bx = static_cast<int>(sideB.x);
    const int by = static_cast<int>(sideB.y);
    const int bz = static_cast<int>(sideB.z);

    const bool a = lightOccluder(blockAt(wx + nx + ax, y + ny + ay, wz + nz + az));
    const bool b = lightOccluder(blockAt(wx + nx + bx, y + ny + by, wz + nz + bz));
    const bool corner = lightOccluder(blockAt(wx + nx + ax + bx, y + ny + ay + by, wz + nz + az + bz));
    if (a && b) return 0.48f;

    const int open = 3 - static_cast<int>(a) - static_cast<int>(b) - static_cast<int>(corner);
    static constexpr float levels[4] = {0.58f, 0.72f, 0.88f, 1.0f};
    return levels[open];
}

bool hasSkyAccess(int wx, int y, int wz)
{
    return y >= terrainHeight(wx, wz);
}

float faceLight(int wx, int y, int wz, int face, Vector3 normal)
{
    const Vector3 sun = Vector3Normalize(SunDirection);
    const float sunTerm = std::max(0.0f, Vector3DotProduct(normal, sun));
    const bool sky = hasSkyAccess(wx, y, wz) || face == 2;
    const float skyLight = sky ? 1.0f : 0.68f;
    const float faceShade[6] = {0.82f, 0.92f, 1.08f, 0.66f, 0.84f, 0.94f};
    return (0.32f + skyLight * (0.52f + sunTerm * 0.18f)) * faceShade[face];
}

std::array<Vector3, 4> aoSidesForFace(int face)
{
    switch (face)
    {
    case 0: return {Vector3{0, 0, 1}, Vector3{0, 1, 0}, Vector3{0, 0, -1}, Vector3{0, -1, 0}};
    case 1: return {Vector3{0, 0, -1}, Vector3{0, 1, 0}, Vector3{0, 0, 1}, Vector3{0, -1, 0}};
    case 2: return {Vector3{-1, 0, 0}, Vector3{0, 0, 1}, Vector3{1, 0, 0}, Vector3{0, 0, -1}};
    case 3: return {Vector3{-1, 0, 0}, Vector3{0, 0, -1}, Vector3{1, 0, 0}, Vector3{0, 0, 1}};
    case 4: return {Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{-1, 0, 0}, Vector3{0, -1, 0}};
    default: return {Vector3{-1, 0, 0}, Vector3{0, 1, 0}, Vector3{1, 0, 0}, Vector3{0, -1, 0}};
    }
}

void appendFace(MeshBuilder& b, int wx, int wy, int wz, float x, float y, float z, int face, uint8_t block)
{
    static constexpr Vector3 normals[6] = {
        {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 1}
    };
    Color c = blockColor(block, face);

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
    const float light = faceLight(wx, wy, wz, face, normals[face]);
    const Color lc = litColor(c, light);

    appendVertex(b, quad[0], uv[0], normals[face], lc);
    appendVertex(b, quad[1], uv[1], normals[face], lc);
    appendVertex(b, quad[2], uv[2], normals[face], lc);
    appendVertex(b, quad[0], uv[0], normals[face], lc);
    appendVertex(b, quad[2], uv[2], normals[face], lc);
    appendVertex(b, quad[3], uv[3], normals[face], lc);
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

uint8_t editedGeneratedBlockAt(const std::unordered_map<BlockPos, uint8_t, BlockPosHash, BlockPosEq>& editSnapshot, int x, int y, int z)
{
    const BlockPos pos{x, y, z};
    auto edit = editSnapshot.find(pos);
    if (edit != editSnapshot.end()) return edit->second;
    return generatedBlockAt(x, y, z);
}

std::vector<uint8_t> generateChunkBlockData(ChunkCoord coord, const std::unordered_map<BlockPos, uint8_t, BlockPosHash, BlockPosEq>& editSnapshot)
{
    std::vector<uint8_t> blocks(ChunkSize * WorldHeight * ChunkSize, Air);
    for (int z = 0; z < ChunkSize; ++z)
    {
        for (int x = 0; x < ChunkSize; ++x)
        {
            const int wx = coord.x * ChunkSize + x;
            const int wz = coord.z * ChunkSize + z;
            const int h = terrainHeight(wx, wz);
            const int slope = terrainSlope(wx, wz);
            for (int y = 0; y < WorldHeight; ++y)
            {
                blocks[blockIndex(x, y, z)] = columnBlockAt(wx, y, wz, h, slope);
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
            if (base <= SeaLevel + 2 || base > SnowLine - 3) continue;

            for (int y = base; y < base + 5; ++y)
            {
                if (y >= 0 && y < WorldHeight && chunkOf(tx, tz).x == coord.x && chunkOf(tx, tz).z == coord.z)
                {
                    blocks[blockIndex(floorMod(tx, ChunkSize), y, floorMod(tz, ChunkSize))] = Wood;
                }
            }

            for (int dz = -2; dz <= 2; ++dz)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    for (int crown = -1; crown <= 2; ++crown)
                    {
                        if (std::abs(dx) + std::abs(dz) + std::max(0, crown) > 4) continue;
                        const int wx = tx - dx;
                        const int wy = base + 4 + crown;
                        const int wz = tz - dz;
                        if (wy < 0 || wy >= WorldHeight || chunkOf(wx, wz).x != coord.x || chunkOf(wx, wz).z != coord.z) continue;
                        uint8_t& target = blocks[blockIndex(floorMod(wx, ChunkSize), wy, floorMod(wz, ChunkSize))];
                        if (target == Air || target == Water) target = Leaves;
                    }
                }
            }
        }
    }

    for (const auto& edit : editSnapshot)
    {
        const BlockPos& p = edit.first;
        if (chunkOf(p.x, p.z).x == coord.x && chunkOf(p.x, p.z).z == coord.z && p.y >= 0 && p.y < WorldHeight)
        {
            blocks[blockIndex(floorMod(p.x, ChunkSize), p.y, floorMod(p.z, ChunkSize))] = edit.second;
        }
    }

    return blocks;
}

void generateChunkBlocks(ChunkCoord coord, Chunk& chunk)
{
    chunk.blocks = generateChunkBlockData(coord, edits);
}

uint8_t meshNeighborBlockAt(ChunkCoord coord, const std::vector<uint8_t>& blocks, const std::unordered_map<BlockPos, uint8_t, BlockPosHash, BlockPosEq>& editSnapshot, int wx, int y, int wz)
{
    if (y < 0 || y >= WorldHeight) return Air;

    const ChunkCoord neighborCoord = chunkOf(wx, wz);
    if (neighborCoord.x == coord.x && neighborCoord.z == coord.z)
    {
        return blocks[blockIndex(floorMod(wx, ChunkSize), y, floorMod(wz, ChunkSize))];
    }

    return editedGeneratedBlockAt(editSnapshot, wx, y, wz);
}

MeshBuilder buildChunkMeshData(ChunkCoord coord, const std::vector<uint8_t>& blocks, const std::unordered_map<BlockPos, uint8_t, BlockPosHash, BlockPosEq>& editSnapshot)
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
                const uint8_t block = blocks[blockIndex(x, y, z)];
                if (block == Air) continue;
                const int wx = coord.x * ChunkSize + x;
                const int wz = coord.z * ChunkSize + z;
                for (int face = 0; face < 6; ++face)
                {
                    const uint8_t neighbor = meshNeighborBlockAt(coord, blocks, editSnapshot, wx + dx[face], y + dy[face], wz + dz[face]);
                    if (transparent(neighbor) && neighbor != block)
                    {
                        appendFace(builder, wx, y, wz, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), face, block);
                    }
                }
            }
        }
    }

    return builder;
}

ChunkBuildResult buildChunkResult(ChunkCoord coord, std::unordered_map<BlockPos, uint8_t, BlockPosHash, BlockPosEq> editSnapshot)
{
    ChunkBuildResult result;
    result.coord = coord;
    result.blocks = generateChunkBlockData(coord, editSnapshot);
    result.mesh = buildChunkMeshData(coord, result.blocks, editSnapshot);
    result.faceCount = static_cast<int>(result.mesh.vertices.size() / 18);
    return result;
}

void uploadChunkMesh(Chunk& chunk, MeshBuilder&& builder)
{
    unloadMesh(chunk);
    chunk.faceCount = static_cast<int>(builder.vertices.size() / 18);
    if (builder.vertices.empty())
    {
        chunk.dirty = false;
        chunk.buildQueued = false;
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
    chunk.buildQueued = false;
}

void rebuildChunkMesh(ChunkCoord coord, Chunk& chunk)
{
    MeshBuilder builder = buildChunkMeshData(coord, chunk.blocks, edits);
    uploadChunkMesh(chunk, std::move(builder));
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
    if (chunk != chunks.end() && !chunk->second.blocks.empty())
    {
        chunk->second.blocks[blockIndex(floorMod(p.x, ChunkSize), p.y, floorMod(p.z, ChunkSize))] = block;
    }
    markDirty(p.x, p.z);
    if (floorMod(p.x, ChunkSize) == 0) markDirty(p.x - 1, p.z);
    if (floorMod(p.x, ChunkSize) == ChunkSize - 1) markDirty(p.x + 1, p.z);
    if (floorMod(p.z, ChunkSize) == 0) markDirty(p.x, p.z - 1);
    if (floorMod(p.z, ChunkSize) == ChunkSize - 1) markDirty(p.x, p.z + 1);
}

void scheduleChunkBuild(ChunkCoord coord)
{
    if (static_cast<int>(pendingChunkBuilds.size()) >= MaxPendingChunkJobs) return;
    if (queuedChunkBuilds.find(coord) != queuedChunkBuilds.end()) return;

    auto it = chunks.find(coord);
    if (it != chunks.end())
    {
        it->second.buildQueued = true;
        it->second.dirty = false;
    }
    else
    {
        Chunk placeholder;
        placeholder.buildQueued = true;
        placeholder.dirty = false;
        chunks.emplace(coord, std::move(placeholder));
    }

    queuedChunkBuilds.insert(coord);
    auto editSnapshot = edits;
    pendingChunkBuilds.push_back(std::async(std::launch::async, [coord, editSnapshot = std::move(editSnapshot)]() mutable {
        return buildChunkResult(coord, std::move(editSnapshot));
    }));
}

void collectFinishedChunkBuilds()
{
    int uploads = 0;
    for (auto it = pendingChunkBuilds.begin(); it != pendingChunkBuilds.end() && uploads < MeshUploadsPerFrame;)
    {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++it;
            continue;
        }

        ChunkBuildResult result = it->get();
        it = pendingChunkBuilds.erase(it);
        queuedChunkBuilds.erase(result.coord);

        Chunk& chunk = chunks[result.coord];
        chunk.blocks = std::move(result.blocks);
        uploadChunkMesh(chunk, std::move(result.mesh));
        chunk.faceCount = result.faceCount;
        ++uploads;
    }
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
        if (static_cast<int>(pendingChunkBuilds.size()) >= MaxPendingChunkJobs) break;
        scheduleChunkBuild(item.second);
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
        if (it != chunks.end() && it->second.dirty && !it->second.buildQueued)
        {
            scheduleChunkBuild(item.second);
            if (++built >= ChunkJobsPerFrame) break;
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

bool pigGroundAt(float x, float z, float& y)
{
    const int wx = static_cast<int>(std::floor(x));
    const int wz = static_cast<int>(std::floor(z));
    const int terrainY = terrainHeight(wx, wz);
    const int top = std::min(WorldHeight - 3, terrainY + 4);
    const int bottom = std::max(1, terrainY - 10);
    for (int gy = top; gy >= bottom; --gy)
    {
        if (!solid(blockAt(wx, gy, wz))) continue;
        if (blockAt(wx, gy + 1, wz) != Air || blockAt(wx, gy + 2, wz) != Air) continue;
        y = static_cast<float>(gy) + 1.02f;
        return true;
    }
    return false;
}

} // namespace

void RunGame()
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "Raylib Minecraft");
    //SetTargetFPS(600);
    SetWorldSeed(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    DisableCursor();
    SearchAndSetResourceDir("resources");

    voxelAtlas = LoadVoxelAtlas();
    voxelMaterial = LoadMaterialDefault();
    voxelMaterial.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    SetMaterialTexture(&voxelMaterial, MATERIAL_MAP_DIFFUSE, voxelAtlas);
    pigModel = LoadModel("pig.obj");
    pigTexture = LoadTexture("pig.png");
    SetTextureFilter(pigTexture, TEXTURE_FILTER_POINT);
    for (int i = 0; i < pigModel.materialCount; ++i)
    {
        SetMaterialTexture(&pigModel.materials[i], MATERIAL_MAP_DIFFUSE, pigTexture);
        pigModel.materials[i].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    }

    Camera3D camera{};
    Vector3 player{8.0f, static_cast<float>(terrainHeight(8, 8) + 3), 8.0f};
    Vector3 velocity{};
    float yaw = -45.0f * DEG2RAD;
    float pitch = -15.0f * DEG2RAD;
    uint8_t selectedBlock = Dirt;

    for (int i = 0; i < 64; ++i)
    {
        ensureChunksAround(player, MaxPendingChunkJobs);
        while (!pendingChunkBuilds.empty())
        {
            collectFinishedChunkBuilds();
            if (!pendingChunkBuilds.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

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
        const Rectangle summonButton{static_cast<float>(GetScreenWidth() - 164), 16.0f, 148.0f, 34.0f};
        const bool summonHovered = !IsCursorHidden() && CheckCollisionPointRec(GetMousePosition(), summonButton);
        if (IsKeyPressed(KEY_P) || (summonHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)))
        {
            SummonPig(player, forward, pigGroundAt);
        }
        if (hit.found && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !summonHovered) setBlock(hit.block, Air);
        if (hit.found && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !boxHitsWorld(Vector3{hit.previous.x + 0.5f, static_cast<float>(hit.previous.y), hit.previous.z + 0.5f}))
        {
            setBlock(hit.previous, selectedBlock);
        }

        collectFinishedChunkBuilds();
        ensureChunksAround(player, ChunkJobsPerFrame);
        rebuildDirtyMeshes(player);
        SpawnPigsAround(player, pigGroundAt);
        UpdatePigs(dt, pigGroundAt);

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
        DrawPigs(pigModel, player);
        EndMode3D();

        DrawRectangle(12, 12, 390, 116, Fade(BLACK, 0.32f));
        DrawText(TextFormat("FPS %d | chunks %d | pigs %d | edits %d", GetFPS(), static_cast<int>(chunks.size()), PigCount(), static_cast<int>(edits.size())), 24, 24, 20, WHITE);
        DrawText("WASD move  Space jump  Ctrl fly  LMB/RMB mine/place  P pig", 24, 50, 16, RAYWHITE);
        DrawText(TextFormat("Block %d | position %.1f %.1f %.1f", selectedBlock, player.x, player.y, player.z), 24, 72, 16, RAYWHITE);
        DrawText(TextFormat("Seed %u", GetWorldSeed()), 24, 94, 16, RAYWHITE);
        DrawRectangleRec(summonButton, summonHovered ? Color{235, 183, 197, 255} : Color{202, 132, 154, 255});
        DrawRectangleLinesEx(summonButton, 2.0f, Color{92, 45, 58, 255});
        DrawText("Summon Pig", static_cast<int>(summonButton.x + 18.0f), static_cast<int>(summonButton.y + 8.0f), 18, WHITE);
        drawCrosshair();
        EndDrawing();
    }

    for (auto& entry : chunks) unloadMesh(entry.second);
    UnloadModel(pigModel);
    UnloadTexture(pigTexture);
    UnloadMaterial(voxelMaterial);
    UnloadTexture(voxelAtlas);
    CloseWindow();
}
