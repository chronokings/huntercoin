#ifndef GAMEMAP_H
#define GAMEMAP_H

namespace Game
{

static const int MAP_WIDTH = 502;
static const int MAP_HEIGHT = 502;

static const int SPAWN_AREA_LENGTH = 9;
static const int NUM_HARVEST_AREAS = 85;

#ifdef GUI
// Visual elements of the map
static const int MAP_LAYERS = 3;          // Map is layered for visual purposes
static const int NUM_TILE_IDS = 571;      // Total number of different tile textures
extern const short GameMap[MAP_LAYERS][MAP_HEIGHT][MAP_WIDTH];
#endif

extern const unsigned char ObstacleMap[MAP_HEIGHT][MAP_WIDTH];

struct HarvestArea
{
    int fraction;
    int x, y, w, h;
};

extern const HarvestArea HarvestAreas[NUM_HARVEST_AREAS];

// The sum of HarvestAreas.fraction
static const int TOTAL_HARVEST = 900;

inline bool IsInsideMap(int x, int y)
{
    return x >= 0 && x < MAP_WIDTH && y >= 0 && y < MAP_HEIGHT;
}

inline bool IsWalkable(int x, int y)
{
    return ObstacleMap[y][x] == 0;
}

inline bool IsInSpawnArea(int x, int y)
{
    return (x == 0 || x == MAP_WIDTH - 1) && (y < SPAWN_AREA_LENGTH || y >= MAP_HEIGHT - SPAWN_AREA_LENGTH)
        || (y == 0 || y == MAP_HEIGHT - 1) && (x < SPAWN_AREA_LENGTH || x >= MAP_WIDTH - SPAWN_AREA_LENGTH);
}

}

#endif
