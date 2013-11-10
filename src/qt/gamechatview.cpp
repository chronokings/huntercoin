#include "gamechatview.h"

#include "../gamestate.h"
#include "../gamedb.h"
#include "../util.h"
#include "guiutil.h"

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QGraphicsPixmapItem>
#include <QGraphicsSimpleTextItem>

#include <boost/foreach.hpp>

using namespace Game;

QString GameChatView::ColorCSS[Game::NUM_TEAM_COLORS] = { "#f8e600", "red", "lime", "blue" };

GameChatView::GameChatView(QWidget *parent)
    : QObject(parent)
{
}

QString GameChatView::MessagesToHTML(const GameState &gameState)
{
    QString msgs;
    msgs.reserve(4000);

    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, gameState.players)
    {
        const PlayerState &pl = p.second;
        if (pl.message.empty() || pl.message_block < gameState.nHeight)
            continue;

        if (!msgs.isEmpty())
            msgs += "<br />";
        msgs += "<span class='C";
        msgs += char('0' + pl.color);
        msgs += "'>" + GUIUtil::HtmlEscape(p.first) + ":</span> " + GUIUtil::HtmlEscape(pl.message);
    }
    return msgs;
}

void GameChatView::updateChat(const GameState &gameState)
{

    // Detect chain rollback and delete messages from disconnected blocks
    for (int i = heights.size() - 1; i >= 0; i--)
    {
        if (heights[i] >= gameState.nHeight)
        {
            heights.erase(heights.begin() + i);
            html_msgs.removeAt(i);
        }
    }

    html_msgs.push_back(MessagesToHTML(gameState));
    heights.push_back(gameState.nHeight);

    // Keep only the messages from last MAX_BLOCKS blocks
    if (heights.size() > MAX_BLOCKS)
    {
        html_msgs.erase(html_msgs.begin());
        heights.erase(heights.begin());
    }

    int s = 0;
    for (int i = 0; i < html_msgs.size(); i++)
        s += html_msgs[i].size();
    QString strHTML;
    strHTML.reserve(1000 + s);

    strHTML += "<html><font face='verdana, arial, helvetica, sans-serif'><style type='text/css'>"
        "p { margin: 1em inherit } ";
    for (int i = 0; i < Game::NUM_TEAM_COLORS; i++)
    {
        strHTML += ".C";
        strHTML += char('0' + i);
        strHTML += " { font-weight: bold; color:";
        strHTML += ColorCSS[i];
        strHTML += " } ";
    }
    strHTML += "</style>";

    for (int i = 0; i < html_msgs.size(); i++)
    {
        if (i > 0 && !html_msgs[i - 1].isEmpty())
            strHTML += "\n<hr />\n";
        strHTML += html_msgs[i];
    }

    strHTML += "</font></html>";

    emit chatUpdated(strHTML);
}
