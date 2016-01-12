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

// better GUI -- more map tiles
static const int NUM_TILE_IDS = 453; // 235;      // Total number of different tile textures

// extern const short GameMap[MAP_LAYERS][MAP_HEIGHT][MAP_WIDTH];
#endif

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


#ifdef GUI
// better GUI -- variables declaration
extern char AsciiArtMap[Game::MAP_HEIGHT + 4][Game::MAP_WIDTH + 4];
extern int AsciiArtTileCount[Game::MAP_HEIGHT + 4][Game::MAP_WIDTH + 4];

#define SHADOW_LAYERS 3
#define SHADOW_EXTRALAYERS 1
#define SHADOW_SHAPES 17
extern int Displaycache_gamemapgood[Game::MAP_HEIGHT][Game::MAP_WIDTH];
extern int Displaycache_gamemap[Game::MAP_HEIGHT][Game::MAP_WIDTH][Game::MAP_LAYERS + SHADOW_LAYERS + SHADOW_EXTRALAYERS];
//extern bool Display_dbg_obstacle_marker;

#define RPG_ICON_EMPTY 276
#define RGP_ICON_HUC_BANDIT 411

//#define RPG_TILE_GRASS_GREEN_DARK 263
//#define RPG_TILE_GRASS_GREEN_LITE 266
//#define RPG_TILE_GRASS_RED_DARK 259
//#define RPG_TILE_GRASS_RED_LITE 262

#define RPG_TILE_TPGLOW 277
#define RPG_TILE_TPGLOW_TINY 304
#define RPG_TILE_TPGLOW_SMALL 305

//#define TILE_IS_TERRAIN(T) (T<=68 ? 1 : ((T==92)||(T==93)||(T==177)||(T==178)||((T>=200)&&(T!=203)&&(T<=208))||((T>=213)&&(T==215))) ? 2 : 0)
#define TILE_IS_GRASS(T) ((T==259) || ((T>=262) && (T<=268)))

#define ASCIIART_IS_TREE(T) ((T=='B') || (T=='b') || (T=='C') || (T=='c'))
#define ASCIIART_IS_ROCK(T) ((T=='G') || (T=='g') || (T=='H') || (T=='h'))
#define ASCIIART_IS_CLIFFBASE(T) ((T=='[') || (T==']') || (T=='!') || (T=='|'))
#define ASCIIART_IS_BASETERRAIN(T) ((T=='0') || (T=='1') || (T=='.'))
#define ASCIIART_IS_WALKABLETERRAIN(T) ((T=='0') || (T=='.'))
#define ASCIIART_IS_CLIFFSIDE(T) ((T=='(') || (T=='{') || (T=='<') || (T==')') || (T=='}') || (T=='>'))
#define ASCIIART_IS_CLIFFSIDE_NEW(T) ((T=='(') || (T=='{') || (T=='<') || (T==')') || (T=='}') || (T=='>') || (T=='i') || (T=='I') || (T=='j') || (T=='J'))
#define ASCIIART_IS_CLIFFSAND(T) ((T==',') || (T==';') || (T==':'))
#define ASCIIART_IS_CLIFFTOP(T) ((T=='?') || (T=='_'))
#define ASCIIART_IS_COBBLESTONE(T) ((T=='o') || (T=='O') || (T=='q') || (T=='Q') || (T=='8'))
#endif


#endif
