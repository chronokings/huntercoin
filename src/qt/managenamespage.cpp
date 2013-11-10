#include "managenamespage.h"
#include "ui_managenamespage.h"

#include "walletmodel.h"
#include "nametablemodel.h"
#include "csvmodelwriter.h"
#include "guiutil.h"
#include "addressbookpage.h"
#include "../base58.h"
#include "../main.h"
#include "../hook.h"
#include "../wallet.h"
#include "../huntercoin.h"
#include "guiconstants.h"
#include "ui_interface.h"
#include "configurenamedialog.h"
#include "gamemapview.h"
#include "gamechatview.h"

#include <QSortFilterProxyModel>
#include <QMessageBox>
#include <QMenu>
#include <QScrollBar>
#include <QClipboard>

#include "../json/json_spirit.h"
#include "../json/json_spirit_utils.h"
#include "../json/json_spirit_writer_template.h"

extern std::map<std::vector<unsigned char>, PreparedNameFirstUpdate> mapMyNameFirstUpdate;

// Prefix that indicates for the user that the player's reward address isn't the one shown
// (currently this column is hidden though)
const QString NON_REWARD_ADDRESS_PREFIX = "-";

//
// NameFilterProxyModel
//

NameFilterProxyModel::NameFilterProxyModel(QObject *parent /* = 0*/)
    : QSortFilterProxyModel(parent)
{
}

void NameFilterProxyModel::setNameSearch(const QString &search)
{
    nameSearch = search;
    invalidateFilter();
}

bool NameFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);

    QString name = index.sibling(index.row(), NameTableModel::Name).data(Qt::EditRole).toString();

    Qt::CaseSensitivity case_sens = filterCaseSensitivity();
    return name.contains(nameSearch, case_sens);
}

//
// ManageNamesPage
//

const static int COLUMN_WIDTH_NAME = 140;

ManageNamesPage::ManageNamesPage(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ManageNamesPage),
    model(0),
    walletModel(0),
    proxyModel(0)
{
    ui->setupUi(this);

    // Context menu actions
    QAction *copyNameAction = new QAction(tr("Copy &Name"), this);
    QAction *copyAddressAction = new QAction(tr("Copy &Address"), this);

    // Build context menu
    contextMenu = new QMenu();
    contextMenu->addAction(copyNameAction);

    // Connect signals for context menu actions
    connect(copyNameAction, SIGNAL(triggered()), this, SLOT(onCopyNameAction()));
    connect(copyAddressAction, SIGNAL(triggered()), this, SLOT(onCopyAddressAction()));

    connect(ui->tableView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextualMenu(QPoint))); 
    connect(ui->tableView, SIGNAL(clicked(QModelIndex)), this, SLOT(onClickedPlayer(QModelIndex)));
    connect(ui->tableView, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(onConfigureName(QModelIndex)));
    connect(ui->nameFilter, SIGNAL(textChanged(QString)), this, SLOT(changedNameFilter(QString)));

    ui->tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    ui->registerName->installEventFilter(this);
    ui->submitNameButton->installEventFilter(this);
    ui->tableView->installEventFilter(this);
    ui->nameFilter->installEventFilter(this);
    ui->textChat->installEventFilter(this);

    ui->registerName->setMaxLength(MAX_NAME_LENGTH);
    ui->nameFilter->setFixedWidth(COLUMN_WIDTH_NAME);

    ui->nameFilter->setMaxLength(MAX_NAME_LENGTH);

#if QT_VERSION >= 0x040700
    /* Do not move this to the XML file, Qt before 4.7 will choke on it */
    ui->nameFilter->setPlaceholderText(tr("Name filter"));
#endif

    ui->nameFilter->setFixedWidth(COLUMN_WIDTH_NAME);

    gameMapView = new GameMapView(this);
    ui->verticalLayoutGameMap->addWidget(gameMapView);

    connect(gameMapView, SIGNAL(tileClicked(int, int)), this, SLOT(onTileClicked(int, int)));

    gameChatView = new GameChatView(this);
    connect(gameChatView, SIGNAL(chatUpdated(const QString &)), ui->textChat, SLOT(setHtml(const QString &)));
    
    ClearSelectedPlayers();
}

ManageNamesPage::~ManageNamesPage()
{
    delete ui;
}

void ManageNamesPage::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
    model = walletModel->getNameTableModel();

    proxyModel = new NameFilterProxyModel(this);
    proxyModel->setSourceModel(model);
    proxyModel->setDynamicSortFilter(true);
    proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

    ui->tableView->setModel(proxyModel);
    ui->tableView->sortByColumn(0, Qt::AscendingOrder);
    connect(ui->tableView->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this, SLOT(onSelectionChanged(QItemSelection, QItemSelection)));

    ui->tableView->horizontalHeader()->setHighlightSections(false);

    // Set column widths
    ui->tableView->horizontalHeader()->resizeSection(
            NameTableModel::Name, COLUMN_WIDTH_NAME);
    ui->tableView->horizontalHeader()->setResizeMode(
            NameTableModel::Status, QHeaderView::Stretch);

    ui->tableView->setColumnHidden(NameTableModel::Value, true);
    ui->tableView->setColumnHidden(NameTableModel::Address, true);
    ui->tableView->setColumnHidden(NameTableModel::State, true);

    connect(model, SIGNAL(gameStateChanged(const Game::GameState &)), gameChatView, SLOT(updateChat(const Game::GameState &)));
    connect(model, SIGNAL(gameStateChanged(const Game::GameState &)), this, SLOT(updateGameState(const Game::GameState &)));

    ClearSelectedPlayers();

    model->emitGameStateChanged();
}

void ManageNamesPage::changedNameFilter(const QString &filter)
{
    if (!proxyModel)
        return;
    proxyModel->setNameSearch(filter);
}

extern bool IsValidPlayerName(const std::string &);

void ManageNamesPage::on_submitNameButton_clicked()
{
    if (!walletModel)
        return;

    QString name = ui->registerName->text();

    if (!IsValidPlayerName(name.toStdString()))
    {
        QMessageBox::warning(this, tr("Name registration error"),
              tr("The entered name is invalid. Allowed characters: alphanumeric, underscore, hyphen, whitespace (but not at start/end and no double whitespaces)."),
              QMessageBox::Ok);
        return;
    }

    if (!walletModel->nameAvailable(name))
    {
        QMessageBox::warning(this, tr("Name registration"), tr("Name not available"));
        ui->registerName->setFocus();
        return;
    }

    /*if (QMessageBox::Yes != QMessageBox::question(this, tr("Confirm name registration"),
          tr("Are you sure you want to create player %1?").arg(name),
          QMessageBox::Yes | QMessageBox::Cancel,
          QMessageBox::Cancel))
    {
        return;
    }*/

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;
    
    QString err_msg;

    try
    {
        WalletModel::NameNewReturn res = walletModel->nameNew(name);

        if (res.ok)
        {
            ui->registerName->setText("");
            ui->submitNameButton->setDefault(true);

            int newRowIndex;
            // FIXME: CT_NEW may have been sent from nameNew (via transaction).
            // Currently updateEntry is modified so it does not complain
            model->updateEntry(name, "", res.address, NameTableEntry::NAME_NEW, CT_NEW, &newRowIndex);
            ui->tableView->selectRow(newRowIndex);
            ui->tableView->setFocus();

            ConfigureNameDialog dlg(name, "", res.address, this);
            dlg.setModel(walletModel);
            if (dlg.exec() == QDialog::Accepted)
            {
                LOCK(cs_main);
                if (mapMyNameFirstUpdate.count(vchFromString(name.toStdString())) != 0)
                    model->updateEntry(name, dlg.getReturnData(), res.address, NameTableEntry::NAME_NEW, CT_UPDATED);
                else
                {
                    // name_firstupdate could have been sent, while the user was editing the value
                    // Do nothing
                }
            }

            return;
        }

        err_msg = res.err_msg;
    }
    catch (std::exception& e) 
    {
        err_msg = e.what();
    }

    if (err_msg == "ABORTED")
        return;

    QMessageBox::warning(this, tr("Name registration failed"), err_msg);
}

void ManageNamesPage::on_updateButton_clicked()
{
    json_spirit::Object json;
    MakeMove(json);
}

void ManageNamesPage::on_attackButton_clicked()
{
    int index = ui->comboBoxAttack->currentIndex();
    attack = ui->comboBoxAttack->itemData(index).toString();

    if (attack.isEmpty())
        return;

    json_spirit::Object json;
    json.push_back(json_spirit::Pair("attack", attack.toStdString()));
    MakeMove(json);
}

void ManageNamesPage::onTileClicked(int x, int y)
{
    if (selectedPlayers.isEmpty())
        return;

    if (!walletModel)
        return;

    json_spirit::Object json;

    if (selectedPlayers.size() <= 1) // Message is ignored when many players are selected
    {
        QString msg = ui->messageEdit->toPlainText();
        if (!msg.isEmpty())
            json.push_back(json_spirit::Pair("message", msg.toStdString()));
    }
    QString reward = ui->rewardAddr->text();
    if (!reward.isEmpty() && !walletModel->validateAddress(reward))
    {
        ui->rewardAddr->setValid(false);
        return;
    }

    QString addr = ui->transferTo->text();
    if (!addr.isEmpty() && !walletModel->validateAddress(addr))
    {
        ui->transferTo->setValid(false);
        return;
    }

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    foreach (QString selectedPlayer, selectedPlayers)
    {
        std::string pl = selectedPlayer.toStdString();
        std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(pl);
        if (it == gameState.players.end())
            continue;

        bool addr_field_added = false;
        if (reward.toStdString() != it->second.address)
        {
            json.push_back(json_spirit::Pair("address", reward.toStdString()));
            addr_field_added = true;
        }

        GamePathfinder &pf = pathfinders[pl];
        if (!pf.FindPath(it->second.coord, Game::Coord(x, y)))
        {
            pathfinders.erase(pl);
            continue;
        }
        Game::Coord c;
        if (!pf.GetNextWaypoint(c))
            pathfinders.erase(pl);
        printf("Sending first waypoint for player %s: (%d, %d)\n", pl.c_str(), c.x, c.y);

        json.push_back(json_spirit::Pair("x", c.x));
        json.push_back(json_spirit::Pair("y", c.y));

        json_spirit::Value val(json);
        std::string jsonStr = json_spirit::write_string(val, false);
        QString data = QString::fromStdString(jsonStr);

        QString err_msg;
        try
        {
            err_msg = walletModel->nameUpdate(selectedPlayer, data, addr);
        }
        catch (std::exception& e) 
        {
            err_msg = e.what();
        }

        if (!err_msg.isEmpty())
        {
            if (err_msg == "ABORTED")
                return;

            QMessageBox::critical(this, tr("Name update error"), err_msg);
            return;
        }

        json.pop_back(); // y
        json.pop_back(); // x
        if (addr_field_added)
            json.pop_back();
    }

    ui->messageEdit->setPlainText(QString());
    ui->transferTo->setText(QString());
}

bool ManageNamesPage::MakeMove(json_spirit::Object &json)
{
    if (selectedPlayers.isEmpty())
        return false;

    if (!walletModel)
        return false;

    if (selectedPlayers.size() <= 1) // Message is ignored when many players are selected
    {
        QString msg = ui->messageEdit->toPlainText();
        if (!msg.isEmpty())
            json.push_back(json_spirit::Pair("message", msg.toStdString()));
    }
    QString reward = ui->rewardAddr->text();
    if (!reward.isEmpty() && !walletModel->validateAddress(reward))
    {
        ui->rewardAddr->setValid(false);
        return false;
    }

    QString addr = ui->transferTo->text();
    if (!addr.isEmpty() && !walletModel->validateAddress(addr))
    {
        ui->transferTo->setValid(false);
        return false;
    }

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return false;

    foreach (QString selectedPlayer, selectedPlayers)
    {
        std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(selectedPlayer.toStdString());

        bool addr_field_added = false;
        if (it == gameState.players.end() || reward.toStdString() != it->second.address)
        {
            json.push_back(json_spirit::Pair("address", reward.toStdString()));
            addr_field_added = true;
        }

        // TODO: if move is attack and player isn't within range, do not send attack move

        json_spirit::Value val(json);
        std::string jsonStr = json_spirit::write_string(val, false);
        QString data = QString::fromStdString(jsonStr);

        QString err_msg;
        try
        {
            err_msg = walletModel->nameUpdate(selectedPlayer, data, addr);
        }
        catch (std::exception& e) 
        {
            err_msg = e.what();
        }

        if (!err_msg.isEmpty())
        {
            if (err_msg == "ABORTED")
                return false;

            QMessageBox::critical(this, tr("Name update error"), err_msg);
            return false;
        }
        
        if (addr_field_added)
            json.pop_back();
    }

    ui->messageEdit->setPlainText(QString());
    ui->transferTo->setText(QString());

    return true;
}

bool ManageNamesPage::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FocusIn)
        ui->submitNameButton->setDefault(object == ui->registerName || object == ui->submitNameButton);
    return QDialog::eventFilter(object, event);
}

void ManageNamesPage::onClickedPlayer(const QModelIndex &index)
{
    QString selectedPlayer = index.sibling(index.row(), NameTableModel::Name).data(Qt::EditRole).toString();

    std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(selectedPlayer.toStdString());
    if (it != gameState.players.end())
        gameMapView->CenterMapOnPlayer(it->second);
}

void ManageNamesPage::onSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    if (!deselected.isEmpty())
    {
        attack = QString();
        ui->groupBoxMove->setEnabled(true);
        ui->transferTo->setText(QString());
        ui->messageEdit->setPlainText(QString());
    }

    foreach (QModelIndex index, deselected.indexes())
    {
        QString deselectedPlayer = index.sibling(index.row(), NameTableModel::Name).data(Qt::EditRole).toString();
        selectedPlayers.removeAll(deselectedPlayer);
    }

    foreach (QModelIndex index, selected.indexes())
    {
        QString selectedPlayer = index.sibling(index.row(), NameTableModel::Name).data(Qt::EditRole).toString();
        if (selectedPlayers.contains(selectedPlayer))
            continue;

        selectedPlayers.append(selectedPlayer);

        std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(selectedPlayer.toStdString());
        if (it == gameState.players.end())
            continue;

        if (ui->rewardAddr->text().isEmpty())
            ui->rewardAddr->setText(QString::fromStdString(it->second.address));
    }

    RefreshSelectedPlayers();
}

void ManageNamesPage::ClearSelectedPlayers()
{
    gameMapView->DeselectPlayers();
    attack = QString();
    ui->groupBoxMove->setEnabled(false);
    ui->transferTo->setText(QString());
    ui->rewardAddr->setText(QString());
    ui->messageEdit->setPlainText(QString());
    ui->comboBoxAttack->clear();
    ui->attackButton->setEnabled(false);
}

void ManageNamesPage::RefreshSelectedPlayers()
{
    if (selectedPlayers.isEmpty())
    {
        ClearSelectedPlayers();
        return;
    }

    ui->groupBoxMove->setEnabled(true);
    ui->messageEdit->setEnabled(selectedPlayers.size() == 1);

    std::set<std::string> victims;

    foreach (QString selectedPlayer, selectedPlayers)
    {
        std::vector<std::string> vec = gameState.ListPossibleAttacks(selectedPlayer.toStdString());
        victims.insert(vec.begin(), vec.end());
    }

    ui->comboBoxAttack->clear();
    BOOST_FOREACH(std::string victim, victims)
    {
        QString s = QString::fromStdString(victim);
        ui->comboBoxAttack->addItem(GUIUtil::HtmlEscape(s), s);
        int idx = ui->comboBoxAttack->findData(s);
        QString colorCSS = GameChatView::ColorCSS[gameState.players[victim].color];
        ui->comboBoxAttack->setItemData(idx, QColor(colorCSS), Qt::ForegroundRole);
    }

    ui->comboBoxAttack->setCurrentIndex(ui->comboBoxAttack->findData(attack));

    gameMapView->SelectPlayers(selectedPlayers, gameState, pathfinders);
}

void ManageNamesPage::on_comboBoxAttack_currentIndexChanged(int index)
{
    attack = ui->comboBoxAttack->itemData(index).toString();
    ui->attackButton->setEnabled(!attack.isEmpty());
}

void ManageNamesPage::updateGameState(const Game::GameState &gameState)
{
    this->gameState = gameState;

    gameMapView->updateGameMap(gameState);

    if (!selectedPlayers.isEmpty())
    {
        /*
        // Remove dead players from selection
        QStringList newSelectedPlayers;
        foreach (QString selectedPlayer, selectedPlayers)
            if (gameState.players.count(selectedPlayer.toStdString()))
                newSelectedPlayers.append(selectedPlayer);

        selectedPlayers = newSelectedPlayers;
        */

        RefreshSelectedPlayers();
    }

    // Send new waypoint for players that reached the current one

    // TODO: instead of waiting for player stopping at the current waypoint,
    // sent immediately when the next waypoint becomes linearly reachable
    // from every position between current position and current waypoint

    std::vector<Game::PlayerID> vUpdate, vDelete;
    BOOST_FOREACH(PAIRTYPE(const Game::PlayerID, GamePathfinder)& item, pathfinders)
    {
        const std::string &pl = item.first;
        GamePathfinder &pf = item.second;

        std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(pl);
        if (it == gameState.players.end())
            continue;

        if (it->second.coord == it->second.target)
        {
            // Player stopped
            // Queue an update, unless already pending
            std::vector<unsigned char> vchName = vchFromString(pl);
            if (!(mapNamePending.count(vchName) && mapNamePending[vchName].size()))
                vUpdate.push_back(pl);
        }
        else if (it->second.target != pf.GetCurWaypoint())
        {
            // Waypoint changed externally, reject pathfinding
            printf("Waypoint changed for player %s: expected target (%d, %d), actual target (%d, %d). Discarding pathfinding.\n", pl.c_str(), pf.GetCurWaypoint().x, pf.GetCurWaypoint().y, it->second.target.x, it->second.target.y);
            vDelete.push_back(pl);
        }
    }

    BOOST_FOREACH(const Game::PlayerID &pl, vDelete)
        pathfinders.erase(pl);

    if (vUpdate.empty())
        return;

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    BOOST_FOREACH(const Game::PlayerID &pl, vUpdate)
    {
        GamePathfinder &pf = pathfinders[pl];

        std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(pl);

        Game::Coord c;
        if (!pf.GetNextWaypoint(c))
            pathfinders.erase(pl);
        printf("Sending next waypoint for player %s: (%d, %d)\n", pl.c_str(), c.x, c.y);

        json_spirit::Object json;
        json.push_back(json_spirit::Pair("x", c.x));
        json.push_back(json_spirit::Pair("y", c.y));

        json_spirit::Value val(json);
        std::string jsonStr = json_spirit::write_string(val, false);
        QString data = QString::fromStdString(jsonStr);

        QString err_msg;
        try
        {
            err_msg = walletModel->nameUpdate(QString::fromStdString(pl), data, "");
        }
        catch (std::exception& e) 
        {
            err_msg = e.what();
        }

        if (!err_msg.isEmpty())
        {
            if (err_msg == "ABORTED")
                return;

            QMessageBox::critical(this, tr("Name update error when sending next waypoint"), err_msg);
            return;
        }
    }
}

void ManageNamesPage::contextualMenu(const QPoint &point)
{
    QModelIndex index = ui->tableView->indexAt(point);
    if (index.isValid())
        contextMenu->exec(QCursor::pos());
}

void ManageNamesPage::onConfigureName(const QModelIndex &index)
{
    QString name = index.sibling(index.row(), NameTableModel::Name).data(Qt::EditRole).toString();
    QString value = index.sibling(index.row(), NameTableModel::Value).data(Qt::EditRole).toString();
    QString address = index.sibling(index.row(), NameTableModel::Address).data(Qt::EditRole).toString();

    if (address.startsWith(NON_REWARD_ADDRESS_PREFIX))
        address = address.mid(NON_REWARD_ADDRESS_PREFIX.size());

    std::vector<unsigned char> vchName = vchFromString(name.toStdString());

    bool fOldPostponed;

    {
        LOCK(cs_main);
        if (mapMyNameFirstUpdate.count(vchName) != 0)
        {
            // Postpone the firstupdate, while the dialog is open
            // If OK is pressed, fPostponed is always set to false
            // If Cancel is pressed, we restore the old fPostponed value
            fOldPostponed = mapMyNameFirstUpdate[vchName].fPostponed;
            mapMyNameFirstUpdate[vchName].fPostponed = true;
        }
        else
            return;   // We only allow ConfigureNameDialog for name_new. Name_update should be done via the game GUI.
    }

    ConfigureNameDialog dlg(name, value, address, this);
    dlg.setModel(walletModel);
    int dlgRes = dlg.exec();

    if (dlgRes == QDialog::Accepted)
    {
        LOCK(cs_main);
        // name_firstupdate could have been sent, while the user was editing the value
        if (mapMyNameFirstUpdate.count(vchName) != 0)
            model->updateEntry(name, dlg.getReturnData(), address, NameTableEntry::NAME_NEW, CT_UPDATED);
    }
    else
    {
        LOCK(cs_main);
        // If cancel was pressed, restore the old fPostponed value
        if (mapMyNameFirstUpdate.count(vchName) != 0)
            mapMyNameFirstUpdate[vchName].fPostponed = fOldPostponed;
    }
}

void ManageNamesPage::onCopyNameAction()
{
    GUIUtil::copyEntryData(ui->tableView, NameTableModel::Name);
}

void ManageNamesPage::onCopyAddressAction()
{
    GUIUtil::copyEntryData(ui->tableView, NameTableModel::Address);
}

void ManageNamesPage::on_pasteButton_clicked()
{
    // Paste text from clipboard into reward address field
    ui->rewardAddr->setText(QApplication::clipboard()->text());
}

void ManageNamesPage::on_addressBookButton_clicked()
{
    if (!walletModel)
        return;
    AddressBookPage dlg(AddressBookPage::ForSending, AddressBookPage::SendingTab, this);
    dlg.setModel(walletModel->getAddressTableModel());
    if (dlg.exec())
        ui->rewardAddr->setText(dlg.getReturnValue());
}

void ManageNamesPage::on_pasteButton_2_clicked()
{
    // Paste text from clipboard into "transfer to" field
    ui->transferTo->setText(QApplication::clipboard()->text());
}

void ManageNamesPage::on_addressBookButton_2_clicked()
{
    if (!walletModel)
        return;
    AddressBookPage dlg(AddressBookPage::ForSending, AddressBookPage::SendingTab, this);
    dlg.setModel(walletModel->getAddressTableModel());
    if (dlg.exec())
        ui->transferTo->setText(dlg.getReturnValue());
}

void ManageNamesPage::exportClicked()
{
    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(
            this,
            tr("Export Registered Names Data"), QString(),
            tr("Comma separated file (*.csv)"));

    if (filename.isNull())
        return;

    CSVModelWriter writer(filename);

    writer.setModel(proxyModel);
    // name, column, role
    writer.addColumn("Name", NameTableModel::Name, Qt::EditRole);
    writer.addColumn("Last Move", NameTableModel::Value, Qt::EditRole);
    writer.addColumn("Address", NameTableModel::Address, Qt::EditRole);
    writer.addColumn("State", NameTableModel::State, Qt::EditRole);
    writer.addColumn("Status", NameTableModel::Status, Qt::EditRole);

    if(!writer.write())
    {
        QMessageBox::critical(this, tr("Error exporting"), tr("Could not write to file %1.").arg(filename),
                              QMessageBox::Abort, QMessageBox::Abort);
    }
}
