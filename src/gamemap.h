#ifndef GAMEMAP_H
#define GAMEMAP_H

namespace Game
{

static const int MAP_WIDTH = 502;
static const int MAP_HEIGHT = 502;

static const int SPAWN_AREA_LENGTH = 15;
static const int NUM_HARVEST_AREAS = 18;
static const int NUM_CROWN_LOCATIONS = 416;

static const int CROWN_START_X = 250;
static const int CROWN_START_Y = 248;

#ifdef GUI
// Visual elements of the map
static const int MAP_LAYERS = 3;          // Map is layered for visual purposes
static const int NUM_TILE_IDS = 235;      // Total number of different tile textures
extern const short GameMap[MAP_LAYERS][MAP_HEIGHT][MAP_WIDTH];
#endif

// for FORK_TIMESAVE
extern const unsigned char SpawnMap[MAP_HEIGHT][MAP_WIDTH];
#define SPAWNMAPFLAG_BANK 1
#define SPAWNMAPFLAG_PLAYER 2
#define CHARACTER_MODE_NORMAL 6
// difference of 2 means we can walk over (and along) the player spawn strip without logout
#define CHARACTER_MODE_LOGOUT 8
#define CHARACTER_MODE_SPECTATOR_BEGIN 9
#define CHARACTER_HAS_SPAWN_PROTECTION(S) (S<CHARACTER_MODE_NORMAL)
#define CHARACTER_IS_PROTECTED(S) ((S<CHARACTER_MODE_NORMAL)||(S>CHARACTER_MODE_LOGOUT))
#define CHARACTER_SPAWN_PROTECTION_ALMOST_FINISHED(S) (S==CHARACTER_MODE_NORMAL-1)
#define CHARACTER_IN_SPECTATOR_MODE(S) (S>CHARACTER_MODE_LOGOUT)
#define CHARACTER_NO_LOGOUT(S) ((S!=CHARACTER_MODE_LOGOUT)&&(S<CHARACTER_MODE_SPECTATOR_BEGIN+15))


extern const unsigned char ObstacleMap[MAP_HEIGHT][MAP_WIDTH];

// HarvestAreas[i] has size 2*HarvestAreaSizes[i] and contains alternating x,y coordinates
extern const int *HarvestAreas[NUM_HARVEST_AREAS];
extern const int HarvestAreaSizes[NUM_HARVEST_AREAS];

// Harvest amounts are subject to block reward halving
extern const int HarvestPortions[NUM_HARVEST_AREAS];  // Harvest amounts in cents
static const int TOTAL_HARVEST = 900;                 // Total harvest in cents (includes CROWN_BONUS)
static const int CROWN_BONUS = 25;                    // Bonus for holding Crown of the Fortune in cents

// Locations where the crown can spawn when the crown holder enters spawn area (x,y pairs)
extern const int CrownSpawn[NUM_CROWN_LOCATIONS * 2];

inline bool IsInsideMap(int x, int y)
{
    return x >= 0 && x < MAP_WIDTH && y >= 0 && y < MAP_HEIGHT;
}

inline bool IsWalkable(int x, int y)
{
    return ObstacleMap[y][x] == 0;
}

inline bool IsOriginalSpawnArea(int x, int y)
{
    return ((x == 0 || x == MAP_WIDTH - 1) && (y < SPAWN_AREA_LENGTH || y >= MAP_HEIGHT - SPAWN_AREA_LENGTH))
        || ((y == 0 || y == MAP_HEIGHT - 1) && (x < SPAWN_AREA_LENGTH || x >= MAP_WIDTH - SPAWN_AREA_LENGTH));
}

}

#endif
