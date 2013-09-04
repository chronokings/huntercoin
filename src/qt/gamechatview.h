#ifndef GAMECHATVIEW_H
#define GAMECHATVIEW_H

#include <QObject>

namespace Game
{
    class GameState;
}

// This class acts as a signal proxy, so it can be connected to
// an object emitting GameState and a text box receiving setHtml

// Only messages from the current block are shown

class GameChatView : public QObject
{
    Q_OBJECT

public:

    explicit GameChatView(QWidget *parent = 0);

signals:

    void chatUpdated(const QString &html);

public slots:

    void updateChat(const Game::GameState &gameState);
};

#endif // GAMECHATVIEW_H
