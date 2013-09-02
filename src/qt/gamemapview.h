#ifndef GAMEMAPVIEW_H
#define GAMEMAPVIEW_H

#include <QGraphicsView>

namespace Game
{
    class GameState;
}

class GameMapView : public QGraphicsView
{
    Q_OBJECT

public:

    explicit GameMapView(QWidget *parent = 0);

public slots:
    void updateGameMap(const Game::GameState &gameState);
};

#endif // GAMEMAPVIEW_H
