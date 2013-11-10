#ifndef GAMEMAPVIEW_H
#define GAMEMAPVIEW_H

#include "gamepathfinder.h"

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QHash>

namespace Game
{
    class PlayerState;
    class GameState;
}

QT_BEGIN_NAMESPACE
class QTimeLine;
QT_END_NAMESPACE

class GameMapCache;
struct GameGraphicsObjects;

class GameMapView : public QGraphicsView
{
    Q_OBJECT

public:

    explicit GameMapView(QWidget *parent = 0);
    ~GameMapView();
    void SelectPlayers(const QStringList &names, const Game::GameState &state, const GamePathfinders &pathfinders);
    void CenterMapOnPlayer(const Game::PlayerState &state);
    void DeselectPlayers();

    const GameGraphicsObjects *grobjs;

public slots:

    void updateGameMap(const Game::GameState &gameState);

signals:

    void tileClicked(int x, int y);

protected:

    QGraphicsScene *scene;
    double zoomFactor;

    virtual void mousePressEvent(QMouseEvent *event);
    virtual void mouseReleaseEvent(QMouseEvent *event);
    virtual void mouseMoveEvent(QMouseEvent *event);
    virtual void wheelEvent(QWheelEvent *event);

private:

    GameMapCache *gameMapCache;
    QGraphicsPathItem *playerPath;
    QPainter::RenderHints defaultRenderHints;

    bool panning;
    bool use_cross_cursor;
    QPoint pan_pos;

    double oldZoom, scheduledZoom;  // For smooth zoom
    QTimeLine *animZoom;

private slots:
    // For smooth zoom
    void scalingTime(qreal t);
    void scalingFinished();
};

#endif // GAMEMAPVIEW_H
