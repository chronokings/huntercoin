#ifndef GAMEMAPVIEW_H
#define GAMEMAPVIEW_H

#include <QGraphicsView>
#include <QHash>

namespace Game
{
    class GameState;
}

class GameMapView : public QGraphicsView
{
    Q_OBJECT

public:

    explicit GameMapView(QWidget *parent = 0);
    void CenterMapOnPlayer(const QString &name);

public slots:

    void updateGameMap(const Game::GameState &gameState);

protected:

    // On right-click just center the scene
    virtual void contextMenuEvent(QContextMenuEvent *event)
    {
        centerOn(0, 0);
    }

private:

    QHash<QString, QPoint> playerLocations;
};

#endif // GAMEMAPVIEW_H
