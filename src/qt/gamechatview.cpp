#include "gamechatview.h"

#include "../gamestate.h"
#include "../util.h"
#include "guiutil.h"

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QGraphicsPixmapItem>
#include <QGraphicsSimpleTextItem>

#include <boost/foreach.hpp>

using namespace Game;

GameChatView::GameChatView(QWidget *parent)
    : QObject(parent)
{
}

void GameChatView::updateChat(const GameState &gameState)
{
    QString strHTML;
    strHTML.reserve(4000);

    strHTML += "<html><font face='verdana, arial, helvetica, sans-serif'><style type='text/css'>p { margin: 1em inherit }</style>";

    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, gameState.players)
    {
        if (p.second.message.empty() || p.second.message_block < gameState.nHeight)
            continue;

        strHTML += "<p><b>" + GUIUtil::HtmlEscape(p.first) + ":</b> " + GUIUtil::HtmlEscape(p.second.message) + "</p>";
    }
        
    strHTML += "</font></html>";

    emit chatUpdated(strHTML);
}
