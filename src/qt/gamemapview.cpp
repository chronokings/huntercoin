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
    QPixmap player_sprite[Game::NUM_TEAM_COLORS][10];

    QPixmap coin_sprite, heart_sprite, crown_sprite;
    QPixmap tiles[NUM_TILE_IDS];

    QBrush player_text_brush[Game::NUM_TEAM_COLORS];

    QPen magenta_pen, gray_pen;

    GameGraphicsObjects()
        : magenta_pen(Qt::magenta, 2.0),
        gray_pen(QColor(170, 170, 170), 2.0)
    {
        player_text_brush[0] = QBrush(QColor(255, 255, 100));
        player_text_brush[1] = QBrush(QColor(255, 80, 80));
        player_text_brush[2] = QBrush(QColor(100, 255, 100));
        player_text_brush[3] = QBrush(QColor(0, 170, 255));

        for (int i = 0; i < Game::NUM_TEAM_COLORS; i++)
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

// for FORK_TIMESAVE -- visualize player spawns
QPen visualize_spawn_pen(Qt::NoPen);
bool visualize_spawn_done = false;
int visualize_nHeight;
int visualize_x;
int visualize_y;
#define VISUALIZE_TIMESAVE_IN_EFFECT(H) (((fTestNet)&&(H>331500))||((!fTestNet)&&(H>1521500)))

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

            // ghosting with phasing-in
            if (VISUALIZE_TIMESAVE_IN_EFFECT (visualize_nHeight))
            {
                if ((((visualize_x % 2) + (visualize_y % 2) > 1) && (visualize_nHeight % 500 >= 300)) ||  // for 150 blocks, every 4th coin spawn is ghosted
                    (((visualize_x % 2) + (visualize_y % 2) > 0) && (visualize_nHeight % 500 >= 450)) ||  // for 30 blocks, 3 out of 4 coin spawns are ghosted
                    (visualize_nHeight % 500 >= 480))                                             // for 20 blocks, full ghosting
                    coin->setOpacity(0.4);
                else
                    coin->setOpacity(1.0);
            }
            else
            {
                 coin->setOpacity(1.0);
            }

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
            // ghosting with phasing-in
            if (VISUALIZE_TIMESAVE_IN_EFFECT (visualize_nHeight))
            {
                if ((((visualize_x % 2) + (visualize_y % 2) > 1) && (visualize_nHeight % 500 >= 300)) ||  // for 150 blocks, every 4th coin spawn is ghosted
                    (((visualize_x % 2) + (visualize_y % 2) > 0) && (visualize_nHeight % 500 >= 450)) ||  // for 30 blocks, 3 out of 4 coin spawns are ghosted
                    (visualize_nHeight % 500 >= 480))                                             // for 20 blocks, full ghosting
                    coin->setOpacity(0.4);
                else
                    coin->setOpacity(1.0);
            }
            else
            {
                 coin->setOpacity(1.0);
            }

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
        QGraphicsSimpleTextItem *text;
        const GameGraphicsObjects *grobjs;
        QString name;
        int x, y, color, dir;
        int z_order;
        int64 nLootAmount;

        void UpdPos()
        {
            sprite->setOffset(x, y);
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

        CachedPlayer() : sprite(NULL) { }

        void Create(QGraphicsScene *scene, const GameGraphicsObjects *grobjs_, int x_, int y_, int z_order_, QString name_, int color_, int dir_, int64 amount)
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
            text = scene->addSimpleText("");
            text->setZValue(1e9);
            UpdPos();
            UpdText();
            UpdColor();
        }

        void Update(int x_, int y_, int z_order_, int color_, int dir_, int64 amount)
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
        }

        operator bool() const { return sprite != NULL; }

        void Destroy(QGraphicsScene *scene)
        {
            scene->removeItem(sprite);
            scene->removeItem(text);
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

        // for FORK_TIMESAVE
        visualize_x = coord.x;
        visualize_y = coord.y;

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

    void AddPlayer(const QString &name, int x, int y, int z_order, int color, int dir, int64 nAmount)
    {
        CachedPlayer &p = cached_players[name];
        if (!p)
            p.Create(scene, grobjs, x, y, z_order, name, color, dir, nAmount);
        else
            p.Update(x, y, z_order, color, dir, nAmount);
    }

    // Erase unreferenced objects from cache
    void EndCachedScene()
    {
        EraseUnreferenced(cached_coins);
        EraseUnreferenced(cached_hearts);
        EraseUnreferenced(cached_players);
    }
};

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
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++)
            {
                int tile = GameMap[layer][y][x];
                // Tile 0 denotes grass in layer 0 and empty cell in other layers
                if (!tile && layer)
                    continue;
                painter->drawPixmap(x * TILE_SIZE, y * TILE_SIZE, grobjs->tiles[tile]);
            }
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
    for (int k = 0; k < MAP_LAYERS; k++)
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
    const CharacterState *state;
};

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

    // for FORK_TIMESAVE -- visualize player spawns
    // note: Formerly, the SpawnMap was calculated in init.cpp, after graphics initialization.
    //       We could now move player spawn visualization code to GameMapView::GameMapView, but it would make the system less flexible.
    visualize_nHeight = gameState.nHeight;
    if (!visualize_spawn_done)
    {
        for (int y = 0; y < MAP_HEIGHT; y++)
            for (int x = 0; x < MAP_WIDTH; x++)
            {
                if (SpawnMap[y][x] & SPAWNMAPFLAG_PLAYER)
                {
                    scene->addRect(x * TILE_SIZE, y * TILE_SIZE,
                        TILE_SIZE, TILE_SIZE,
                        visualize_spawn_pen, QColor(255, 255, 0, 40));

                    if (!visualize_spawn_done) visualize_spawn_done = true;
                }
            }
    }


    /* Update the banks.  */
    const int bankOpacity = 40;
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
        banks.push_back (r);
      }

    gameMapCache->StartCachedScene();
    BOOST_FOREACH(const PAIRTYPE(Coord, LootInfo) &loot, gameState.loot)
        gameMapCache->PlaceCoin(loot.first, loot.second.nAmount);
    BOOST_FOREACH(const Coord &h, gameState.hearts)
        gameMapCache->PlaceHeart(h);

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

            // for FORK_TIMESAVE -- show protected/spectator state
            if (characterState.stay_in_spawn_area != CHARACTER_MODE_NORMAL)
            {
                entry.name += QString::fromStdString(" (");
                entry.name += QString::number(characterState.stay_in_spawn_area);
                if (VISUALIZE_TIMESAVE_IN_EFFECT(visualize_nHeight))
                {
                    if (CharacterInSpectatorMode(characterState.stay_in_spawn_area))
                        entry.name += QString::fromStdString(", spectator)");
                    else if (CharacterIsProtected(characterState.stay_in_spawn_area))
                        entry.name += QString::fromStdString(", protected)");
                    else
                        entry.name += QString::fromStdString(")");
                }
                else
                    entry.name += QString::fromStdString(")");
            }

            entry.color = pl.color;
            entry.state = &characterState;
            sortedPlayers.insert(std::make_pair(Coord(-coord.x, -coord.y), entry));
        }
    }

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

        gameMapCache->AddPlayer(playerName, x, y, 1 + offs, data.second.color, characterState.dir, characterState.loot.nAmount);
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
        QPoint p = mapToScene(event->pos()).toPoint();

        zoomReset();
        centerOn(p);
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
