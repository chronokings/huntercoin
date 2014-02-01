#ifndef GAMECHATVIEW_H
#define GAMECHATVIEW_H

#include <QObject>
#include <QStringList>
#include <vector>

namespace Game
{
    class GameState;
}

// This class acts as a signal proxy, so it can be connected to
// an object emitting GameState and a text box receiving setHtml

class GameChatView : public QObject
{
    Q_OBJECT

public:

    explicit GameChatView(QWidget *parent = 0);

    static QString ColorCSS[];

signals:

    void chatUpdated(const QString &html);
    void chatScrollToAnchor(const QString &anchor);

public slots:

    void updateChat(const Game::GameState &gameState);

private:

    static const int MAX_BLOCKS = 10;
    QStringList html_msgs;
    std::vector<int> heights;
    QString MessagesToHTML(const Game::GameState &gameState);
};

#endif // GAMECHATVIEW_H
