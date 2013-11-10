#ifndef MANAGENAMESPAGE_H
#define MANAGENAMESPAGE_H

#include "../gamestate.h"
#include "gamepathfinder.h"

#include <QDialog>
#include <QSortFilterProxyModel>
#include <QLineEdit>

#include "../json/json_spirit.h"

namespace Ui {
    class ManageNamesPage;
}

class WalletModel;
class NameTableModel;
class GameMapView;
class GameChatView;

QT_BEGIN_NAMESPACE
class QTableView;
class QItemSelection;
class QMenu;
class QModelIndex;
QT_END_NAMESPACE

class NameFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit NameFilterProxyModel(QObject *parent = 0);

    void setNameSearch(const QString &search);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const;

private:
    QString nameSearch;
};

/** Page for managing names */
class ManageNamesPage : public QDialog
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
    NameFilterProxyModel *proxyModel;
    QMenu *contextMenu;
    
    GameMapView *gameMapView;
    GameChatView *gameChatView;

    Game::GameState gameState;
    QString attack;
    QStringList selectedPlayers;

    void ClearSelectedPlayers();
    void RefreshSelectedPlayers();
    bool MakeMove(json_spirit::Object &json);

    // Client-side path-finding
    GamePathfinders pathfinders;

public slots:
    void exportClicked();

    void changedNameFilter(const QString &filter);

private slots:
    void on_submitNameButton_clicked();
    void on_updateButton_clicked();
    void on_attackButton_clicked();
    void onTileClicked(int x, int y);

    bool eventFilter(QObject *object, QEvent *event);

    /** Spawn contextual menu (right mouse menu) for name table entry */
    void contextualMenu(const QPoint &point);
    void onConfigureName(const QModelIndex &index);
    void onClickedPlayer(const QModelIndex &index);
    void onSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected);

    void onCopyNameAction();
    void onCopyAddressAction();
    
    void on_addressBookButton_clicked();
    void on_pasteButton_clicked();
    void on_addressBookButton_2_clicked();
    void on_pasteButton_2_clicked();

    void on_comboBoxAttack_currentIndexChanged(int index);
    void updateGameState(const Game::GameState &gameState);
};

#endif // MANAGENAMESPAGE_H
