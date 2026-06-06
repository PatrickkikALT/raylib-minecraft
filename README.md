# Raylib Minecraft Clone

A compact C++/Raylib voxel sandbox built from the Raylib quickstart template.

## Features

- Infinite deterministic terrain generation around the player
- Chunk streaming with distance-based unloads
- Culled chunk meshes instead of per-block draw calls
- Vertex-colored block types: grass, dirt, stone, sand, snow, water, wood, and leaves
- Runtime texture atlas using the PNG block textures in `resources`
- Basic collision, jumping, flying, raycast mining, and block placement
- Per-frame mesh rebuild throttling to reduce hitches while moving or editing

## Controls

- `WASD`: move
- `Space`: jump
- `Left Shift`: sprint
- `Left Ctrl`: fly mode while held
- `C`: descend while flying
- `Left Mouse`: remove targeted block
- `Right Mouse`: place selected block
- `1` through `7`: select block type
- `Tab`: toggle mouse capture
- `Esc`: quit

## Build

Generate the makefiles:

```powershell
cd build
.\premake5.exe gmake
cd ..
```

Build with the Raylib MinGW toolchain:

```powershell
$env:Path='C:\raylib\w64devkit\bin;' + $env:Path
mingw32-make.exe
```

The debug executable is written to:

```text
bin\Debug\raylib-minecraft.exe
```

## Notes

This is intentionally still small enough to hack on. The main performance win is that each chunk is uploaded as one mesh containing only exposed faces, so the renderer avoids issuing a draw call per cube. The atlas is generated at startup from `grassblock.png`, `dirt.png`, `grass.png`, and `bedrock.png`.
