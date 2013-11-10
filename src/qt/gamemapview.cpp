#include "gamemapview.h"

#include "../gamestate.h"
#include "../gamemap.h"
#include "../util.h"

#include <QImage>
#include <QGraphicsItem>
#include <QGraphicsPixmapItem>
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

    QPixmap coin_sprite;
    QPixmap tiles[NUM_TILE_IDS];

    QBrush player_text_brush[Game::NUM_TEAM_COLORS];

    QPen magenta_pen;

    GameGraphicsObjects()
        : magenta_pen(Qt::magenta, 2.0)
    {
        player_text_brush[0] = QBrush(QColor(255, 255, 100));
        player_text_brush[1] = QBrush(QColor(255, 80, 80));
        player_text_brush[2] = QBrush(QColor(100, 255, 100));
        player_text_brush[3] = QBrush(QColor(130, 150, 255));

        for (int i = 0; i < Game::NUM_TEAM_COLORS; i++)
            for (int j = 1; j < 10; j++)
            {
                if (j != 5)
                    player_sprite[i][j].load(":/gamemap/sprites/" + QString::number(i) + "_" + QString::number(j));
            }

        coin_sprite.load(":/gamemap/sprites/coin");

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
    std::map<PlayerID, CachedPlayer> cached_players;

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
        for (std::map<PlayerID, CachedPlayer>::iterator mi = cached_players.begin(); mi != cached_players.end(); ++mi)
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
    
    void AddPlayer(const std::string &name, int x, int y, int z_order, int color, int dir, int64 nAmount)
    {
        CachedPlayer &p = cached_players[name];
        if (!p)
            p.Create(scene, grobjs, x, y, z_order, QString::fromStdString(name), color, dir, nAmount);
        else
            p.Update(x, y, z_order, color, dir, nAmount);
    }

    void EndCachedScene()
    {
        // Erase unreferenced objects
        for (std::map<Coord, CachedCoin>::iterator mi = cached_coins.begin(); mi != cached_coins.end(); )
        {
            if (mi->second.referenced)
                ++mi;
            else
            {
                mi->second.Destroy(scene);
                cached_coins.erase(mi++);
            }
        }
        for (std::map<PlayerID, CachedPlayer>::iterator mi = cached_players.begin(); mi != cached_players.end(); )
        {
            if (mi->second.referenced)
                ++mi;
            else
            {
                mi->second.Destroy(scene);
                cached_players.erase(mi++);
            }
        }
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
    : QGraphicsView(parent), zoomFactor(1.0), panning(false), use_cross_cursor(false), scheduledZoom(1.0),
    grobjs(new GameGraphicsObjects), playerPath(NULL)
{
    scene = new QGraphicsScene(this);

    scene->setItemIndexMethod(QGraphicsScene::BspTreeIndex);
    scene->setBspTreeDepth(15);

    setScene(scene);

    gameMapCache = new GameMapCache(scene, grobjs);

    setOptimizationFlags(QGraphicsView::DontSavePainterState);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);

    setBackgroundBrush(QColor(128, 128, 128));

    defaultRenderHints = renderHints();
    
    setStatusTip(tr("Left click - make move. Right button - scroll map. Mouse wheel - zoom map. Middle click - reset zoom."));

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

    // Draw spawn areas
    const int spawn_opacity = 40;

    QPen no_pen(Qt::NoPen);

    // Yellow (top-left)
    scene->addRect(0, 0,
        SPAWN_AREA_LENGTH * TILE_SIZE, TILE_SIZE,
        no_pen, QColor(255, 255, 0, spawn_opacity));
    scene->addRect(0, TILE_SIZE,
        TILE_SIZE, (SPAWN_AREA_LENGTH - 1) * TILE_SIZE,
        no_pen, QColor(255, 255, 0, spawn_opacity));
    // Red (top-right)
    scene->addRect((MAP_WIDTH - SPAWN_AREA_LENGTH) * TILE_SIZE, 0,
        SPAWN_AREA_LENGTH * TILE_SIZE, TILE_SIZE,
        no_pen, QColor(255, 0, 0, spawn_opacity));
    scene->addRect((MAP_WIDTH - 1) * TILE_SIZE, TILE_SIZE,
        TILE_SIZE, (SPAWN_AREA_LENGTH - 1) * TILE_SIZE,
        no_pen, QColor(255, 0, 0, spawn_opacity));
    // Green (bottom-right)
    scene->addRect((MAP_WIDTH - SPAWN_AREA_LENGTH) * TILE_SIZE, (MAP_HEIGHT - 1) * TILE_SIZE,
        SPAWN_AREA_LENGTH * TILE_SIZE, TILE_SIZE,
        no_pen, QColor(0, 255, 0, spawn_opacity));
    scene->addRect((MAP_WIDTH - 1) * TILE_SIZE, (MAP_HEIGHT - SPAWN_AREA_LENGTH) * TILE_SIZE,
        TILE_SIZE, (SPAWN_AREA_LENGTH - 1) * TILE_SIZE,
        no_pen, QColor(0, 255, 0, spawn_opacity));
    // Blue (bottom-left)
    scene->addRect(0, (MAP_HEIGHT - 1) * TILE_SIZE,
        SPAWN_AREA_LENGTH * TILE_SIZE, TILE_SIZE,
        no_pen, QColor(0, 0, 255, spawn_opacity));
    scene->addRect(0, (MAP_HEIGHT - SPAWN_AREA_LENGTH) * TILE_SIZE,
        TILE_SIZE, (SPAWN_AREA_LENGTH - 1) * TILE_SIZE,
        no_pen, QColor(0, 0, 255, spawn_opacity));
}

GameMapView::~GameMapView()
{
    delete gameMapCache;
    delete grobjs;
}

void GameMapView::updateGameMap(const GameState &gameState)
{
    if (playerPath)
    {
        scene->removeItem(playerPath);
        delete playerPath;
        playerPath = NULL;
    }

    gameMapCache->StartCachedScene();
    BOOST_FOREACH(const PAIRTYPE(Coord, LootInfo) &loot, gameState.loot)
        gameMapCache->PlaceCoin(loot.first, loot.second.nAmount);

    // Sort by coordinate bottom-up, so the stacking (multiple players on tile) looks correct
    typedef const std::pair<const PlayerID, PlayerState> *PlayerEntryPtr;
    std::multimap<Coord, PlayerEntryPtr> sortedPlayers;

    for (std::map<PlayerID, PlayerState>::const_iterator mi = gameState.players.begin(); mi != gameState.players.end(); mi++)
    {
        const Coord &coord = mi->second.coord;
        sortedPlayers.insert(std::make_pair(Coord(-coord.x, -coord.y), &(*mi)));
    }

    Coord prev_coord;
    int offs = -1;
    BOOST_FOREACH(const PAIRTYPE(Coord, PlayerEntryPtr) &data, sortedPlayers)
    {
        const PlayerID &playerName = data.second->first;
        const PlayerState &playerState = data.second->second;
        const Coord &coord = playerState.coord;

        if (offs >= 0 && coord == prev_coord)
            offs++;
        else
        {
            prev_coord = coord;
            offs = 0;
        }

        int x = coord.x * TILE_SIZE + offs;
        int y = coord.y * TILE_SIZE + offs * 2;

        gameMapCache->AddPlayer(playerName, x, y, 1 + offs, playerState.color, playerState.dir, playerState.loot.nAmount);
    }
    gameMapCache->EndCachedScene();

    //scene->invalidate();
    repaint(rect());
}

void GameMapView::SelectPlayers(const QStringList &names, const GameState &state, const GamePathfinders &pathfinders)
{
    // Clear old path
    DeselectPlayers();
    
    if (names.isEmpty())
        return;

    QPainterPath path;

    foreach (QString name, names)
    {
        std::string pl = name.toStdString();
        std::map<Game::PlayerID, Game::PlayerState>::const_iterator mi = state.players.find(pl);
        if (mi == state.players.end())
            continue;

        // Path till next waypoint
        std::vector<Coord> coords = mi->second.DumpPath();

        // Remaining waypoints
        GamePathfinders::const_iterator pf = pathfinders.find(pl);
        if (pf != pathfinders.end() &&
                // Make sure the path continues from current position or current waypoint
                // (otherwise it's still pending or was broken by a concurrent transaction)
                pf->second.GetCurWaypoint() == (coords.empty() ? mi->second.coord : coords.back())
            )
        {
            if (coords.empty())
                coords.push_back(mi->second.coord);
            std::vector<Coord> coords2 = pf->second.DumpPath();
            coords.insert(coords.end(), coords2.begin(), coords2.end());
        }

        if (!coords.empty())
        {
            for (int i = 0; i < coords.size(); i++)
            {
                QPointF p((coords[i].x + 0.5) * TILE_SIZE, (coords[i].y + 0.5) * TILE_SIZE);
                if (i == 0)
                    path.moveTo(p);
                else
                    path.lineTo(p);
            }
        }
    }
    if (!path.isEmpty())
    {
        playerPath = scene->addPath(path, grobjs->magenta_pen);
        playerPath->setZValue(0.5);
    }

    use_cross_cursor = true;
    if (!panning)
        setCursor(Qt::CrossCursor);
}

void GameMapView::CenterMapOnPlayer(const Game::PlayerState &state)
{
    centerOn((state.coord.x + 0.5) * TILE_SIZE, (state.coord.y + 0.5) * TILE_SIZE);
}

void GameMapView::DeselectPlayers()
{
    if (playerPath)
    {
        scene->removeItem(playerPath);
        delete playerPath;
        playerPath = NULL;
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
            emit tileClicked(x, y);
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

        animZoom->stop();
        oldZoom = zoomFactor = scheduledZoom = 1.0;

        resetTransform();
        setRenderHints(defaultRenderHints);
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
