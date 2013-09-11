#include "gamemapview.h"

#include "../gamestate.h"
#include "../util.h"

#include <QImage>
#include <QGraphicsPixmapItem>
#include <QGraphicsSimpleTextItem>

#include <boost/foreach.hpp>

using namespace Game;

static const int TILE_SIZE = 48;

// Cache coin sprites to avoid recreating them on each state update
// TODO: when there are too many players on the map, we need to cache them too
class GameMapCache
{
    struct CachedCoin
    {
        QGraphicsEllipseItem *coin;
        QGraphicsTextItem *text;
        int64 nAmount;
        bool referenced;

        CachedCoin() : coin(NULL) { }

        void Create(QGraphicsScene *scene, int x, int y, int64 amount)
        {
            nAmount = amount;
            referenced = true;
            coin = scene->addEllipse(x, y, TILE_SIZE, TILE_SIZE, QPen(Qt::black), QBrush(Qt::yellow));
            text = new QGraphicsTextItem(coin);
            text->setHtml(
                    "<center>"
                    + QString::fromStdString(FormatMoney(nAmount))
                    + "</center>"
                );
            text->setPos(x, y + 13);
            text->setTextWidth(TILE_SIZE);
        }

        void Update(QGraphicsScene *scene, int64 amount)
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
    };

    QGraphicsScene *scene;

    std::map<Coord, CachedCoin> cached_coins;

    // All uncached objects are deleted and recreated on each state update
    std::vector<QGraphicsItem *> uncached_objects;

public:

    GameMapCache(QGraphicsScene *inScene)
        : scene(inScene)
    {
    }

    void StartCachedScene()
    {
        // Mark each cached object as unreferenced
        for (std::map<Coord, CachedCoin>::iterator mi = cached_coins.begin(); mi != cached_coins.end(); ++mi)
            mi->second.referenced = false;
    }

    void PlaceCoin(const Coord &coord, int64 nAmount)
    {
        CachedCoin &c = cached_coins[coord];

        if (!c.coin)
            c.Create(scene, coord.x * TILE_SIZE, coord.y * TILE_SIZE, nAmount);
        else
            c.Update(scene, nAmount);
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
                scene->removeItem(mi->second.coin);
                delete mi->second.coin;
                cached_coins.erase(mi++);
            }
        }
    }

    void AddUncachedObject(QGraphicsItem *obj)
    {
        uncached_objects.push_back(obj);
    }

    void EraseUncachedObjects()
    {
        BOOST_FOREACH(QGraphicsItem *obj, uncached_objects)
        {
            scene->removeItem(obj);
            delete obj;
        }
        uncached_objects.resize(0);
    }
};

GameMapView::GameMapView(QWidget *parent)
    : QGraphicsView(parent)
{
    scene = new QGraphicsScene(this);
    scene->setItemIndexMethod(QGraphicsScene::NoIndex);
    setScene(scene);

    gameMapCache = new GameMapCache(scene);

    setOptimizationFlags(QGraphicsView::DontSavePainterState);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);

    QPixmap bg_tile(TILE_SIZE, TILE_SIZE);
    bg_tile.fill(QColor(0, 180, 0));

    QPainter painter(&bg_tile);
    painter.drawRect(0, 0, TILE_SIZE, TILE_SIZE);
    painter.end();

    setBackgroundBrush(bg_tile);

    setDragMode(QGraphicsView::ScrollHandDrag);

    // Draw coordinate axes
    QGraphicsTextItem *text = new QGraphicsTextItem;
    text->setHtml("<span style='color:white'>&nbsp;&nbsp; &rarr; x<br />&darr;<br />y</span>");
    text->setPos(-6, -12);
    scene->addItem(text);
}

GameMapView::~GameMapView()
{
    delete gameMapCache;
}

void GameMapView::updateGameMap(const GameState &gameState)
{
    gameMapCache->EraseUncachedObjects();
    gameMapCache->StartCachedScene();
    BOOST_FOREACH(const PAIRTYPE(Coord, GameState::LootInfo) &loot, gameState.loot)
        gameMapCache->PlaceCoin(loot.first, loot.second.nAmount);
    gameMapCache->EndCachedScene();

    playerLocations.clear();
    playerLocations.reserve(gameState.players.size());

    // Sort by coordinate bottom-up, so the stacking (multiple players on tile) looks correct
    typedef const std::pair<const PlayerID, PlayerState> *PlayerEntryPtr;
    std::multimap<Coord, PlayerEntryPtr> sortedPlayers;

    for (std::map<PlayerID, PlayerState>::const_iterator mi = gameState.players.begin(); mi != gameState.players.end(); mi++)
    {
        const Coord &coord = mi->second.coord;
        sortedPlayers.insert(std::make_pair(Coord(-coord.x, -coord.y), &(*mi)));
        playerLocations[QString::fromStdString(mi->first)] = QPoint(coord.x, coord.y);
    }

    Coord prev_coord;
    int offs = -1;
    BOOST_FOREACH(const PAIRTYPE(Coord, PlayerEntryPtr) &data, sortedPlayers)
    {
        const PlayerID &playerName = data.second->first;
        const PlayerState &playerState = data.second->second;
        const Coord &coord = playerState.coord;
        int color = playerState.color;

        if (offs >= 0 && coord == prev_coord)
            offs++;
        else
        {
            prev_coord = coord;
            offs = 0;
        }

        int x = coord.x * TILE_SIZE + offs * 2;
        int y = coord.y * TILE_SIZE + offs * 13;
        QGraphicsRectItem *square = scene->addRect(x, y, TILE_SIZE, TILE_SIZE, QPen(Qt::black), QBrush(color == 0 ? Qt::red : Qt::blue));
        QGraphicsSimpleTextItem *simpleText = new QGraphicsSimpleTextItem(QString::fromStdString(playerName), square);
        simpleText->setPos(x + 2, y);
        if (color == 1)
            simpleText->setBrush(QBrush(Qt::white));
        gameMapCache->AddUncachedObject(square);
    }
}

const static QPoint NO_POINT(INT_MAX, INT_MAX);

void GameMapView::CenterMapOnPlayer(const QString &name)
{
    QPoint p = playerLocations.value(name, NO_POINT);
    if (p == NO_POINT)
        return;
    centerOn((p.x() + 0.5) * TILE_SIZE, (p.y() + 0.5) * TILE_SIZE);
}
