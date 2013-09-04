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

private:

    QHash<QString, QPoint> playerLocations;
};

#endif // GAMEMAPVIEW_H
