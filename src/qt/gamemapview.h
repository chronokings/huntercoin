#ifndef GAMEMAPVIEW_H
#define GAMEMAPVIEW_H

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QHash>

namespace Game
{
    class GameState;
}

class GameMapCache;

class GameMapView : public QGraphicsView
{
    Q_OBJECT

public:

    explicit GameMapView(QWidget *parent = 0);
    ~GameMapView();
    void CenterMapOnPlayer(const QString &name);

public slots:

    void updateGameMap(const Game::GameState &gameState);

protected:

    QGraphicsScene *scene;

    // On right-click just center the scene
    virtual void contextMenuEvent(QContextMenuEvent *event)
    {
        centerOn(0, 0);
    }

private:

    // Player locations for centering when player name is clicked
    QHash<QString, QPoint> playerLocations;

    GameMapCache *gameMapCache;
};

#endif // GAMEMAPVIEW_H
