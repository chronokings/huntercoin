#ifndef GAMEMAPVIEW_H
#define GAMEMAPVIEW_H

#include "../gamemovecreator.h"

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QHash>

#include <vector>

namespace Game
{
    class CharacterState;
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
    void SelectPlayer(const QString &name, const Game::GameState &state, QueuedMoves &queuedMoves);
    void CenterMapOnCharacter(const Game::CharacterState &state);
    void DeselectPlayer();

    const GameGraphicsObjects *grobjs;

public slots:

    void updateGameMap(const Game::GameState &gameState);
    void zoomIn();
    void zoomOut();
    void zoomReset();

signals:

    void tileClicked(int x, int y, bool ctrlPressed);

protected:

    QGraphicsScene *scene;
    QGraphicsPixmapItem *crown;
    double zoomFactor;

    virtual void mousePressEvent(QMouseEvent *event);
    virtual void mouseReleaseEvent(QMouseEvent *event);
    virtual void mouseMoveEvent(QMouseEvent *event);
    virtual void wheelEvent(QWheelEvent *event);

private:

    GameMapCache *gameMapCache;
    QGraphicsPathItem *playerPath, *queuedPlayerPath;
    std::vector<QGraphicsRectItem*> banks;
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
