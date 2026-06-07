#pragma once

#include "raylib.h"

#include <cstdint>
#include <vector>

constexpr int ChunkSize = 16;
constexpr int WorldHeight = 128;
constexpr int SeaLevel = 46;
constexpr int SnowLine = 88;
constexpr int LoadRadius = 8;
constexpr int UnloadRadius = LoadRadius + 2;
constexpr int ChunkJobsPerFrame = 2;
constexpr int MaxPendingChunkJobs = 4;
constexpr int MeshUploadsPerFrame = 1;
constexpr int AtlasTileSize = 1024;
constexpr int AtlasTilePadding = 4;
constexpr int AtlasTileCount = 7;
constexpr float PlayerHeight = 1.75f;
constexpr float PlayerRadius = 0.28f;
constexpr Vector3 SunDirection = Vector3{-0.45f, 0.85f, -0.28f};

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
    bool buildQueued = false;
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

struct PigEntity
{
    Vector3 position{};
    Vector3 target{};
    Vector3 pathStep{};
    float yaw = 0.0f;
    float wanderTimer = 0.0f;
    float pathTimer = 0.0f;
    float speed = 0.0f;
    bool hasTarget = false;
    bool hasPathStep = false;
};

struct ChunkBuildResult
{
    ChunkCoord coord{};
    std::vector<uint8_t> blocks;
    MeshBuilder mesh;
    int faceCount = 0;
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
