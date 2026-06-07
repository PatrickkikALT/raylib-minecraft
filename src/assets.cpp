#include "assets.h"

#include "game_types.h"

namespace
{
void drawAtlasTile(Image& atlas, const char* fileName, int tile)
{
    Image source = LoadImage(fileName);
    if (source.data == nullptr) return;
    const int stride = AtlasTileSize + AtlasTilePadding * 2;

    const Rectangle src{
        0.0f,
        0.0f,
        static_cast<float>(source.width),
        static_cast<float>(source.height)
    };
    const Rectangle paddedDst{
        static_cast<float>(tile * stride),
        0.0f,
        static_cast<float>(stride),
        static_cast<float>(stride)
    };
    const Rectangle dst{
        static_cast<float>(tile * stride + AtlasTilePadding),
        static_cast<float>(AtlasTilePadding),
        static_cast<float>(AtlasTileSize),
        static_cast<float>(AtlasTileSize)
    };
    ImageDraw(&atlas, source, src, paddedDst, WHITE);
    ImageDraw(&atlas, source, src, dst, WHITE);
    UnloadImage(source);
}
}

Texture2D LoadVoxelAtlas()
{
    const int stride = AtlasTileSize + AtlasTilePadding * 2;
    Image atlas = GenImageColor(stride * AtlasTileCount, stride, BLACK);
    drawAtlasTile(atlas, "grassblock.png", 0);
    drawAtlasTile(atlas, "dirt.png", 1);
    drawAtlasTile(atlas, "grass.png", 2);
    drawAtlasTile(atlas, "sand.png", 3);
    drawAtlasTile(atlas, "stone.png", 4);
    drawAtlasTile(atlas, "snow.png", 5);
    drawAtlasTile(atlas, "bedrock.png", 6);

    Texture2D texture = LoadTextureFromImage(atlas);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
    UnloadImage(atlas);
    return texture;
}
