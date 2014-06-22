#ifndef MANAGENAMESPAGE_H
#define MANAGENAMESPAGE_H

#include "../gamestate.h"
#include "../gamemovecreator.h"

#include <QWidget>

#include "../json/json_spirit.h"

namespace Ui {
    class ManageNamesPage;
}

class WalletModel;
class NameTableModel;
class CharacterTableModel;
class GameMapView;
class GameChatView;

QT_BEGIN_NAMESPACE
class QTableView;
class QItemSelection;
class QModelIndex;
class QTimeLine;
QT_END_NAMESPACE

/** Page for managing names */
class ManageNamesPage : public QWidget
{
    Q_OBJECT

public:
    explicit ManageNamesPage(QWidget *parent = 0);
    ~ManageNamesPage();

    void setModel(WalletModel *walletModel);

private:
    Ui::ManageNamesPage *ui;
    NameTableModel *model;
    WalletModel *walletModel;

    GameMapView *gameMapView;
    GameChatView *gameChatView;

    Game::GameState gameState;
    QTimeLine *chrononAnim;  // Flash block number when a new block arrives

    class CharacterTableModel *characterTableModel;

    QString selectedPlayer;
    QStringList selectedCharacters;

    class NameTabs *tabsNames;
    QString rewardAddr, transferTo;
    bool rewardAddrChanged;
    QueuedMoves queuedMoves;

    void RefreshCharacterList();
    void UpdateQueuedMoves();
    void SetPlayerEnabled(bool enable);
    void SetPlayerMoveEnabled(bool enable = true);

public slots:
    void exportClicked();
    void PendingStatusChanged();

private slots:
    void on_newButton_clicked();
    void on_configButton_clicked();
    void on_destructButton_clicked();
    void on_goButton_clicked();
    void on_cancelButton_clicked();
    void onTileClicked(int x, int y, bool ctrlPressed);

    void onSelectName(const QString &name);
    void onCharacterSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected);
    void onClickedCharacter(const QModelIndex &index);

    void chrononAnimChanged(qreal t);
    void chrononAnimFinished();

    void updateGameState(const Game::GameState &gameState);
};

#endif // MANAGENAMESPAGE_H
