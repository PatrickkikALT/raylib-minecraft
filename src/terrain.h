#pragma once

#include "game_types.h"

#include <cstdint>

void SetWorldSeed(uint32_t seed);
uint32_t GetWorldSeed();

int floorDiv(int v, int d);
int floorMod(int v, int d);
float hash2(int x, int z);
int terrainHeight(int x, int z);
int terrainSlope(int x, int z);
uint8_t surfaceBlockFor(int x, int z, int h, int slope);
uint8_t columnBlockAt(int x, int y, int z, int h, int slope);
bool treeAt(int x, int z);
uint8_t generatedBlockAt(int x, int y, int z);
