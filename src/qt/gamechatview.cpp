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

#ifndef Q_MOC_RUN
#include <boost/foreach.hpp>
#endif

using namespace Game;

QString GameChatView::ColorCSS[Game::NUM_TEAM_COLORS] = { "#e8d800", "#e00000", "#00d000", "#0000ff" };

GameChatView::GameChatView(QWidget *parent)
    : QObject(parent)
{
}


void MessagesToHTML_Helper(QString &msgs, int nHeight, const std::map<PlayerID, PlayerState> &players)
{
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, players)
    {
        const PlayerState &pl = p.second;
        if (pl.message.empty() || pl.message_block < nHeight)
            continue;

        bool first = msgs.isEmpty();
        if (!first)
            msgs += "<br />";
        msgs += "<span class='C";
        msgs += char('0' + pl.color);
        msgs += "'>" + GUIUtil::HtmlEscape(p.first) + ":</span> ";
        //if (first)
        //    msgs += QString("<a name='block%1' />").arg(gameState.nHeight);
        msgs += GUIUtil::HtmlEscape(pl.message);
    }
}


QString GameChatView::MessagesToHTML(const GameState &gameState)
{
    QString msgs;
    msgs.reserve(4000);
    MessagesToHTML_Helper(msgs, gameState.nHeight, gameState.players);
    // TODO: add some marker to dead players, e.g. strike-through or non-bold font or "[dead]" suffix
    MessagesToHTML_Helper(msgs, gameState.nHeight, gameState.dead_players_chat);
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
    for (size_t i = 0, n = html_msgs.size(); i < n; i++)
        s += html_msgs[i].size();
    QString strHTML;
    strHTML.reserve(1000 + s + s / 10);

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

    for (size_t i = 0, n = html_msgs.size(); i < n; i++)
    {
        if (i > 0 && !html_msgs[i - 1].isEmpty())
            strHTML += "\n<hr />\n";
        strHTML += html_msgs[i];
    }

    strHTML += "<a name='end' /></font></html>";

    emit chatUpdated(strHTML);
    //if (!html_msgs.back().isEmpty())
    //    emit chatScrollToAnchor(QString("block%1").arg(gameState.nHeight));
    emit chatScrollToAnchor("end");
}
