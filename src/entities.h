#pragma once

#include "raylib.h"

using PigGroundProbe = bool (*)(float x, float z, float& y);

void SpawnPigsAround(Vector3 player, PigGroundProbe groundProbe);
void UpdatePigs(float dt, PigGroundProbe groundProbe);
void SummonPig(Vector3 player, Vector3 forward, PigGroundProbe groundProbe);
void DrawPigs(Model& pigModel, Vector3 player);
int PigCount();
