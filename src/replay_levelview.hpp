#pragma once
#include <vector>
#include <cmath>
#include <cocos2d.h>

enum class GDHitboxCategory {
    Player = 0,
    SolidBlock = 1,
    Spike = 2,
    Orb = 3,
    JumpPad = 4,
    Portal = 5,
    Trigger = 6,
    CollisionBlock = 7,
};

enum class GDHitboxShape {
    Rectangle = 0,
    Circle = 1,
    OrientedQuad = 2,
};

using Vec2 =  cocos2d::CCPoint;

struct GDHitboxPrimitive {
    GDHitboxShape shape = GDHitboxShape::Rectangle;
    GDHitboxCategory category = GDHitboxCategory::SolidBlock;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float radius = 0.0f;
    std::vector<Vec2> corners;
    int objectId = 0;
    int runtimeObjectType = 0;
};

struct GDLevelModel {
    std::vector<GDHitboxPrimitive> hitboxes;
    std::vector<float> interactionXs;
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    size_t objectTokenCount = 0;
    size_t parsedObjectCount = 0;
    size_t recognizedHitboxCount = 0;
    int rawLength = 0;
    std::string sourceSection;
};
