#include "gamemapview.h"

#include "../gamestate.h"
#include "../gamemap.h"
#include "../util.h"

#include <QImage>
#include <QGraphicsItem>
#include <QGraphicsSimpleTextItem>
#include <QStyleOptionGraphicsItem>
#include <QScrollBar>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimeLine>

#include <boost/foreach.hpp>
#include <cmath>

using namespace Game;

static const int TILE_SIZE = 48;

// Container for graphic objects
// Objects like pen could be made global variables, but sprites would crash, since QPixmap cannot be initialized
// before QApplication is initialized
struct GameGraphicsObjects
{
    // Player sprites for each color and 10 directions (with 0 and 5 being null, the rest are as on numpad)
    // better GUI -- more player sprites
    QPixmap player_sprite[Game::NUM_TEAM_COLORS+1][10];

    QPixmap coin_sprite, heart_sprite, crown_sprite;
    QPixmap tiles[NUM_TILE_IDS];

    // better GUI -- more player sprites
    QBrush player_text_brush[Game::NUM_TEAM_COLORS+1];

    QPen magenta_pen, gray_pen;

    GameGraphicsObjects()
        : magenta_pen(Qt::magenta, 2.0),
        gray_pen(QColor(170, 170, 170), 2.0)
    {
        player_text_brush[0] = QBrush(QColor(255, 255, 100));
        player_text_brush[1] = QBrush(QColor(255, 80, 80));
        player_text_brush[2] = QBrush(QColor(100, 255, 100));
        player_text_brush[3] = QBrush(QColor(0, 170, 255));

        // better GUI -- more player sprites
        player_text_brush[4] = QBrush(QColor(255, 255, 255));

        // better GUI -- more player sprites
        for (int i = 0; i < Game::NUM_TEAM_COLORS+1; i++)

            for (int j = 1; j < 10; j++)
            {
                if (j != 5)
                    player_sprite[i][j].load(":/gamemap/sprites/" + QString::number(i) + "_" + QString::number(j));
            }

        coin_sprite.load(":/gamemap/sprites/coin");
        heart_sprite.load(":/gamemap/sprites/heart");
        crown_sprite.load(":/gamemap/sprites/crown");

        for (short tile = 0; tile < NUM_TILE_IDS; tile++)
            tiles[tile].load(":/gamemap/" + QString::number(tile));
    }
};

// Cache scene objects to avoid recreating them on each state update
class GameMapCache
{
    struct CacheEntry
    {
        bool referenced;
    };    

    class CachedCoin : public CacheEntry
    {
        QGraphicsPixmapItem *coin;
        QGraphicsTextItem *text;
        int64 nAmount;

    public:

        CachedCoin() : coin(NULL) { }

        void Create(QGraphicsScene *scene, const GameGraphicsObjects *grobjs, int x, int y, int64 amount)
        {
            nAmount = amount;
            referenced = true;
            coin = scene->addPixmap(grobjs->coin_sprite);
            coin->setOffset(x, y);
            coin->setZValue(0.1);
            text = new QGraphicsTextItem(coin);
            text->setHtml(
                    "<center>"
                    + QString::fromStdString(FormatMoney(nAmount))
                    + "</center>"
                );
            text->setPos(x, y + 13);
            text->setTextWidth(TILE_SIZE);
        }

        void Update(int64 amount)
        {
            referenced = true;
            if (amount == nAmount)
                return;
            // If only the amount changed, update text
            nAmount = amount;
            text->setHtml(
                    "<center>"
                    + QString::fromStdString(FormatMoney(nAmount))
                    + "</center>"
                );
        }

        operator bool() const { return coin != NULL; }

        void Destroy(QGraphicsScene *scene)
        {
            scene->removeItem(coin);
            delete coin;
            //scene->invalidate();
        }
    };

    class CachedHeart : public CacheEntry
    {
        QGraphicsPixmapItem *heart;

    public:

        CachedHeart() : heart(NULL) { }

        void Create(QGraphicsScene *scene, const GameGraphicsObjects *grobjs, int x, int y)
        {
            referenced = true;
            heart = scene->addPixmap(grobjs->heart_sprite);
            heart->setOffset(x, y);
            heart->setZValue(0.2);
        }

        void Update()
        {
            referenced = true;
        }

        operator bool() const { return heart != NULL; }

        void Destroy(QGraphicsScene *scene)
        {
            scene->removeItem(heart);
            delete heart;
            //scene->invalidate();
        }
    };

    class CachedPlayer : public CacheEntry
    {
        QGraphicsPixmapItem *sprite;

        // better GUI -- better player sprites
        QGraphicsPixmapItem *shadow_sprite1;
        QGraphicsPixmapItem *shadow_sprite2;
        QGraphicsPixmapItem *symbol_sprite_a1;
        QGraphicsPixmapItem *symbol_sprite_d1;
        QGraphicsPixmapItem *symbol_sprite_d2;
        int color_a1, color_d1, color_d2;

        QGraphicsSimpleTextItem *text;
        const GameGraphicsObjects *grobjs;
        QString name;
        int x, y, color, dir;
        int z_order;
        int64 nLootAmount;

        void UpdPos()
        {
            sprite->setOffset(x, y);

            // better GUI -- better player sprites
            shadow_sprite1->setOffset(x, y);
            shadow_sprite2->setOffset(x, y + TILE_SIZE);
            symbol_sprite_a1->setOffset(x, y + 6);
            symbol_sprite_d1->setOffset(x, y + 6);
            symbol_sprite_d2->setOffset(x, y + 12 + 6);

            UpdTextPos(nLootAmount);
        }

        void UpdTextPos(int64 lootAmount)
        {
            if (lootAmount > 0)
                text->setPos(x, std::max(0, y - 20));
            else
                text->setPos(x, std::max(0, y - 12));
        }

        void UpdText()
        {
            if (nLootAmount > 0)
                text->setText(name + "\n" + QString::fromStdString(FormatMoney(nLootAmount)));
            else
                text->setText(name);
        }

        void UpdSprite()
        {
            sprite->setPixmap(grobjs->player_sprite[color][dir]);
        }

        void UpdColor()
        {
            text->setBrush(grobjs->player_text_brush[color]);
        }

    public:

        // better GUI -- better player sprites
        CachedPlayer() : sprite(NULL), shadow_sprite1(NULL), shadow_sprite2(NULL), symbol_sprite_a1(NULL), symbol_sprite_d1(NULL), symbol_sprite_d2(NULL) { }

        void Create(QGraphicsScene *scene, const GameGraphicsObjects *grobjs_, int x_, int y_, int z_order_, QString name_, int color_, int color_a1_, int color_d1_, int color_d2_, int dir_, int64 amount)
        {
            grobjs = grobjs_;
            x = x_;
            y = y_;
            z_order = z_order_;
            name = name_;
            color = color_;
            dir = dir_;
            nLootAmount = amount;
            referenced = true;
            sprite = scene->addPixmap(grobjs->player_sprite[color][dir]);
            sprite->setOffset(x, y);
            sprite->setZValue(z_order);


            // better GUI -- better player sprites
            color_a1 = color_a1_;
            color_d1 = color_d1_;
            color_d2 = color_d2_;
            shadow_sprite1 = scene->addPixmap(grobjs->tiles[260]);
            shadow_sprite1->setOffset(x, y);
            shadow_sprite1->setZValue(z_order);
            shadow_sprite1->setOpacity(0.4);
            shadow_sprite2 = scene->addPixmap(grobjs->tiles[261]);
            shadow_sprite2->setOffset(x, y + TILE_SIZE);
            shadow_sprite2->setZValue(z_order);
            shadow_sprite2->setOpacity(0.4);

            symbol_sprite_a1 = scene->addPixmap(grobjs->tiles[color_a1]);
            symbol_sprite_a1->setOffset(x, y + 6);
            symbol_sprite_a1->setZValue(z_order);

            symbol_sprite_d1 = scene->addPixmap(grobjs->tiles[color_d1]);
            symbol_sprite_d1->setOffset(x, y + 6);
            symbol_sprite_d1->setZValue(z_order);
//            symbol_sprite_d1->setOpacity(color_d1 == RPG_TILE_TPGLOW ? 0.65 : 1.0);

            symbol_sprite_d2 = scene->addPixmap(grobjs->tiles[color_d2]);
            symbol_sprite_d2->setOffset(x, y + 12 + 6);
            symbol_sprite_d2->setZValue(z_order);


            text = scene->addSimpleText("");
            text->setZValue(1e9);
            UpdPos();
            UpdText();
            UpdColor();
        }

        // better GUI -- better player sprites
        void Update(int x_, int y_, int z_order_, int color_, int color_a1_, int color_d1_, int color_d2_, int dir_, int64 amount)
        {
            referenced = true;
            if (amount != nLootAmount)
            {
                if ((amount > 0) != (nLootAmount > 0))
                    UpdTextPos(amount);
                nLootAmount = amount;
                UpdText();
            }
            if (x != x_ || y != y_)
            {
                x = x_;
                y = y_;
                UpdPos();
            }
            if (z_order != z_order_)
            {
                z_order = z_order_;
                sprite->setZValue(z_order);
            }
            if (color != color_)
            {
                color = color_;
                dir = dir_;
                UpdSprite();
                UpdColor();
            }
            else if (dir != dir_)
            {
                dir = dir_;
                UpdSprite();
            }

            // better GUI -- better player sprites
            if (color_a1 != color_a1_)
            {
                color_a1 = color_a1_;
                symbol_sprite_a1->setPixmap(grobjs->tiles[color_a1]);
            }
            if (color_d1 != color_d1_)
            {
                color_d1 = color_d1_;
                symbol_sprite_d1->setPixmap(grobjs->tiles[color_d1]);
//                symbol_sprite_d1->setOpacity(color_d1 == RPG_TILE_TPGLOW ? 0.65 : 1.0);
            }
            if (color_d2 != color_d2_)
            {
                color_d2 = color_d2_;
                symbol_sprite_d2->setPixmap(grobjs->tiles[color_d2]);
            }
        }

        operator bool() const { return sprite != NULL; }

        void Destroy(QGraphicsScene *scene)
        {
            scene->removeItem(sprite);
            scene->removeItem(text);

            // better GUI -- better player sprites
            scene->removeItem(shadow_sprite1);
            scene->removeItem(shadow_sprite2);
            scene->removeItem(symbol_sprite_a1);
            scene->removeItem(symbol_sprite_d1);
            scene->removeItem(symbol_sprite_d2);
            delete shadow_sprite1;
            delete shadow_sprite2;
            delete symbol_sprite_a1;
            delete symbol_sprite_d1;
            delete symbol_sprite_d2;

            delete sprite;
            delete text;
            //scene->invalidate();
        }
    };

    QGraphicsScene *scene;
    const GameGraphicsObjects *grobjs;

    std::map<Coord, CachedCoin> cached_coins;
    std::map<Coord, CachedHeart> cached_hearts;
    std::map<QString, CachedPlayer> cached_players;

    template<class CACHE>
    void EraseUnreferenced(CACHE &cache)
    {
        for (typename CACHE::iterator mi = cache.begin(); mi != cache.end(); )
        {
            if (mi->second.referenced)
                ++mi;
            else
            {
                mi->second.Destroy(scene);
                cache.erase(mi++);
            }
        }
    }

public:

    GameMapCache(QGraphicsScene *scene_, const GameGraphicsObjects *grobjs_)
        : scene(scene_), grobjs(grobjs_)
    {
    }

    void StartCachedScene()
    {
        // Mark each cached object as unreferenced
        for (std::map<Coord, CachedCoin>::iterator mi = cached_coins.begin(); mi != cached_coins.end(); ++mi)
            mi->second.referenced = false;
        for (std::map<Coord, CachedHeart>::iterator mi = cached_hearts.begin(); mi != cached_hearts.end(); ++mi)
            mi->second.referenced = false;
        for (std::map<QString, CachedPlayer>::iterator mi = cached_players.begin(); mi != cached_players.end(); ++mi)
            mi->second.referenced = false;
    }

    void PlaceCoin(const Coord &coord, int64 nAmount)
    {
        CachedCoin &c = cached_coins[coord];

        if (!c)
            c.Create(scene, grobjs, coord.x * TILE_SIZE, coord.y * TILE_SIZE, nAmount);
        else
            c.Update(nAmount);
    }

    void PlaceHeart(const Coord &coord)
    {
        CachedHeart &h = cached_hearts[coord];

        if (!h)
            h.Create(scene, grobjs, coord.x * TILE_SIZE, coord.y * TILE_SIZE);
        else
            h.Update();
    }

    // better GUI -- better player sprites
    void AddPlayer(const QString &name, int x, int y, int z_order, int color, int color_a1, int color_d1, int color_d2, int dir, int64 nAmount)
    {
        CachedPlayer &p = cached_players[name];
        if (!p)
            p.Create(scene, grobjs, x, y, z_order, name, color, color_a1, color_d1, color_d2, dir, nAmount);
        else
            p.Update(x, y, z_order, color, color_a1, color_d1, color_d2, dir, nAmount);
    }

    // Erase unreferenced objects from cache
    void EndCachedScene()
    {
        EraseUnreferenced(cached_coins);
        EraseUnreferenced(cached_hearts);
        EraseUnreferenced(cached_players);
    }
};


// better GUI -- more map tiles
bool Display_dbg_allow_tile_offset = true;
bool Display_dbg_obstacle_marker = false;

int Display_dbg_maprepaint_cachemisses = 0;
int Display_dbg_maprepaint_cachehits = 0;

int Displaycache_grassoffs_x[MAP_HEIGHT][MAP_WIDTH][MAP_LAYERS + SHADOW_LAYERS + SHADOW_EXTRALAYERS];
int Displaycache_grassoffs_y[MAP_HEIGHT][MAP_WIDTH][MAP_LAYERS + SHADOW_LAYERS + SHADOW_EXTRALAYERS];

int Display_go_x[7] = {12, 26, 7, 13, 34, 18, 1};
int Display_go_y[7] = {19, 1, 29, 8, 16, 20, 34};
int Display_go_idx = 0;

uint64_t Display_rng[2] = {98347239859043, 653935414278534};
uint64_t Display_xorshift128plus(void)
{
    uint64_t x = Display_rng[0];
    uint64_t const y = Display_rng[1];
    Display_rng[0] = y;
    x ^= x << 23; // a
    x ^= x >> 17; // b
    x ^= y ^ (y >> 26); // c
    Display_rng[1] = x;
    return x + y;
}

// to parse the asciiart map
#define SHADOWMAP_AAOBJECT_MAX 129
#define SHADOWMAP_AAOBJECT_MAX_ONLY_YELLOW_GRASS 127
#define SHADOWMAP_AAOBJECT_MAX_NO_GRASS 126
int ShadowAAObjects[SHADOWMAP_AAOBJECT_MAX][4] = {{ 0, 0, 'H', 251},  // menhir
                                                  { 0, 0, 'h', 252},
                                                  { 0, 1, 'H', 250},
                                                  { 0, 1, 'h', 253},

                                                  { 0, 0, 'G', 212},  // boulder
                                                  { 0, 0, 'g', 249},

                                                  { 2, 2, 'b', 122},  // broadleaf, bright
                                                  { 1, 2, 'b', 123},
                                                  { 0, 2, 'b', 124},
                                                  { 2, 1, 'b', 138},
                                                  { 1, 1, 'b', 139},
                                                  { 0, 1, 'b', 160},
                                                  { 2, 0, 'b', 156},
                                                  { 1, 0, 'b', 157},
                                                  { 0, 0, 'b', 173},

                                                  { 2, 2, 'B', 117},  // broadleaf, dark
                                                  { 1, 2, 'B', 118},
                                                  { 0, 2, 'B', 119},
                                                  { 2, 1, 'B', 133},
                                                  { 1, 1, 'B', 134},
                                                  { 0, 1, 'B', 135},
                                                  { 2, 0, 'B', 151},
                                                  { 1, 0, 'B', 152},
                                                  { 0, 0, 'B', 153},

                                                  { 1, 2, 'c', 140},  // conifer, bright
                                                  { 0, 2, 'c', 141},
                                                  { 1, 1, 'c', 158},
                                                  { 0, 1, 'c', 159},
                                                  { 1, 0, 'c', 171},
                                                  { 0, 0, 'c', 172},

                                                  { 1, 2, 'C', 120},  // conifer, dark
                                                  { 0, 2, 'C', 121},
                                                  { 1, 1, 'C', 136},
                                                  { 0, 1, 'C', 137},
                                                  { 1, 0, 'C', 154},
                                                  { 0, 0, 'C', 155},

                                                  { 0, 2, 'p', 111},  // big palisade, left
                                                  { 0, 1, 'p', 113},
                                                  { 0, 0, 'p', 115},
                                                  { 0, 2, 'P', 187},  // big palisade, right
                                                  { 0, 1, 'P', 189},
                                                  { 0, 0, 'P', 191},

                                                  { 1, 2, '[', 91},  // cliff, lower left corner
                                                  { 0, 2, '[', 92},
                                                  { 1, 1, '[', 74},
                                                  { 0, 1, '[', 75},
                                                  { 1, 0, '[', 85},
                                                  { 0, 0, '[', 86},

                                                  // alternative terrain version (some tiles converted to terrain)
                                                  { 1, 2, 'm', 91},  // cliff, lower left corner
                                                  { 1, 1, 'm', 74},
                                                  { 1, 0, 'm', 85},
                                                  { 0, 0, 'm', 86},

                                                  // tiles converted to terrain commented out
//                                                { 0, 2, ']', 69},  // cliff, lower right corner
                                                  { -1, 2, ']', 70},
//                                                { 0, 1, ']', 71},
                                                  { -1, 1, ']', 72},
                                                  { 0, 0, ']', 83},
                                                  { -1, 0, ']', 84},

//                                                { 0, 2, '!', 103},  // cliff, lower end of normal column (2 versions)
//                                                { 0, 1, '!', 105},
                                                  { 0, 0, '!', 101},
//                                                { 0, 2, '|', 107},
//                                                { 0, 1, '|', 109},
                                                  { 0, 0, '|', 73},

                                                  { 1, 2, '{', 210},  // cliff, left/right end of normal line (2 versions)
                                                  { 0, 2, '{', 97},
                                                  { 1, 2, '(', 202},
                                                  { 0, 2, '(', 203},
                                                  { 0, 2, '}', 95},
                                                  { -1, 2, '}', 99},
                                                  { 0, 2, ')', 177},
                                                  { -1, 2, ')', 179},

                                                  // alternative terrain version (some tiles converted to terrain)
                                                  { -1, 2, 'j', 99},  // cliff, left/right end of normal line
                                                  { -1, 2, 'J', 179},
                                                  { 1, 2, 'i', 210},
                                                  { 1, 2, 'I', 202},

                                                  { 0, 3, '<', 185},   // cliff, left/right side of "special" line
                                                  { 1, 2, '<', 221},
                                                  { 0, 2, '<', 216},
                                                  { 0, 2, '>', 181},
                                                  { -1, 2, '>', 182},
                                                  { 0, 3, '>', 196},

                                                  { 0, 1, '?', 198},  // cliff, upper end of normal column (2 versions)
                                                  { 0, 0, '?', 200},
                                                  { 0, 1, '_', 218},
                                                  { 0, 0, '_', 213},

                                                  { 0, 1, 'r', 279},  // cliff, "concave" lower right corner
                                                  { -1, 1, 'r', 280},
                                                  { 0, 0, 'r', 281},
                                                  { -1, 0, 'r', 282},

                                                  { 1, 1, 'l', 283},  // cliff, "concave" lower left corner
                                                  { 0, 1, 'l', 284},
                                                  { 1, 0, 'l', 285},
                                                  { 0, 0, 'l', 286},

                                                  // tiles converted to terrain commented out
//                                                { 0, 2, 'R', 287},  // cliff, "concave" upper right corner
//                                                { -1, 2, 'R', 288},
                                                  { 0, 1, 'R', 289},
//                                                { -1, 1, 'R', 290},
                                                  { 0, 0, 'R', 291},
                                                  { -1, 0, 'R', 292},

//                                                { 1, 2, 'L', 293},  // cliff, "concave" upper left corner
//                                                { 0, 2, 'L', 294},
//                                                { 1, 1, 'L', 295},
//                                                { 0, 1, 'L', 296},
//                                                { 1, 0, 'L', 297},
                                                  { 0, 0, 'L', 298},

//                                                { 1, 4, 'Z', 309},  // if columns of cliff tiles get larger by 2 at lower end
//                                                { 0, 4, 'Z', 310},
//                                                { 1, 3, 'Z', 311},
//                                                { 0, 3, 'Z', 312},
                                                  { 1, 2, 'Z', 313},
//                                                { 0, 2, 'Z', 314},
                                                  { 1, 1, 'Z', 315},
                                                  { 0, 1, 'Z', 316},
                                                  { 1, 0, 'Z', 317},
                                                  { 0, 0, 'Z', 318},

//                                                { 1, 4, 'z', 319},  // if columns of cliff tiles get smaller by 2 at lower end
//                                                { 0, 4, 'z', 320},
//                                                { 1, 3, 'z', 321},
//                                                { 0, 3, 'z', 322},
//                                                { 1, 2, 'z', 323},
                                                  { 0, 2, 'z', 324},
                                                  { 1, 1, 'z', 325},
                                                  { 0, 1, 'z', 326},
                                                  { 1, 0, 'z', 327},
                                                  { 0, 0, 'z', 328},

//                                                { 1, 3, 'S', 348}, // if columns of cliff tiles get larger by 1 at lower end
//                                                { 0, 3, 'S', 349},
//                                                { 1, 2, 'S', 350},
//                                                { 0, 2, 'S', 351},
                                                  { 1, 1, 'S', 352},
//                                                { 0, 1, 'S', 353},
                                                  { 1, 0, 'S', 354},
                                                  { 0, 0, 'S', 355},

//                                                { 1, 3, 's', 356}, // if columns of cliff tiles get smaller by 1 at lower end
//                                                { 0, 3, 's', 357},
//                                                { 1, 2, 's', 358},
//                                                { 0, 2, 's', 359},
//                                                { 1, 1, 's', 360},
                                                  { 0, 1, 's', 361},
                                                  { 1, 0, 's', 362},
                                                  { 0, 0, 's', 363},

                                                  { 0, 2, '/', 333}, // if columns of cliff tiles get larger by 1 at upper end (only 5 tiles)
                                                  { 1, 1, '/', 334},
                                                  { 0, 1, '/', 335},
                                                  { 1, 0, '/', 336},
                                                  { 0, 0, '/', 337},

                                                  { 1, 2, '\\', 338}, // if columns of cliff tiles get smaller  by 1 at upper end
                                                  { 0, 2, '\\', 339},
                                                  { 1, 1, '\\', 340},
                                                  { 0, 1, '\\', 341},
                                                  { 1, 0, '\\', 342},
                                                  { 0, 0, '\\', 343},

                                                  { 1, 1, 'U', 231},  // Gate
                                                  { 0, 1, 'U', 232},
                                                  { 1, 0, 'U', 233},
                                                  { 0, 0, 'U', 234},

                                                  { 0, 0, '"', 263},  // grass, green (manually placed)
                                                  { 0, 0, '\'', 266},  // grass, green to yellow (manually placed)
                                                  { 0, 0, 'v', 259},  // red grass (manually placed)

                                                  { 0, 0, '1', 268}, // yellow grass -- "conditional" objects are last in this list
                                                  { 0, 0, '0', 263}, // grass -- "conditional" objects are last in this list
                                                  { 0, 0, '.', 266}, // grass -- "conditional" objects are last in this list
                                                 };
// to parse the asciiart map (shadows)
#define SHADOWMAP_AASHAPE_MAX 72
#define SHADOWMAP_AASHAPE_MAX_CLIFFCORNER 28
int ShadowAAShapes[SHADOWMAP_AASHAPE_MAX][5] = {{ 0, 0, 'C', 'c', 244}, // conifer, important shadow tiles
                                                { 0, -1, 'C', 'c', 247},

                                                { 1, 0, 'B', 'b', 237},  // broadleaf, important shadow tiles
                                                { 0, 0, 'B', 'b', 238},
                                                { 1, -1, 'B', 'b', 240},
                                                { 0, -1, 'B', 'b', 241},

                                                { 0, 0, 'H', 'h', 254},  // menhir
                                                { -1, 0, 'H', 'h', 255},

                                                { 0, 0, 'P', 'p', 412},  // palisades
                                                { 0, -1, 'P', 'p', 427},
                                                { -1, 0, 'P', 'p', 418},
                                                { -1, -1, 'P', 'p', 438},

                                                { 1, 0, 'C', 'c', 243},  // conifer, small shadow tiles (skipped if layers are full)
                                                { -1, 0, 'C', 'c', 245},
                                                { 1, -1, 'C', 'c', 246},
                                                { -1, -1, 'C', 'c', 248},

                                                { 2, 0, 'B', 'b', 236},  // broadleaf, small shadow tiles (skipped if layers are full)
                                                { -1, 0, 'B', 'b', 239},
                                                { -1, -1, 'B', 'b', 242},

                                                { 1, 0, 'G', 'g', 256},  // boulder
                                                { 0, 0, 'G', 'g', 257},
                                                { -1, 0, 'G', 'g', 258},

                                                { 0, 0, 'R', 'R', 364}, // cliff, corner 1
                                                { 0, -1, 'R', 'R', 365},

                                                { 0, 0, 'L', 'L', 366}, // cliff, corner 2
                                                { -1, 0, 'L', 'L', 367},
//                                              { 0, -1, 'L', 'L', 368},
                                                { -1, -1, 'L', 'L', 369},

                                                { -1, 1, '>', '>', 383}, // cliff, right side of special "upper right corner" row (CLIFVEG)
                                                { -2, 1, '>', '>', 384},

                                                { -1, 2, ')', '}', 381}, // cliff, right side of normal row (CLIFVEG)
                                                { -2, 2, ')', '}', 382},
                                                { 0, 1, 'l', 'l', 381}, // cliff, corner 3
                                                { -1, 1, 'l', 'l', 382},

                                                { -1, 2, 'J', 'j', 381}, // cliff, right side of normal row (terrain version)
                                                { -2, 2, 'J', 'j', 382},

                                                { 1, 0, '[', 'm', 395},  // cliff, lower left corner (CLIFVEG)
                                                { 0, 0, '[', 'm', 396},
                                                { 1, -1, '[', 'm', 397},
                                                { 0, -1, '[', 'm', 398},

                                                { -1, 2, ']', ']', 401},  // cliff, lower right corner (CLIFVEG)
                                                { -2, 2, ']', ']', 402},
                                                { -1, 1, ']', ']', 403},
                                                { -2, 1, ']', ']', 404},
                                                { 0, 0, ']', ']', 405},
                                                { -1, 0, ']', ']', 406},
                                                { -2, 0, ']', ']', 407},
                                                { 0, -1, ']', ']', 408},
                                                { -1, -1, ']', ']', 409},
                                                { -2, -1, ']', ']', 410},

                                                { 1, 2, 'Z', 'Z', 370}, // if columns of cliff tiles get larger by 2 at lower end
                                                { 1, 1, 'Z', 'Z', 371},
                                                { 1, 0, 'Z', 'Z', 372},
                                                { 0, 0, 'Z', 'Z', 373},
                                                { 0, -1, 'Z', 'Z', 374},

                                                { 0, 2, 'z', 'z', 375}, //                               smaller by 2 at lower end
                                                { 0, 1, 'z', 'z', 376},
                                                { 1, 0, 'z', 'z', 377},
                                                { 0, 0, 'z', 'z', 378},
                                                { 1, -1, 'z', 'z', 379},
                                                { 0, -1, 'z', 'z', 380},

                                                { 1, 1, 'S', 'S', 385}, // if columns of cliff tiles get larger by 1 at lower end
                                                { 1, 0, 'S', 'S', 386},
                                                { 0, 0, 'S', 'S', 387},
                                                { 1, -1, 'S', 'S', 388},
                                                { 0, -1, 'S', 'S', 389},
                                                { 0, 1, 's', 's', 390}, //                               smaller by 1 at lower end
                                                { 1, 0, 's', 's', 391},
                                                { 0, 0, 's', 's', 392},
                                                { 1, -1, 's', 's', 393},
                                                { 0, -1, 's', 's', 394},

                                                { 0, 0, '!', '|', 399}, // cliff, lower side of normal column (CLIFVEG)
                                                { 0, -1, '!', '|', 400}};


class GameMapLayer : public QGraphicsItem
{
    int layer;
    const GameGraphicsObjects *grobjs;

public:
    GameMapLayer(int layer_, const GameGraphicsObjects *grobjs_, QGraphicsItem *parent = 0)
        : QGraphicsItem(parent), layer(layer_), grobjs(grobjs_)
    {
        setFlag(QGraphicsItem::ItemUsesExtendedStyleOption, true);
    }

    QRectF boundingRect() const
    {
        return QRectF(0, 0, MAP_WIDTH * TILE_SIZE, MAP_HEIGHT * TILE_SIZE);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
    {
        Q_UNUSED(widget)

        int x1 = std::max(0, int(option->exposedRect.left()) / TILE_SIZE);
        int x2 = std::min(MAP_WIDTH, int(option->exposedRect.right()) / TILE_SIZE + 1);
        int y1 = std::max(0, int(option->exposedRect.top()) / TILE_SIZE);
        int y2 = std::min(MAP_HEIGHT, int(option->exposedRect.bottom()) / TILE_SIZE + 1);

        // allow offset for some tiles without popping
        if (Display_dbg_allow_tile_offset)
        {
            if (x1 > 0) x1--;
            if (y1 > 0) y1--;
            if (x2 < MAP_WIDTH - 1) x2++;
            if (y2 < MAP_HEIGHT - 1) y2++;
        }

        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++)
            {
                // better GUI -- more map tiles
                // insert shadows
                if ((layer > 0) && (layer <= SHADOW_LAYERS))
                {
                    int stile = 0;
                    int stile1 = 0;
                    int stile2 = 0;
                    int stile3 = 0;

                    // parse asciiart map
                    if (Displaycache_gamemapgood[y][x] < SHADOW_LAYERS + 1)
                    {
                        Displaycache_gamemapgood[y][x] = SHADOW_LAYERS + 1;

                        if ((SHADOW_LAYERS > 1) && (layer > 1)) break;

                        bool is_cliffcorner = false;
                        bool is_palisade = false;
                        for (int m = 0; m < SHADOWMAP_AASHAPE_MAX; m++)
                        {
                            int u = x + ShadowAAShapes[m][0];
                            int v = y + ShadowAAShapes[m][1];

                            // AsciiArtMap array bounds
                            if ((u < 0) || (v < 0) || (u >= MAP_WIDTH + 4) || (v >= MAP_HEIGHT + 4))
                               continue;

                            if ((is_cliffcorner) && (m >= SHADOWMAP_AASHAPE_MAX_CLIFFCORNER))
                                break;

                            if ((AsciiArtMap[v][u] == ShadowAAShapes[m][2]) || (AsciiArtMap[v][u] == ShadowAAShapes[m][3]))
                            {
                                // cache data for all shadow layers in 1 pass (at layer 1)
                                // todo: do this with non-shadow tiles too
                                stile = ShadowAAShapes[m][4];

                                // palisade shadows need custom logic
                                if (((stile == 427) || (stile == 418) || (stile == 438)     || (stile == 412)) &&
                                        (x > 0) && (y > 0) && (y < MAP_HEIGHT - 1))
                                {
                                    if (is_palisade)
                                    {
                                        continue; // only 1 palisade shadow per tile
                                    }
                                    else
                                    {
                                        is_palisade = true;
                                    }

                                    char terrain_W = AsciiArtMap[y][x - 1];
                                    char terrain_SW = AsciiArtMap[y + 1][x - 1];
                                    char terrain_NW = AsciiArtMap[y - 1][x - 1];
                                    if (stile == 427)
                                    {
                                        if ((terrain_W == 'P') || (terrain_W == 'p') || (terrain_SW == 'P') || (terrain_SW == 'p'))
                                            stile = 413;
                                        else if ((terrain_NW == 'P') || (terrain_NW == 'p'))
                                             stile = 432;
                                    }
                                    else if (stile == 418)
                                    {
                                        if ((terrain_NW == 'P') || (terrain_NW == 'p'))
                                            stile = 421;
                                    }
                                }

                                if ((stile <= 0) || (stile >= NUM_TILE_IDS))
                                    continue;

                                if (!stile1)
                                {
                                    stile1 = stile;
                                    Displaycache_gamemap[y][x][1] = stile;
                                }
                                else if ((!stile2) && (SHADOW_LAYERS >= 2))
                                {
                                    stile2 = stile;
                                    Displaycache_gamemap[y][x][2] = stile;
                                }
                                else if ((!stile3) && (SHADOW_LAYERS >= 3))
                                {
                                    stile3 = stile;
                                    Displaycache_gamemap[y][x][3] = stile;
                                }
                                else
                                {
                                    continue;
                                }

                                painter->setOpacity(0.4);
                                painter->drawPixmap(x * TILE_SIZE, y * TILE_SIZE, grobjs->tiles[stile]);
                                painter->setOpacity(1);

                                // shadows of 1 of the cliff corners need custom logic
                                if ((AsciiArtMap[v][u] == 'L') || (AsciiArtMap[v][u] == 'R') || (AsciiArtMap[v][u] == '>'))
                                    is_cliffcorner = true;
                            }
                        }
                        Display_dbg_maprepaint_cachemisses++;
                    }
                    else
                    {
                        stile = Displaycache_gamemap[y][x][layer];

                        if (stile)
                        {
                            Display_dbg_maprepaint_cachehits++;

                            painter->setOpacity(0.4);
                            painter->drawPixmap(x * TILE_SIZE, y * TILE_SIZE, grobjs->tiles[stile]);
                            painter->setOpacity(1);
                        }
                    }

                    continue; // it's a shadow layer
                }

                int tile = 0;
                int grassoffs_x = 0;
                int grassoffs_y = 0;

                if (Displaycache_gamemapgood[y][x] < layer+1)
                {
                    int l = layer - SHADOW_LAYERS > 0 ? layer - SHADOW_LAYERS : 0;
                    int l_free = 1;

                    if (!layer)
                    {
                        char terrain = AsciiArtMap[y][x];
                        char terrain_N = (y > 0) ? AsciiArtMap[y - 1][x] : '0';
                        char terrain_W = (x > 0) ? AsciiArtMap[y][x - 1] : '0';
                        char terrain_E = AsciiArtMap[y][x + 1];
                        char terrain_S = AsciiArtMap[y + 1][x];
                        char terrain_S2 = AsciiArtMap[y + 2][x];

                        // gate on cobblestone (or on something else)
                        if (terrain == 'U')
                        {
                            terrain = terrain_W;
                        }

                        // tiles converted to terrain
                        // cliff, lower left corner
                        if (terrain_S == 'm') tile = 75;
                        else if (terrain_S2 == 'm') tile = 92;

                        // cliff, lower right corner
                        else if (terrain_S == ']') tile = 71;
                        else if (AsciiArtMap[y + 2][x] == ']') tile = 69;

                        // cliff, lower end of normal column (2 versions)
                        else if (terrain_S == '!') tile = 105;
                        else if (AsciiArtMap[y + 2][x] == '!') tile = 103;
                        else if (terrain_S == '|') tile = 109;
                        else if (AsciiArtMap[y + 2][x] == '|') tile = 107;

                        // cliff, left/right end of normal line (alternative terrain version)
                        else if (terrain_S2 == 'j') tile = 95;
                        else if (terrain_S2 == 'J') tile = 177;
                        else if (terrain_S2 == 'i') tile = 97;
                        else if (terrain_S2 == 'I') tile = 203;

                        // cliff, "concave" upper right corner
                        else if (terrain_S2 == 'R') tile = 287;
                        else if ((x >= 1) && (AsciiArtMap[y + 2][x - 1] == 'R')) tile = 288;
                        else if ((x >= 1) && (AsciiArtMap[y + 1][x - 1] == 'R')) tile = 290;

                        // cliff, "concave" upper left corner
                        else if (AsciiArtMap[y + 2][x + 1] == 'L') tile = 293;
                        else if (AsciiArtMap[y + 2][x] == 'L') tile = 294;
                        else if (AsciiArtMap[y + 1][x + 1] == 'L') tile = 295;
                        else if (AsciiArtMap[y + 1][x] == 'L') tile = 296;
                        else if (AsciiArtMap[y][x + 1] == 'L') tile = 297;

                        // if columns of cliff tiles get larger by 2 at lower end
                        else if (AsciiArtMap[y + 4][x + 1] == 'Z') tile = 309;
                        else if (AsciiArtMap[y + 4][x] == 'Z') tile = 310;
                        else if (AsciiArtMap[y + 3][x + 1] == 'Z') tile = 311;
                        else if (AsciiArtMap[y + 3][x] == 'Z') tile = 312;
                        else if (AsciiArtMap[y + 2][x] == 'Z') tile = 314;

                        // if columns of cliff tiles get smaller by 2 at lower end
                        else if (AsciiArtMap[y + 4][x + 1] == 'z') tile = 319;
                        else if (AsciiArtMap[y + 4][x] == 'z') tile = 320;
                        else if (AsciiArtMap[y + 3][x + 1] == 'z') tile = 321;
                        else if (AsciiArtMap[y + 3][x] == 'z') tile = 322;
                        else if (AsciiArtMap[y + 2][x + 1] == 'z') tile = 323;

                        // if columns of cliff tiles get larger by 1 at lower end
                        else if (AsciiArtMap[y + 3][x + 1] == 'S') tile = 348;
                        else if (AsciiArtMap[y + 3][x] == 'S') tile = 349;
                        else if (AsciiArtMap[y + 2][x + 1] == 'S') tile = 350;
                        else if (AsciiArtMap[y + 2][x] == 'S') tile = 351;
                        else if (terrain_S == 'S')  tile = 353; // special

                        // if columns of cliff tiles get smaller by 1 at lower end
                        else if (AsciiArtMap[y + 3][x + 1] == 's') tile = 356;
                        else if (AsciiArtMap[y + 3][x] == 's') tile = 357;
                        else if (AsciiArtMap[y + 2][x + 1] == 's') tile = 358;
                        else if (AsciiArtMap[y + 2][x] == 's') tile = 359;
                        else if (AsciiArtMap[y + 1][x + 1] == 's') tile = 360;

                        // water
                        else if (terrain == 'w')
                        {
                            tile = 299;
                        }
                        else if (terrain == 'W')
                        {
                            if (y % 2) tile = x % 2 ? 329 : 330;
                            else tile = x % 2 ? 331 : 332;
                        }

                        else if (terrain == ';') tile = 68; // sand
                        else if (terrain == ':') tile = 205; // sand
                        else if (terrain == ',') tile = 204; // sand
                        else if (terrain == 'v')             // red grass on sand
                        {
                            tile = 205;
                        }
                        else if (terrain == 'o') tile = 31; // cobblestone
                        else if (terrain == 'O') tile = 32; // cobblestone
                        else if (terrain == 'q') tile = 33; // cobblestone
                        else if (terrain == 'Q') tile = 37; // cobblestone
                        else if (terrain == '8') tile = 38; // cobblestone
                        else if (terrain == '9')            // special cobblestone, small vertical road
                        {
                            if (terrain_W == '9') tile = 34;
                            else tile = 30;
                        }
                        else if (terrain == '6')            // special cobblestone, small horizontal road
                        {
                            if (terrain_N == '6') tile = 35;
                            else tile = 28;
                        }

                        else if (terrain == '.')
                        {
                            tile = 1;

                            char terrain_SE = (y < MAP_HEIGHT - 1) && (x < MAP_WIDTH - 1) ? AsciiArtMap[y + 1][x + 1] : '0';
                            char terrain_NE = (y > 0) && (x < MAP_WIDTH - 1) ? AsciiArtMap[y - 1][x + 1] : '0';
                            char terrain_NW = (y > 0) && (x > 0) ? AsciiArtMap[y - 1][x - 1] : '0';
                            char terrain_SW = (y < MAP_HEIGHT - 1) && (x > 0) ? AsciiArtMap[y + 1][x - 1] : '0';

                            if (ASCIIART_IS_COBBLESTONE(terrain_S))
                            {
                                if (ASCIIART_IS_COBBLESTONE(terrain_W))
                                    tile = 39;
                                else if (ASCIIART_IS_COBBLESTONE(terrain_E))
                                    tile = 36;
                                else
                                    tile = 28;
                            }
                            else if (ASCIIART_IS_COBBLESTONE(terrain_N))
                            {
                                if (ASCIIART_IS_COBBLESTONE(terrain_W))
                                    tile = 31; // fixme
                                else if (ASCIIART_IS_COBBLESTONE(terrain_E))
                                    tile = 31; // fixme
                                else
                                    tile = 35;
                            }
                            else if (ASCIIART_IS_COBBLESTONE(terrain_W))
                            {
                                tile = 34;
                            }
                            else if (ASCIIART_IS_COBBLESTONE(terrain_E))
                            {
                                tile = 30;
                            }
                            else if (ASCIIART_IS_COBBLESTONE(terrain_SE))
                            {
                                tile = 27;
                            }
                            else if (ASCIIART_IS_COBBLESTONE(terrain_NE))
                            {
                                tile = 54;
                            }
                            else if (ASCIIART_IS_COBBLESTONE(terrain_NW))
                            {
                                tile = 55;
                            }
                            else if (ASCIIART_IS_COBBLESTONE(terrain_SW))
                            {
                                tile = 29;
                            }
                        }

                        // tree on cliff
                        // rocks on cliff
                        // and also cliff side tiles that don't have normal terrain as background
                        else if ((ASCIIART_IS_TREE(terrain)) || (ASCIIART_IS_ROCK(terrain)))
                        {
                            if ((ASCIIART_IS_CLIFFSAND(terrain_S)) || (terrain_S == 'v'))
                                tile = 68;                                                      // also sand
                        }
                        // cliff on cliff
                        // cliff on water
                        else if ((ASCIIART_IS_CLIFFBASE(terrain)) || (terrain == 'L') || (terrain == 'R') || (terrain == '#') || (terrain == 'S') || (terrain == 's' || (terrain == 'Z') || (terrain == 'z')))
                        {
                            if ((terrain_S == ';') || (terrain_S == ':') || (terrain_S == ',')) // sand
                                tile = 68;                                                      // also sand
                            else if (terrain_S == 'w')
                                tile = 299;
                            else if (terrain_S == 'W')
                            {
                                if (y % 2) tile = x % 2 ? 329 : 330;
                                else tile = x % 2 ? 331 : 332;
                            }

                        }
                        // cliff side tiles are painted at 1 cliff height (2 rows) vertical offset
                        else if (ASCIIART_IS_CLIFFSIDE_NEW(terrain))
                        {
                            if (tile == 0) tile = 68;                                           // change default terrain tile from grass to sand
                        }

                        if (tile == 0)
                        {
                            bool dirt_S = (terrain_S == '.');
                            bool dirt_N = (terrain_N == '.');
                            bool dirt_E = (terrain_E == '.');
                            bool dirt_W = (terrain_W == '.');
                            bool dirt_SE = ((y < MAP_HEIGHT - 1) && (x < MAP_WIDTH - 1) && (AsciiArtMap[y + 1][x + 1] == '.'));
                            bool dirt_NE = ((y > 0) && (x < MAP_WIDTH - 1) && (AsciiArtMap[y - 1][x + 1] == '.'));
                            bool dirt_NW = ((y > 0) && (x > 0) && (AsciiArtMap[y - 1][x - 1] == '.'));
                            bool dirt_SW = ((y < MAP_HEIGHT - 1) && (x > 0) && (AsciiArtMap[y + 1][x - 1] == '.'));

                            if (dirt_S)
                            {
                                if (dirt_W)
                                {
                                    if (dirt_NE) tile = 1;
                                    else tile = 20;
                                }
                                else if (dirt_E)
                                {
                                    if (dirt_NW) tile = 1;
                                    else tile = 26;
                                }
                                else
                                {
                                    if (dirt_N) tile = 1;
                                    else if (dirt_NW) tile = 20;   // 3/4 dirt SW
                                    else if (dirt_NE) tile = 26;   // 3/4 dirt SE
                                    else tile = 4;                 // 1/2 dirt S
                                }
                            }
                            else if (dirt_N)
                            {
                                if (dirt_W)
                                {
                                    if (dirt_SE) tile = 1;
                                    else if (dirt_NE || dirt_SW) tile = 15; // or tile = 19;   3/4 dirt NW
                                    else tile = 19;                                         // 3/4 dirt NW
                                }
                                else if (dirt_E)
                                {
                                    if (dirt_SW) tile = 1;
                                    else if (dirt_NW || dirt_SE) tile = 14; // or tile = 23;   3/4 dirt NE
                                    else tile = 23;                                         // 3/4 dirt NE
                                }
                                else
                                {
                                    if (dirt_S) tile = 1;
                                    else if (dirt_SW) tile = 15; // 3/4 dirt NW
                                    else if (dirt_SE) tile = 14; // 3/4 dirt NE
                                    else tile = 21;              // 1/2 dirt N
                                }
                            }
                            else if (dirt_W)
                            {
                                if (dirt_NE) tile = 19;      //  3/4 dirt NW
                                else if (dirt_SE) tile = 20; //  3/4 dirt SW
                                else if (dirt_E) tile = 1;
                                else tile = 10;              //  1/2 dirt W
                            }
                            else if (dirt_E)
                            {
                                if (dirt_NW) tile = 23;      //  3/4 dirt NE
                                else if (dirt_SW) tile = 26; //  3/4 dirt SE
                                else if (dirt_W) tile = 1;
                                else tile = 9;               //  1/2 dirt E
                            }
                            else if (dirt_SE)
                            {
                                tile = 6; // 1/4 dirt SE
                            }
                            else if (dirt_NE)
                            {
                                tile = 25; // 1/4 dirt NE
                            }
                            else if (dirt_NW)
                            {
                                tile = 24; // 1/4 dirt NW
                            }
                            else if (dirt_SW)
                            {
                                tile = 5; // 1/4 dirt SW
                            }
                            else
                            {
                                bool sand_S = ASCIIART_IS_CLIFFSAND(terrain_S);
                                bool sand_N = ASCIIART_IS_CLIFFSAND(terrain_N);
                                bool sand_E = ASCIIART_IS_CLIFFSAND(terrain_E);
                                bool sand_W = ASCIIART_IS_CLIFFSAND(terrain_W);
                                bool sand_SE = (ASCIIART_IS_CLIFFSAND(AsciiArtMap[y + 1][x + 1]));
                                bool sand_NE = ((y > 0) && (ASCIIART_IS_CLIFFSAND(AsciiArtMap[y - 1][x + 1])));
                                bool sand_NW = ((y > 0) && (x > 0) && (ASCIIART_IS_CLIFFSAND(AsciiArtMap[y - 1][x - 1])));
                                bool sand_SW = ((x > 0) && (ASCIIART_IS_CLIFFSAND(AsciiArtMap[y + 1][x - 1])));

                                if (sand_S)
                                {
                                    if (sand_W)
                                    {
                                        if (sand_NE) tile = 68;  // sand
                                        else tile = 450;
                                    }
                                    else if (sand_E)
                                    {
                                        if (sand_NW) tile = 68;  // sand
                                        else tile = 449;
                                    }
                                    else
                                    {
                                        if (sand_N) tile = 68;  // sand
                                        else if (sand_NW) tile = 450;   // 3/4 sand SW
                                        else if (sand_NE) tile = 449;   // 3/4 sand SE
                                        else tile = 442;                // 1/2 sand S
                                    }
                                }
                                else if (sand_N)
                                {
                                    if (sand_W)
                                    {
                                        if (sand_SE) tile = 68;  // sand
                                        else tile = 452;         // 3/4 sand NW
                                    }
                                    else if (sand_E)
                                    {
                                        if (sand_SW) tile = 68;  // sand
                                        else tile = 451;         // 3/4 sand NE
                                    }
                                    else
                                    {
                                        if (sand_S) tile = 68;  // sand
                                        else if (sand_SW) tile = 452; // 3/4 sand NW
                                        else if (sand_SE) tile = 451; // 3/4 sand NE
                                        else tile = 447;              // 1/2 sand N
                                    }
                                }
                                else if (sand_W)
                                {
                                    if (sand_NE) tile = 452;      // 3/4 sand NW
                                    else if (sand_SE) tile = 450; // 3/4 sand SW
                                    else if (sand_E) tile = 68;   // sand
                                    else tile = 445;              // 1/2 sand W
                                }
                                else if (sand_E)
                                {
                                    if (sand_NW) tile = 451;      // 3/4 sand NE
                                    else if (sand_SW) tile = 449; // 3/4 sand SE
                                    else if (sand_W) tile = 68;   // sand
                                    else tile = 444;              // 1/2 sand E
                                }
                                else if (sand_SE)
                                {
                                    tile = 441; // 1/4 sand SE
                                }
                                else if (sand_NE)
                                {
                                    tile = 446; // 1/4 sand NE
                                }
                                else if (sand_NW)
                                {
                                    tile = 448; // 1/4 sand NW
                                }
                                else if (sand_SW)
                                {
                                    tile = 443; // 1/4 sand SW
                                }
                            }
                        }
                    }

                    // insert mapobjects from asciiart map (that can cast shadows)
                    if (l)
                    {
                        int off_min = -1;
                        int off_mid = -1;
                        int off_max = -1;
                        int tile_min = 0;
                        int tile_mid = 0;
                        int tile_max = 0;
                        int m_max = SHADOWMAP_AAOBJECT_MAX_NO_GRASS;

                        // insert grass if desired (and possible)
                        if (AsciiArtTileCount[y][x] < 3) // MAP_LAYERS - 1 + SHADOW_EXTRALAYERS  == 3
                        if (Display_dbg_obstacle_marker)
                        {
                            // if we need yellow grass as marker for unwalkable tiles
//                            if (AsciiArtMap[y][x] == '1')
                            if (ObstacleMap[y][x] == 1)
                            {
                                bool need_grass = true;

                                if ((x > 0) && (y > 0) && (x < MAP_WIDTH - 1) && (y < MAP_HEIGHT - 2)) // adjacent tile + 2 tiles south ok
                                {
                                    // skip if either hidden behind trees/cliffs or if this tile would be unwalkable anyway
                                    // (still needed because AsciiArtTileCount only counts trees and rocks, not cliffs)
                                    char c_east = AsciiArtMap[y][x + 1];
                                    char c_west = AsciiArtMap[y][x - 1];
//                                    char c_north = AsciiArtMap[y - 1][x];
                                    char c_se = AsciiArtMap[y + 1][x + 1];
                                    char c_south = AsciiArtMap[y + 1][x];
                                    char c_south2 = AsciiArtMap[y + 2][x];
                                    if ((c_east == 'C') || (c_east == 'c')) need_grass = false;
                                    if ((c_east == 'B') || (c_east == 'b') || (c_se == 'B') || (c_se == 'b')) need_grass = false;
                                    if ((c_south == '<') || (c_south == '>') || (c_south2 == '<') || (c_south2 == '>')) need_grass = false;
                                    if ((c_south == '!') || (c_south == '|') || (c_south2 == '!') || (c_south2 == '|')) need_grass = false;

                                    // skip on some tiles
                                    if ((ASCIIART_IS_CLIFFSIDE(c_east)) || (ASCIIART_IS_CLIFFSIDE(c_west)))
                                        need_grass = false;
                                }

                                if (need_grass)
                                {
                                    // skip if all adjacent tiles are unreachable
                                    need_grass = false;
                                    for (int v = y - 1; v <= y + 1; v++)
                                    {
                                        for (int u = x - 1; u <= x + 1; u++)
                                        {
                                            if ((u < 0) || (v < 0) || (u >= MAP_WIDTH) || (v >= MAP_HEIGHT)) // if (!(IsInsideMap((u, v))))
                                                continue;
                                            if ((u == x) && (v == y))
                                                continue;
                                            if (ObstacleMap[v][u] == 0)
//                                            if (Distance_To_POI[POIINDEX_CENTER][v][u] >= 0)
                                            {
                                                need_grass = true;
                                                break;
                                            }
                                        }
                                        if (need_grass) break;
                                    }
                                }

                                if (need_grass) m_max = SHADOWMAP_AAOBJECT_MAX_ONLY_YELLOW_GRASS;
                            }
                        }

                        // sort and insert mapobjects from asciiart map into 1 of 3 possible layers
                        for (int m = 0; m < m_max; m++)
                        {
                            int x_offs = ShadowAAObjects[m][0];   int u = x + x_offs;
                            int y_offs = ShadowAAObjects[m][1];   int v = y + y_offs;

                            // need 2 additional lines (2 tiles offset for cliffs because of their "height")
                            if ((u < 0) || (v < 0) || (u >= MAP_WIDTH) || (v >= MAP_HEIGHT + 2))
                                continue;

                            if ((AsciiArtMap[v][u] == ShadowAAObjects[m][2]) &&
                                (Displaycache_gamemap[y][x][0] != ShadowAAObjects[m][3]))
                            {
                                int off = y_offs * 10 + x_offs;
                                if (!tile_min)
                                {
                                    tile_min = ShadowAAObjects[m][3];
                                    off_min = off;
                                }
                                else if (off < off_min) // lower offset == farther away == lower layer
                                {
                                    if (tile_mid)
                                    {
                                        tile_max = tile_mid;
                                        off_max = off_mid;
                                    }
                                    tile_mid = tile_min;
                                    off_mid = off_min;
                                    tile_min = ShadowAAObjects[m][3];
                                    off_min = off;
                                }
                                else if (!tile_mid)
                                {
                                    tile_mid = ShadowAAObjects[m][3];
                                    off_mid = off;
                                }
                                else if (off < off_mid)
                                {
                                    tile_max = tile_mid;
                                    off_max = off_mid;
                                    tile_mid = ShadowAAObjects[m][3];
                                    off_mid = off;
                                }
                                else
                                {
                                    tile_max = ShadowAAObjects[m][3];
                                    off_max = off;
                                }
                            }
                        }


                        if (l == l_free)
                        {
                            if (tile_min) tile = tile_min;
                        }
                        else if (l == l_free + 1)
                        {
                            if ((tile_mid) && (tile_mid != tile_min)) tile = tile_mid;
                        }
                        else if (l == l_free + 2)
                        {
                            if ((tile_max) && (tile_mid != tile_max)) tile = tile_max;
                        }
                    }

                    if (TILE_IS_GRASS(tile))
                    {
                        if (tile == 259)
                            if (Display_xorshift128plus() & 1)
                                tile = 262;

                        if ((AsciiArtMap[y][x] == '"') || (AsciiArtMap[y][x] == '\'') || (AsciiArtMap[y][x] == 'v'))
                        {
                            Display_go_idx++;
                            if ((Display_go_idx >= 7) || (Display_go_idx < 0)) Display_go_idx = 0;
                            grassoffs_x = Display_go_x[Display_go_idx];
                            grassoffs_y = Display_go_y[Display_go_idx];
                            Displaycache_grassoffs_x[y][x][layer] = grassoffs_x;
                            Displaycache_grassoffs_y[y][x][layer] = grassoffs_y;
                        }
                    }
                    else
                    {
                         Displaycache_grassoffs_x[y][x][layer] = grassoffs_x = 0;
                         Displaycache_grassoffs_y[y][x][layer] = grassoffs_y = 0;
                    }

                    Displaycache_gamemapgood[y][x] = layer+1;
                    if (!Displaycache_gamemap[y][x][layer]) Displaycache_gamemap[y][x][layer] = tile;
                    Display_dbg_maprepaint_cachemisses++;
                }
                else
                {
                    tile = Displaycache_gamemap[y][x][layer];
                    grassoffs_x = Displaycache_grassoffs_x[y][x][layer];
                    grassoffs_y = Displaycache_grassoffs_y[y][x][layer];

                    Display_dbg_maprepaint_cachehits++;
                }


                // Tile 0 denotes grass in layer 0 and empty cell in other layers
                if (!tile && layer)
                    continue;

                float tile_opacity = 1.0;
                if ((tile == RPG_TILE_TPGLOW) || (tile == RPG_TILE_TPGLOW_SMALL) || (tile == RPG_TILE_TPGLOW_TINY)) tile_opacity = 0.65;
                else if ((tile >= 299) && (tile <= 303)) tile_opacity = 0.78; // water
                else if ((tile >= 329) && (tile <= 332)) tile_opacity = 0.78; // blue water

                if (tile_opacity < 0.99) painter->setOpacity(tile_opacity);
                painter->drawPixmap(x * TILE_SIZE + grassoffs_x, y * TILE_SIZE + grassoffs_y, grobjs->tiles[tile]);
                if (tile_opacity < 0.99) painter->setOpacity(1.0);
            }

//       printf("repaint gamemap: cache hits %d misses %d\n", Display_dbg_maprepaint_cachehits, Display_dbg_maprepaint_cachemisses);
    }
};

GameMapView::GameMapView(QWidget *parent)
    : QGraphicsView(parent),
      grobjs(new GameGraphicsObjects()),
      zoomFactor(1.0),
      playerPath(NULL), queuedPlayerPath(NULL), banks(),
      panning(false), use_cross_cursor(false), scheduledZoom(1.0)
{
    scene = new QGraphicsScene(this);

    scene->setItemIndexMethod(QGraphicsScene::BspTreeIndex);
    scene->setBspTreeDepth(15);

    setScene(scene);
    setSceneRect(0, 0, MAP_WIDTH * TILE_SIZE, MAP_HEIGHT * TILE_SIZE);
    centerOn(MAP_WIDTH * TILE_SIZE / 2, MAP_HEIGHT * TILE_SIZE / 2);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);

    gameMapCache = new GameMapCache(scene, grobjs);

    setOptimizationFlags(QGraphicsView::DontSavePainterState);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);

    setBackgroundBrush(QColor(128, 128, 128));

    defaultRenderHints = renderHints();

    animZoom = new QTimeLine(350, this);
    animZoom->setUpdateInterval(20);
    connect(animZoom, SIGNAL(valueChanged(qreal)), SLOT(scalingTime(qreal)));
    connect(animZoom, SIGNAL(finished()), SLOT(scalingFinished()));

    // Draw map
    // better GUI -- more map tiles
    for (int k = 0; k < MAP_LAYERS + SHADOW_LAYERS + SHADOW_EXTRALAYERS; k++)
    {
        GameMapLayer *layer = new GameMapLayer(k, grobjs);
        layer->setZValue(k * 1e8);
        scene->addItem(layer);
    }

    crown = scene->addPixmap(grobjs->crown_sprite);
    crown->hide();
    crown->setOffset(CROWN_START_X * TILE_SIZE, CROWN_START_Y * TILE_SIZE);
    crown->setZValue(0.3);
}

GameMapView::~GameMapView ()
{
  BOOST_FOREACH (QGraphicsRectItem* b, banks)
    {
      scene->removeItem (b);
      delete b;
    }

  delete gameMapCache;
  delete grobjs;
}

struct CharacterEntry
{
    QString name;
    unsigned char color;

    // better GUI -- icons
    int icon_d1;
    int icon_d2;

    const CharacterState *state;
};


// pending tx monitor -- helper
static int wmon_CoordStep(int x, int target)
{
    if (x < target)
        return x + 1;
    else if (x > target)
        return x - 1;
    else
        return x;
}
// Compute new 'v' coordinate using line slope information applied to the 'u' coordinate
// 'u' is reference coordinate (largest among dx, dy), 'v' is the coordinate to be updated
static int wmon_CoordUpd(int u, int v, int du, int dv, int from_u, int from_v)
{
    if (dv != 0)
    {
        int tmp = (u - from_u) * dv;
        int res = (abs(tmp) + abs(du) / 2) / du;
        if (tmp < 0)
            res = -res;
        return res + from_v;
    }
    else
        return v;
}


void GameMapView::updateGameMap(const GameState &gameState)
{
    if (playerPath)
    {
        scene->removeItem(playerPath);
        delete playerPath;
        playerPath = NULL;
    }
    if (queuedPlayerPath)
    {
        scene->removeItem(queuedPlayerPath);
        delete queuedPlayerPath;
        queuedPlayerPath = NULL;
    }


    /* Update the banks.  */
    // better GUI -- banks
    const int bankOpacity = 10;
    int bank_xpos[75];
    int bank_ypos[75];
    int bank_timeleft[75];
    int bank_idx = 0;


    BOOST_FOREACH (QGraphicsRectItem* b, banks)
      {
        scene->removeItem (b);
        delete b;
      }
    banks.clear ();
    BOOST_FOREACH (const PAIRTYPE(Coord, unsigned)& b, gameState.banks)
      {
        QGraphicsRectItem* r
          = scene->addRect (TILE_SIZE * b.first.x, TILE_SIZE * b.first.y,
                            TILE_SIZE, TILE_SIZE,
                            Qt::NoPen, QColor (255, 255, 255, bankOpacity));


        // better GUI -- banks
        if (bank_idx < 75)
        {
            bank_xpos[bank_idx] = b.first.x;
            bank_ypos[bank_idx] = b.first.y;
            bank_timeleft[bank_idx] = b.second;
            bank_idx++;
        }


        banks.push_back (r);
      }

    gameMapCache->StartCachedScene();
    BOOST_FOREACH(const PAIRTYPE(Coord, LootInfo) &loot, gameState.loot)
        gameMapCache->PlaceCoin(loot.first, loot.second.nAmount);
    BOOST_FOREACH(const Coord &h, gameState.hearts)
        gameMapCache->PlaceHeart(h);


    // pending tx monitor -- reset name list
    pmon_all_count = 0;
    for (int m = 0; m < PMON_MY_MAX; m++)
    {
        pmon_my_idx[m] = -1;
    }


    // Sort by coordinate bottom-up, so the stacking (multiple players on tile) looks correct
    std::multimap<Coord, CharacterEntry> sortedPlayers;

    for (std::map<PlayerID, PlayerState>::const_iterator mi = gameState.players.begin(); mi != gameState.players.end(); mi++)
    {
        const PlayerState &pl = mi->second;
        for (std::map<int, CharacterState>::const_iterator mi2 = pl.characters.begin(); mi2 != pl.characters.end(); mi2++)
        {
            const CharacterState &characterState = mi2->second;
            const Coord &coord = characterState.coord;
            CharacterEntry entry;
            CharacterID chid(mi->first, mi2->first);
            entry.name = QString::fromStdString(chid.ToString());
            if (mi2->first == 0) // Main character ("the general") has a star before the name
                entry.name = QString::fromUtf8("\u2605") + entry.name;
            if (chid == gameState.crownHolder)
                entry.name += QString::fromUtf8(" \u265B");


            // pending tx monitor -- info text
            const Coord &wmon_from = characterState.from;
            if (((pmon_state == PMONSTATE_CONSOLE) || (pmon_state == PMONSTATE_RUN)) &&
                (pmon_all_count < PMON_ALL_MAX))
            {
                pmon_all_names[pmon_all_count] = chid.ToString();
                pmon_all_x[pmon_all_count] = coord.x;
                pmon_all_y[pmon_all_count] = coord.y;
                pmon_all_color[pmon_all_count] = pl.color;

                entry.icon_d1 = 0;
                entry.icon_d2 = 0;
                if (pl.value > 40000000000)
                    entry.icon_d2 = RGP_ICON_HUC_BANDIT;
                if (pl.value > 20000000000)
                    entry.icon_d1 = RGP_ICON_HUC_BANDIT;

                entry.name += QString::fromStdString(" ");
                entry.name += QString::number(coord.x);
                entry.name += QString::fromStdString(",");
                entry.name += QString::number(coord.y);

                // pending waypoints/destruct
                int pending_tx_idx = -1;
                int wp_age = 0;

                for (int k = 0; k < pmon_tx_count; k++)
                {
                    if (chid.ToString() == pmon_tx_names[k])
                    {
                        pending_tx_idx = k;
                        wp_age = pmon_tx_age[k];

                        break;
                    }
                }

                bool will_move = false;
                Coord target;

                if (!(characterState.waypoints.empty()))
                {
                    Coord new_c;
                    target = characterState.waypoints.back();


                    int dx = target.x - wmon_from.x;
                    int dy = target.y - wmon_from.y;

                    if (abs(dx) > abs(dy))
                    {
                        new_c.x = wmon_CoordStep(coord.x, target.x);
                        new_c.y = wmon_CoordUpd(new_c.x, coord.y, dx, dy, wmon_from.x, wmon_from.y);
                    }
                    else
                    {
                        new_c.y = wmon_CoordStep(coord.y, target.y);
                        new_c.x = wmon_CoordUpd(new_c.y, coord.x, dy, dx, wmon_from.y, wmon_from.x);
                    }
                    pmon_all_next_x[pmon_all_count] = new_c.x;
                    pmon_all_next_y[pmon_all_count] = new_c.y;
                    will_move = true;
                }
                else
                {
                    pmon_all_next_x[pmon_all_count] = coord.x;
                    pmon_all_next_y[pmon_all_count] = coord.y;
                }

                if (will_move)
                {
                    entry.name += QString::fromStdString("->");
                    entry.name += QString::number(pmon_all_next_x[pmon_all_count]);
                    entry.name += QString::fromStdString(",");
                    entry.name += QString::number(pmon_all_next_y[pmon_all_count]);
                }

                if (!(characterState.waypoints.empty()))
                {
                    entry.name += QString::fromStdString(" wp:");
                    entry.name += QString::number(target.x);
                    entry.name += QString::fromStdString(",");
                    entry.name += QString::number(target.y);
                }

                // is this one of my players?
                pmon_all_cache_isinmylist[pmon_all_count] = false;
                for (int m = 0; m < PMON_MY_MAX; m++)
                {
                    if (chid.ToString() == pmon_my_names[m])
                    {
                        pmon_all_cache_isinmylist[pmon_all_count] = true;
                        pmon_my_idx[m] = pmon_all_count;

                        // if one of my hunters has alarm triggered
                        bool tmp_alarm = false;
                        for (int m2 = 0; m2 < PMON_MY_MAX; m2++)
                        {
                            if (pmon_my_alarm_state[m2])
                            {
                                if (!tmp_alarm)
                                {
                                    tmp_alarm = true;
                                    entry.name += QString::fromStdString(" *ALARM*:");
                                }
                                else
                                {
                                    entry.name += QString::fromStdString(",");
                                }
                                entry.name += QString::fromStdString(pmon_my_names[m2]);
                            }
                        }

                        // check for pending tx (to determine idle status)
                        bool tmp_has_pending_tx = false;
                        bool tmp_is_banking = gameState.IsBank(coord);
                        if ((characterState.waypoints.empty()) || (pmon_out_of_wp_idx == m))
                        {
                            for (int k2 = 0; k2 < pmon_tx_count; k2++)
                            {
                                if (pmon_tx_names[k2] == pmon_my_names[m])
                                {
                                    tmp_has_pending_tx = true;
                                    break;
                                }
                            }
                        }

                        // if this hunter is idle, or "longest idle"
                        if ((characterState.waypoints.empty()) && (!tmp_has_pending_tx) && (!tmp_is_banking))
                        {
                            pmon_my_idlecount[m]++;

                            if (pmon_out_of_wp_idx <= -1)
                                pmon_out_of_wp_idx = m;
                            else if ((pmon_out_of_wp_idx < PMON_MY_MAX) && (pmon_my_idlecount[m] > pmon_my_idlecount[pmon_out_of_wp_idx]))
                                pmon_out_of_wp_idx = m;
                        }
                        else
                        {
                            pmon_my_idlecount[m] = 0;

                            if (pmon_out_of_wp_idx == m)
                                pmon_out_of_wp_idx = -1;
                        }

                        // longest idle time in minutes
                        if (pmon_out_of_wp_idx >= 0)
                        {
#ifdef WIN32
                            // note: "Segoe UI Symbol" has u231B and many other unicode characters on windows7 (and all newer versions?)
                            entry.name += QString::fromUtf8(" \u2603"); // snowman
#else
                            entry.name += QString::fromUtf8(" \u231B"); // hourglass
#endif
                            if (pmon_out_of_wp_idx < PMON_MY_MAX)
                            {
                                entry.name += QString::number(pmon_my_idlecount[pmon_out_of_wp_idx] * pmon_go / 60);
                                entry.name += QString::fromStdString("min:");
                                entry.name += QString::fromStdString(pmon_my_names[pmon_out_of_wp_idx]);
                            }
                        }
                        else if (!tmp_alarm)
                        {
                            entry.name += QString::fromStdString(" (OK)");
                        }

                        if (!pmon_noisy)
                            entry.name += QString::fromStdString(" (silent)");

                        if (pmon_my_foecontact_age[m])
                        {
                            entry.name += QString::fromStdString(" CONTACT*");
                            entry.name += QString::number(pmon_my_foecontact_age[m]);
                        }
                    }
                }

                // value of pending tx
                if (pending_tx_idx >= 0)
                {
                    entry.name += QString::fromStdString(" tx*");
                    entry.name += QString::number(wp_age);

                    entry.name += QString::fromStdString(" ");
                    entry.name += QString::fromStdString(pmon_tx_values[pending_tx_idx]);
                }

                pmon_all_count++;
            }


            entry.color = pl.color;
            entry.state = &characterState;
            sortedPlayers.insert(std::make_pair(Coord(-coord.x, -coord.y), entry));
        }
    }


    // pending tx monitor --  if enemy is nearby
    if (true)
    {
      for (int m = 0; m < PMON_MY_MAX; m++)
      {
        if (pmon_state == PMONSTATE_SHUTDOWN)
        {
            pmon_my_foecontact_age[m] = 0;
            pmon_my_alarm_state[m] = 0;
            pmon_my_idlecount[m] = 0;

            continue;
        }

        bool tmp_trigger_alarm = false;
        bool enemy_in_range = false;
        int my_alarm_range = pmon_my_alarm_dist[m];
        if (pmon_my_alarm_state[m])
            my_alarm_range++;

        int my_idx = pmon_my_idx[m];
        if (my_idx < 0)  // not alive
        {
            pmon_my_alarm_state[m] = 0;
            pmon_my_idlecount[m] = 0;

            if (pmon_out_of_wp_idx == m) pmon_out_of_wp_idx = -1;

            continue;
        }

        int my_next_x = pmon_all_next_x[my_idx];
        int my_next_y = pmon_all_next_y[my_idx];

        int my_x = pmon_all_x[my_idx];
        int my_y = pmon_all_y[my_idx];

        for (int k_all = 0; k_all < pmon_all_count; k_all++)
        {
            if (k_all == my_idx) continue; // that's me
            if (pmon_all_color[my_idx] == pmon_all_color[k_all]) continue; // same team
            if (pmon_all_cache_isinmylist[k_all]) continue; // one of my players

            if ((abs(my_next_x - pmon_all_next_x[k_all]) <= 1) && (abs(my_next_y - pmon_all_next_y[k_all]) <= 1))
                enemy_in_range = true;

            if ((my_alarm_range) && (abs(my_x - pmon_all_x[k_all]) <= my_alarm_range) && (abs(my_y - pmon_all_y[k_all]) <= my_alarm_range))
            {
                tmp_trigger_alarm = true;
            }
        }

        if (tmp_trigger_alarm)
        {
            if (!pmon_my_alarm_state[m]) pmon_my_alarm_state[m] = 1;
        }
        else
        {
            pmon_my_alarm_state[m] = 0;
        }

        if (enemy_in_range) pmon_my_foecontact_age[m]++;
        else pmon_my_foecontact_age[m] = 0;

      }
    }
    if (pmon_state == PMONSTATE_SHUTDOWN)
        pmon_state = PMONSTATE_STOPPED;


    Coord prev_coord;
    int offs = -1;
    BOOST_FOREACH(const PAIRTYPE(Coord, CharacterEntry) &data, sortedPlayers)
    {
        const QString &playerName = data.second.name;
        const CharacterState &characterState = *data.second.state;
        const Coord &coord = characterState.coord;

        if (offs >= 0 && coord == prev_coord)
            offs++;
        else
        {
            prev_coord = coord;
            offs = 0;
        }

        int x = coord.x * TILE_SIZE + offs;
        int y = coord.y * TILE_SIZE + offs * 2;


        // better GUI -- better player sprites
        int color_attack1 = RPG_ICON_EMPTY;
        int color_defense1 = data.second.icon_d1 ==  RGP_ICON_HUC_BANDIT ? RGP_ICON_HUC_BANDIT : RPG_ICON_EMPTY;
        int color_defense2 = data.second.icon_d2 ==  RGP_ICON_HUC_BANDIT ? RGP_ICON_HUC_BANDIT : RPG_ICON_EMPTY;
        gameMapCache->AddPlayer(playerName, x, y, 1 + offs, data.second.color, color_attack1, color_defense1, color_defense2, characterState.dir, characterState.loot.nAmount);
    }


    // better GUI -- banks
    // note: players need unique names
    for (int m = 0; m < bank_idx; m++)
    {
        QString tmp_name = QString::number(m);
        tmp_name += QString::fromStdString(":");
        tmp_name += QString::number(bank_timeleft[m]);
        tmp_name += QString::fromStdString(" ");
        for (int tl = 0; tl < bank_timeleft[m]; tl++)
            tmp_name += QString::fromStdString("|");

        gameMapCache->AddPlayer(tmp_name, TILE_SIZE * bank_xpos[m], TILE_SIZE * bank_ypos[m], 1 + 0, 4, RPG_ICON_EMPTY, RPG_ICON_EMPTY, RPG_ICON_EMPTY, 3, 0);
    }


    gameMapCache->EndCachedScene();

    if (!gameState.crownHolder.player.empty())
        crown->hide();
    else
    {
        crown->show();
        crown->setOffset(gameState.crownPos.x * TILE_SIZE, gameState.crownPos.y * TILE_SIZE);
    }

    //scene->invalidate();
    repaint(rect());
}

static void DrawPath(const std::vector<Coord> &coords, QPainterPath &path)
{
    if (coords.empty())
        return;

    for (int i = 0; i < coords.size(); i++)
    {
        QPointF p((coords[i].x + 0.5) * TILE_SIZE, (coords[i].y + 0.5) * TILE_SIZE);
        if (i == 0)
            path.moveTo(p);
        else
            path.lineTo(p);
    }
}

void GameMapView::SelectPlayer(const QString &name, const GameState &state, QueuedMoves &queuedMoves)
{
    // Clear old path
    DeselectPlayer();

    if (name.isEmpty())
        return;

    QPainterPath path, queuedPath;

    std::map<PlayerID, PlayerState>::const_iterator mi = state.players.find(name.toStdString());
    if (mi == state.players.end())
        return;

    BOOST_FOREACH(const PAIRTYPE(int, CharacterState) &pc, mi->second.characters)
    {
        int i = pc.first;
        const CharacterState &ch = pc.second;

        DrawPath(ch.DumpPath(), path);

        std::vector<Coord> *p = UpdateQueuedPath(ch, queuedMoves, CharacterID(mi->first, i));
        if (p)
        {
            std::vector<Coord> wp = PathToCharacterWaypoints(*p);
            DrawPath(ch.DumpPath(&wp), queuedPath);
        }
    }
    if (!path.isEmpty())
    {
        playerPath = scene->addPath(path, grobjs->magenta_pen);
        playerPath->setZValue(1e9 + 1);
    }
    if (!queuedPath.isEmpty())
    {
        queuedPlayerPath = scene->addPath(queuedPath, grobjs->gray_pen);
        queuedPlayerPath->setZValue(1e9 + 2);
    }

    use_cross_cursor = true;
    if (!panning)
        setCursor(Qt::CrossCursor);
}

void GameMapView::CenterMapOnCharacter(const Game::CharacterState &state)
{
    centerOn((state.coord.x + 0.5) * TILE_SIZE, (state.coord.y + 0.5) * TILE_SIZE);
}

void GameMapView::DeselectPlayer()
{
    if (playerPath || queuedPlayerPath)
    {
        if (playerPath)
        {
            scene->removeItem(playerPath);
            delete playerPath;
            playerPath = NULL;
        }
        if (queuedPlayerPath)
        {
            scene->removeItem(queuedPlayerPath);
            delete queuedPlayerPath;
            queuedPlayerPath = NULL;
        }
        //scene->invalidate();
        repaint(rect());
    }
    use_cross_cursor = false;
    if (!panning)
        setCursor(Qt::ArrowCursor);
}

const static double MIN_ZOOM = 0.1;
const static double MAX_ZOOM = 2.0;

void GameMapView::mousePressEvent(QMouseEvent *event)
{   
    if (event->button() == Qt::LeftButton)
    {
        QPoint p = mapToScene(event->pos()).toPoint();
        int x = p.x() / TILE_SIZE;
        int y = p.y() / TILE_SIZE;
        if (IsInsideMap(x, y))
            emit tileClicked(x, y, event->modifiers().testFlag( Qt::ControlModifier ));
    }
    else if (event->button() == Qt::RightButton)
    {
        panning = true;
        setCursor(Qt::ClosedHandCursor);
        pan_pos = event->pos();
    }
    else if (event->button() == Qt::MiddleButton)
    {
        // pending tx monitor -- middle mouse button
        if (event->modifiers().testFlag( Qt::ControlModifier ))
        {
            pmon_noisy = pmon_noisy ? false : true;
        }
        else if ( ! (event->modifiers().testFlag( Qt::ShiftModifier )) )
        {
            if (pmon_state == PMONSTATE_STOPPED)
                pmon_state = PMONSTATE_START;
            else
                pmon_state = PMONSTATE_SHUTDOWN;
        }
        else
        {
            QPoint p = mapToScene(event->pos()).toPoint();
            zoomReset();
            centerOn(p);
        }
    }
    event->accept();
}

void GameMapView::mouseReleaseEvent(QMouseEvent *event)
{   
    if (event->button() == Qt::RightButton)
    {
        panning = false;
        setCursor(use_cross_cursor ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    event->accept();
}

void GameMapView::mouseMoveEvent(QMouseEvent *event)
{
    if (panning)
    {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + pan_pos.x() - event->pos().x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() + pan_pos.y() - event->pos().y());
        pan_pos = event->pos();
    }
    event->accept();
}

void GameMapView::wheelEvent(QWheelEvent *event)
{
    double delta = event->delta() / 120.0;

    // If user moved the wheel in another direction, we reset previously scheduled scalings
    if ((scheduledZoom > zoomFactor && delta < 0) || (scheduledZoom < zoomFactor && delta > 0))
        scheduledZoom = zoomFactor;

    scheduledZoom *= std::pow(1.2, delta);
    oldZoom = zoomFactor;

    animZoom->stop();
    if (scheduledZoom != zoomFactor)
        animZoom->start();

    event->accept();
}

void GameMapView::zoomIn()
{
    if (scheduledZoom < zoomFactor)
        scheduledZoom = zoomFactor;
    scheduledZoom *= 1.2;
    oldZoom = zoomFactor;

    animZoom->stop();
    if (scheduledZoom != zoomFactor)
        animZoom->start();
}

void GameMapView::zoomOut()
{
    if (scheduledZoom > zoomFactor)
        scheduledZoom = zoomFactor;
    scheduledZoom /= 1.2;
    oldZoom = zoomFactor;

    animZoom->stop();
    if (scheduledZoom != zoomFactor)
        animZoom->start();
}

void GameMapView::zoomReset()
{
    animZoom->stop();
    oldZoom = zoomFactor = scheduledZoom = 1.0;

    resetTransform();
    setRenderHints(defaultRenderHints);
}

void GameMapView::scalingTime(qreal t)
{
    if (t > 0.999)
        zoomFactor = scheduledZoom;
    else
        zoomFactor = oldZoom * (1.0 - t) + scheduledZoom * t;
        //zoomFactor = std::exp(std::log(oldZoom) * (1.0 - t) + std::log(scheduledZoom) * t);

    if (zoomFactor > MAX_ZOOM)
        zoomFactor = MAX_ZOOM;
    else if (zoomFactor < MIN_ZOOM)
        zoomFactor = MIN_ZOOM;

    resetTransform();
    scale(zoomFactor, zoomFactor);

    if (zoomFactor < 0.999)
        setRenderHints(defaultRenderHints | QPainter::SmoothPixmapTransform);
    else
        setRenderHints(defaultRenderHints);
}

void GameMapView::scalingFinished()
{
    // This may be redundant, if QTimeLine ensures that last frame is always procesed
    scalingTime(1.0);
}
