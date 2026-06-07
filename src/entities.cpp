#include "entities.h"

#include "game_types.h"
#include "terrain.h"

#include "raymath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace
{
std::vector<PigEntity> pigs;
std::unordered_set<ChunkCoord, ChunkKeyHash, ChunkKeyEq> spawnedPigCells;

bool pigCellWalkable(PigGroundProbe groundProbe, int x, int z, int& groundY)
{
    float y = 0.0f;
    if (!groundProbe(static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f, y)) return false;
    groundY = static_cast<int>(std::floor(y));
    return true;
}

bool pigCanMoveBetween(PigGroundProbe groundProbe, int fromX, int fromZ, int toX, int toZ)
{
    int fromY = 0;
    int toY = 0;
    if (!pigCellWalkable(groundProbe, fromX, fromZ, fromY) || !pigCellWalkable(groundProbe, toX, toZ, toY)) return false;
    return std::abs(toY - fromY) <= 1;
}

bool findPigPathStep(PigGroundProbe groundProbe, Vector3 from, Vector3 desired, Vector3& next)
{
    constexpr int Radius = 4;
    constexpr int Size = Radius * 2 + 1;
    constexpr int Count = Size * Size;
    const int startX = static_cast<int>(std::floor(from.x));
    const int startZ = static_cast<int>(std::floor(from.z));
    const int goalX = static_cast<int>(std::floor(desired.x));
    const int goalZ = static_cast<int>(std::floor(desired.z));
    auto indexOf = [](int lx, int lz) { return lz * Size + lx; };

    std::array<int, Count> parent{};
    std::array<int, Count> queue{};
    parent.fill(-2);
    int head = 0;
    int tail = 0;
    const int start = indexOf(Radius, Radius);
    parent[start] = -1;
    queue[tail++] = start;

    int best = start;
    int bestScore = std::abs(startX - goalX) + std::abs(startZ - goalZ);
    static constexpr int ox[4] = {1, -1, 0, 0};
    static constexpr int oz[4] = {0, 0, 1, -1};

    while (head < tail)
    {
        const int current = queue[head++];
        const int lx = current % Size;
        const int lz = current / Size;
        const int wx = startX + lx - Radius;
        const int wz = startZ + lz - Radius;
        const int score = std::abs(wx - goalX) + std::abs(wz - goalZ);
        if (score < bestScore)
        {
            bestScore = score;
            best = current;
            if (score == 0) break;
        }

        for (int i = 0; i < 4; ++i)
        {
            const int nlx = lx + ox[i];
            const int nlz = lz + oz[i];
            if (nlx < 0 || nlx >= Size || nlz < 0 || nlz >= Size) continue;
            const int ni = indexOf(nlx, nlz);
            if (parent[ni] != -2) continue;
            if (!pigCanMoveBetween(groundProbe, wx, wz, startX + nlx - Radius, startZ + nlz - Radius)) continue;
            parent[ni] = current;
            queue[tail++] = ni;
        }
    }

    if (best == start) return false;
    int step = best;
    while (parent[step] != start && parent[step] != -1) step = parent[step];

    const int sx = startX + (step % Size) - Radius;
    const int sz = startZ + (step / Size) - Radius;
    float y = 0.0f;
    if (!groundProbe(static_cast<float>(sx) + 0.5f, static_cast<float>(sz) + 0.5f, y)) return false;
    next = {static_cast<float>(sx) + 0.5f, y, static_cast<float>(sz) + 0.5f};
    return true;
}

void addPigAt(Vector3 position, float yaw)
{
    PigEntity pig;
    pig.position = position;
    pig.yaw = yaw;
    pig.wanderTimer = 0.8f;
    pig.pathTimer = 0.0f;
    pig.speed = 0.0f;
    pig.hasTarget = false;
    pig.hasPathStep = false;
    pigs.push_back(pig);
}
}

void SpawnPigsAround(Vector3 player, PigGroundProbe groundProbe)
{
    constexpr int EntityCellSize = 32;
    constexpr int EntityCellRadius = 3;
    constexpr int MaxPigs = 28;
    const int centerX = floorDiv(static_cast<int>(std::floor(player.x)), EntityCellSize);
    const int centerZ = floorDiv(static_cast<int>(std::floor(player.z)), EntityCellSize);

    pigs.erase(std::remove_if(pigs.begin(), pigs.end(), [&](const PigEntity& pig) {
        return Vector3DistanceSqr(pig.position, player) > 190.0f * 190.0f;
    }), pigs.end());

    for (int dz = -EntityCellRadius; dz <= EntityCellRadius && static_cast<int>(pigs.size()) < MaxPigs; ++dz)
    {
        for (int dx = -EntityCellRadius; dx <= EntityCellRadius && static_cast<int>(pigs.size()) < MaxPigs; ++dx)
        {
            const ChunkCoord cell{centerX + dx, centerZ + dz};
            if (spawnedPigCells.find(cell) != spawnedPigCells.end()) continue;
            spawnedPigCells.insert(cell);

            if (hash2(cell.x, cell.z) < 0.78f) continue;
            const int count = 1 + static_cast<int>(hash2(cell.x + 19, cell.z - 7) * 2.0f);
            for (int i = 0; i < count && static_cast<int>(pigs.size()) < MaxPigs; ++i)
            {
                const float px = static_cast<float>(cell.x * EntityCellSize) + 4.0f + hash2(cell.x * 11 + i, cell.z * 17) * 24.0f;
                const float pz = static_cast<float>(cell.z * EntityCellSize) + 4.0f + hash2(cell.x * 23, cell.z * 13 + i) * 24.0f;
                float py = 0.0f;
                if (!groundProbe(px, pz, py)) continue;

                addPigAt({px, py, pz}, hash2(cell.x + i * 5, cell.z - i * 9) * PI * 2.0f);
                pigs.back().wanderTimer = 0.5f + hash2(cell.x - i, cell.z + i) * 2.5f;
                pigs.back().speed = 0.5f;
            }
        }
    }
}

void UpdatePigs(float dt, PigGroundProbe groundProbe)
{
    for (PigEntity& pig : pigs)
    {
        pig.wanderTimer -= dt;
        if (pig.wanderTimer <= 0.0f)
        {
            pig.wanderTimer = 1.2f + hash2(static_cast<int>(pig.position.x * 7.0f), static_cast<int>(pig.position.z * 5.0f)) * 2.0f;
            const float angle = hash2(static_cast<int>(pig.position.x * 13.0f), static_cast<int>(pig.position.z * 19.0f)) * PI * 2.0f;
            const float distance = 3.0f + hash2(static_cast<int>(pig.position.x * 3.0f), static_cast<int>(pig.position.z * 3.0f)) * 7.0f;
            pig.target = {
                pig.position.x + std::sin(angle) * distance,
                pig.position.y,
                pig.position.z + std::cos(angle) * distance
            };
            pig.hasTarget = true;
            pig.hasPathStep = false;
            pig.pathTimer = 0.0f;
            pig.speed = 0.9f;
        }

        if (!pig.hasTarget) continue;

        pig.pathTimer -= dt;
        if (!pig.hasPathStep || pig.pathTimer <= 0.0f)
        {
            if (!findPigPathStep(groundProbe, pig.position, pig.target, pig.pathStep))
            {
                pig.hasTarget = false;
                pig.hasPathStep = false;
                pig.speed = 0.0f;
                continue;
            }
            pig.hasPathStep = true;
            pig.pathTimer = 0.35f;
        }

        Vector3 toStep = Vector3Subtract(pig.pathStep, pig.position);
        toStep.y = 0.0f;
        const float distanceToStep = Vector3Length(toStep);
        if (distanceToStep < 0.10f)
        {
            pig.position = pig.pathStep;
            pig.hasPathStep = false;
            if (Vector3DistanceSqr(pig.position, pig.target) < 1.4f)
            {
                pig.hasTarget = false;
                pig.hasPathStep = false;
                pig.speed = 0.0f;
            }
        }
        else
        {
            const Vector3 dir = Vector3Scale(toStep, 1.0f / distanceToStep);
            const float move = std::min(distanceToStep, pig.speed * dt);
            const Vector3 next = Vector3Add(pig.position, Vector3Scale(dir, move));
            pig.position = {next.x, Lerp(pig.position.y, pig.pathStep.y, std::min(1.0f, dt * 10.0f)), next.z};
            pig.yaw = std::atan2(dir.x, dir.z);
        }
    }
}

void SummonPig(Vector3 player, Vector3 forward, PigGroundProbe groundProbe)
{
    Vector3 flatForward{forward.x, 0.0f, forward.z};
    if (Vector3LengthSqr(flatForward) < 0.01f) flatForward = {0.0f, 0.0f, 1.0f};
    flatForward = Vector3Normalize(flatForward);

    for (float distance = 2.0f; distance <= 9.0f; distance += 1.0f)
    {
        const float x = player.x + flatForward.x * distance;
        const float z = player.z + flatForward.z * distance;
        float y = 0.0f;
        if (!groundProbe(x, z, y)) continue;
        addPigAt({x, y, z}, std::atan2(-flatForward.x, -flatForward.z));
        return;
    }

    const int x = static_cast<int>(std::floor(player.x + flatForward.x * 3.0f));
    const int z = static_cast<int>(std::floor(player.z + flatForward.z * 3.0f));
    addPigAt({static_cast<float>(x) + 0.5f, static_cast<float>(terrainHeight(x, z)) + 0.02f, static_cast<float>(z) + 0.5f}, 0.0f);
}

void DrawPigs(Model& pigModel, Vector3 player)
{
    constexpr float PigModelScale = 3.2f;
    constexpr float PigFootOffset = 0.2032f * PigModelScale;
    for (const PigEntity& pig : pigs)
    {
        if (Vector3DistanceSqr(pig.position, player) > 130.0f * 130.0f) continue;
        DrawModelEx(
            pigModel,
            {pig.position.x, pig.position.y + PigFootOffset, pig.position.z},
            {0.0f, 1.0f, 0.0f},
            pig.yaw * RAD2DEG,
            {PigModelScale, PigModelScale, PigModelScale},
            WHITE
        );
    }
}

int PigCount()
{
    return static_cast<int>(pigs.size());
}
