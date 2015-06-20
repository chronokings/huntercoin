#include "managenamespage.h"
#include "ui_managenamespage.h"

#include "walletmodel.h"
#include "nametablemodel.h"
#include "csvmodelwriter.h"
#include "guiutil.h"
#include "addressbookpage.h"
#include "../headers.h"
#include "../wallet.h"
#include "../huntercoin.h"
#include "guiconstants.h"
#include "ui_interface.h"
#include "configurenamedialog1.h"
#include "configurenamedialog2.h"
#include "gamemapview.h"
#include "gamechatview.h"

#include <QMessageBox>
#include <QScrollBar>
#include <QTimeLine>
#include <QInputDialog>
#include <QShortcut>

#include "../json/json_spirit.h"
#include "../json/json_spirit_utils.h"
#include "../json/json_spirit_writer_template.h"

class CharacterTableModel : public QAbstractTableModel
{
    Q_OBJECT
    Game::PlayerID player;
    Game::PlayerState state;
    Game::CharacterID crownHolder;
    std::vector<int> alive;
    const QueuedPlayerMoves &queuedMoves;
    bool pending;      // Global flag if player transaction state is pending

public:
    CharacterTableModel(const Game::PlayerID &player_, const Game::PlayerState &state_, const QueuedPlayerMoves &queuedMoves_, Game::CharacterID crownHolder_)
        : player(player_), state(state_), queuedMoves(queuedMoves_), crownHolder(crownHolder_), pending(false)
    {
        BOOST_FOREACH(const PAIRTYPE(int, Game::CharacterState) &pc, state.characters)
            alive.push_back(pc.first);
    }

    enum ColumnIndex {
        Name = 0,
        Coins = 1,
        Status = 2,
        Time = 3,
        Life = 4,
        NUM_COLUMNS
    };

    /** @name Methods overridden from QAbstractTableModel
        @{*/
    int rowCount(const QModelIndex &parent = QModelIndex()) const
    {
        Q_UNUSED(parent);
        return alive.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const
    {
        Q_UNUSED(parent);
        return NUM_COLUMNS;
    }

    void MoveQueueChanged()
    {
        emit dataChanged(index(0, Status), index(alive.size() - 1, Status));
    }

    void SetPending(bool pending_)
    {
        if (pending != pending_)
        {
            pending = pending_;
            emit dataChanged(index(0, Status), index(alive.size() - 1, Status));
        }
    }

    QVariant data(const QModelIndex &index, int role) const
    {
        if (!index.isValid())
            return QVariant();

        if (role == Qt::DisplayRole || role == Qt::EditRole)
        {
            const int i = alive[index.row()];
            QueuedPlayerMoves::const_iterator mi;
            std::map<int, Game::CharacterState>::const_iterator mi2;
            switch (index.column())
            {
                case Name:
                {
                    // TODO: for sorting return the index as number
                    Game::CharacterID chid(player, i);
                    QString ret = QString::fromStdString(chid.ToString());
                    if (role == Qt::DisplayRole)
                    {
                        if (i == 0)
                            ret = QString::fromUtf8("\u2605") + ret;
                        if (crownHolder == chid)
                            ret += QString::fromUtf8(" \u265B");
                    }
                    return ret;
                }

                case Coins:
                    mi2 = state.characters.find(i);
                    if (mi2 != state.characters.end())
                        return QString::fromStdString(FormatMoney(mi2->second.loot.nAmount));    // TODO: for sorting return as float value
                    return QVariant();

                case Status:
                    if (pending)
                        return tr("Pending");
                    mi = queuedMoves.find(i);
                    if (mi != queuedMoves.end())
                    {
                        if (mi->second.destruct)
                            return tr("Destruct");
                        else
                            return tr("Queued");
                    }
                    else
                    {
                        mi2 = state.characters.find(i);
                        if (mi2 != state.characters.end() && mi2->second.waypoints.empty())
                            return tr("Ready");
                        else
                            return tr("Moving");
                    }

                case Time:
                  {
                    unsigned val = 0;
                    mi = queuedMoves.find(i);
                    mi2 = state.characters.find(i);
                    const Game::WaypointVector* wp = NULL;
                    if (mi != queuedMoves.end())
                        wp = &mi->second.waypoints;
                    if (mi2 != state.characters.end())
                        val = mi2->second.TimeToDestination(wp);
                    if (val > 0)
                        return QString("%1").arg(val);
                    return "";
                  }

                case Life:
                  {
                    /* Show remaining life only for general.  */
                    if (i != 0)
                      return "";

                    if (state.remainingLife == -1)
                      return QString("%1").arg (state.value / COIN);

                    assert (state.remainingLife > 0);
                    return QString("%1").arg (state.remainingLife);
                  }
            }
        }
        else if (role == Qt::TextAlignmentRole)
            return QVariant(int(Qt::AlignLeft|Qt::AlignVCenter));

        return QVariant();
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation == Qt::Horizontal)
        {
            if (role == Qt::DisplayRole)
            {
                switch (section)
                {
                    case Name:
                        return tr("Name");
                    case Coins:
                        return tr("Coins");
                    case Status:
                        return tr("Status");
                    case Time:
                        return tr("Time");
                    case Life:
                        return tr("Life");
                }
            }
            else if (role == Qt::TextAlignmentRole)
                return QVariant(int(Qt::AlignLeft|Qt::AlignVCenter));
            else if (role == Qt::ToolTipRole)
            {
                switch (section)
                {
                    case Name:
                        return tr("Character name");
                    case Coins:
                        return tr("Amount of collected loot (return the character to spawn area to cash out)");
                    case Status:
                        return tr("Character status");
                    case Time:
                        return tr("Time until destination is reached (in blocks)");
                    case Life:
                        return tr("Remaining life while poisoned or health value if not poisoned");
                }
            }
        }
        return QVariant();
    }

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const
    {
        Q_UNUSED(parent);
        return createIndex(row, column);
    }

    Qt::ItemFlags flags(const QModelIndex &index) const
    {
        if (!index.isValid())
            return 0;

        return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    }
    /*@}*/

    int FindRow(const QString &name)
    {
        Game::CharacterID chid = Game::CharacterID::Parse(name.toStdString());
        if (chid.player != player)
            return -1;
        std::vector<int>::const_iterator it = std::lower_bound(alive.begin(), alive.end(), chid.index);
        if (it == alive.end())
            return -1;
        if (*it != chid.index)
            return -1;
        return it - alive.begin();
    }
};

class NameTabs : public QTabBar
{
    Q_OBJECT

public:
    NameTabs(NameTableModel *model_, QWidget *parent = 0)
        : QTabBar(parent), model(model_)
    {
#define NAME_TABS_CONNECT_HELPER(x) \
            connect(model, SIGNAL(x), this, SLOT(x))

        NAME_TABS_CONNECT_HELPER(dataChanged(const QModelIndex &, const QModelIndex &));
        NAME_TABS_CONNECT_HELPER(modelReset());
        NAME_TABS_CONNECT_HELPER(rowsInserted(const QModelIndex &, int, int));
        NAME_TABS_CONNECT_HELPER(rowsMoved(const QModelIndex &, int, int, const QModelIndex &, int));
        NAME_TABS_CONNECT_HELPER(rowsRemoved(const QModelIndex &, int, int));

#undef NAME_TABS_CONNECT_HELPER

        connect(this, SIGNAL(currentChanged(int)), this, SLOT(onCurrentChanged(int)));

        setStyleSheet("QTabBar { font-weight: bold }");

        CreateTabs();
    }

    void EmitSelect()
    {
        onCurrentChanged(currentIndex());
    }

signals:
    void onSelectName(const QString &name);
    void PendingStatusChanged();

private slots:
    void onCurrentChanged(int row)
    {
        if (row >= 0)
        {
            QModelIndex index = model->index(row, NameTableModel::Name);
            QString name = index.data(Qt::EditRole).toString();
            emit onSelectName(name);
        }
        else
            emit onSelectName(QString());
    }

    void dataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
    {
        int row_start = topLeft.row();
        int row_end = bottomRight.row();
        for (int i = row_start; i <= row_end; i++)
        {
            QModelIndex index = model->index(i, NameTableModel::Name);
            setTabText(i, index.data(Qt::DisplayRole).toString());
            setTabData(i, index.data(Qt::EditRole));
            setTabTextColor(i, index.data(Qt::DecorationRole).value<QColor>());
        }
        int cur = currentIndex();
        if (row_start <= cur && cur <= row_end)
            emit PendingStatusChanged();
    }

    void modelReset()
    {
        ClearTabs();
        CreateTabs();
    }

    void rowsInserted(const QModelIndex &parent, int start, int end)
    {
        Q_UNUSED(parent);
        for (int i = start; i <= end; i++)
        {
            QModelIndex index = model->index(i, NameTableModel::Name);
            insertTab(i, index.data(Qt::DisplayRole).toString());
            setTabTextColor(i, index.data(Qt::DecorationRole).value<QColor>());
        }
    }

    void rowsMoved(const QModelIndex &sourceParent, int sourceStart, int sourceEnd, const QModelIndex &destinationParent, int destinationRow)
    {
        throw std::runtime_error("NameTabs::rowsMoved not implemented");
    }

    void rowsRemoved(const QModelIndex &parent, int start, int end)
    {
        Q_UNUSED(parent);
        for (int i = end; i >= start; i--)
            removeTab(i);
    }

private:
    NameTableModel *model;
    
    void ClearTabs()
    {
        for (int i = count(); i >= 0; i--)
            removeTab(i);
    }

    void CreateTabs()
    {
        for (int i = 0, n = model->rowCount(); i < n; i++)
        {
            QModelIndex index = model->index(i, NameTableModel::Name);
            addTab(index.data(Qt::DisplayRole).toString());
            setTabTextColor(i, index.data(Qt::DecorationRole).value<QColor>());
        }
    }
};

#include "managenamespage.moc"

// Prefix that indicates for the user that the player's reward address isn't the one shown
// (currently this column is hidden though)
const QString NON_REWARD_ADDRESS_PREFIX = "-";

//
// ManageNamesPage
//

const static int COLUMN_WIDTH_COINS = 70;
const static int COLUMN_WIDTH_STATE = 50;
const static int COLUMN_WIDTH_TIME = 40;
const static int COLUMN_WIDTH_LIFE = 40;

ManageNamesPage::ManageNamesPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ManageNamesPage),
    model(0),
    walletModel(0),
    characterTableModel(0),
    chrononAnim(0),
    tabsNames(NULL),
    rewardAddrChanged(false)
{
    ui->setupUi(this);

    connect(ui->tableCharacters, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(onClickedCharacter(QModelIndex)));
    ui->tableCharacters->setEditTriggers(QAbstractItemView::NoEditTriggers);

    gameMapView = new GameMapView(this);
    ui->verticalLayoutGameMap->addWidget(gameMapView);
    gameMapView->setAttribute(Qt::WA_NoMousePropagation, true);
    gameMapView->setStatusTip(tr("Left click - make move. Right button - scroll map. Mouse wheel - zoom map. Middle click - reset zoom. Ctrl + +,-,0 - zoom in/out/reset"));

    connect(gameMapView, SIGNAL(tileClicked(int, int, bool)), this, SLOT(onTileClicked(int, int, bool)));

    QShortcut *zoomInKey = new QShortcut(QKeySequence::ZoomIn, this);
    QShortcut *zoomInKey2 = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Equal), this);
    QShortcut *zoomOutKey = new QShortcut(QKeySequence::ZoomOut, this);
    QShortcut *zoomResetKey = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_0), this);
    connect(zoomInKey, SIGNAL(activated()), gameMapView, SLOT(zoomIn()));
    connect(zoomInKey2, SIGNAL(activated()), gameMapView, SLOT(zoomIn()));
    connect(zoomOutKey, SIGNAL(activated()), gameMapView, SLOT(zoomOut()));
    connect(zoomResetKey, SIGNAL(activated()), gameMapView, SLOT(zoomReset()));

    gameChatView = new GameChatView(this);
    connect(gameChatView, SIGNAL(chatUpdated(const QString &)), ui->textChat, SLOT(setHtml(const QString &)));
    connect(gameChatView, SIGNAL(chatScrollToAnchor(const QString &)), ui->textChat, SLOT(scrollToAnchor(const QString &)));

    chrononAnim = new QTimeLine(3000, this);
    chrononAnim->setUpdateInterval(50);
    connect(chrononAnim, SIGNAL(valueChanged(qreal)), SLOT(chrononAnimChanged(qreal)));
    connect(chrononAnim, SIGNAL(finished()), SLOT(chrononAnimFinished()));

    onSelectName(QString());
}

ManageNamesPage::~ManageNamesPage()
{
    delete ui;
    delete characterTableModel;
}

void ManageNamesPage::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
    model = walletModel->getNameTableModel();
    model->updateGameState();

    if (tabsNames)
        delete tabsNames;
    tabsNames = new NameTabs(model, this);
    tabsNames->setDrawBase(false);
    ui->horizontalLayoutTop->insertWidget(0, tabsNames);

    connect(tabsNames, SIGNAL(onSelectName(const QString &)), this, SLOT(onSelectName(const QString &)));
    connect(tabsNames, SIGNAL(PendingStatusChanged()), this, SLOT(PendingStatusChanged()));
    tabsNames->EmitSelect();

    connect(model, SIGNAL(gameStateChanged(const Game::GameState &)), gameChatView, SLOT(updateChat(const Game::GameState &)));
    connect(model, SIGNAL(gameStateChanged(const Game::GameState &)), this, SLOT(updateGameState(const Game::GameState &)));

    ui->tableCharacters->horizontalHeader()->setHighlightSections(false);

    model->emitGameStateChanged();
}

void ManageNamesPage::on_newButton_clicked()
{
    if (!walletModel)
        return;

    QString name;
    for (;;)
    {
        bool ok;
        name = QInputDialog::getText(this, tr("New name"), tr("Player name:"), QLineEdit::Normal, "", &ok, Qt::WindowSystemMenuHint | Qt::WindowTitleHint);
        if (!ok)
            return;

        if (!IsValidPlayerName(name.toStdString()))
            QMessageBox::warning(this, tr("Name registration error"),
                  tr("The entered name is invalid.  Allowed characters: alphanumeric, underscore, hyphen, whitespace (but not at start/end and no double whitespaces).  Maximum length is 10 characters."),
                  QMessageBox::Ok);
        else
            break;
    }

    if (!walletModel->nameAvailable(name))
    {
        QMessageBox::warning(this, tr("Name registration"), tr("Name not available"));
        return;
    }

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;
    
    QString err_msg;

    try
    {
        ConfigureNameDialog1 dlg(name, std::string(), this);
        dlg.setModel(walletModel);
        if (dlg.exec() == QDialog::Accepted)
        {
            LOCK(cs_main);
            int newRowIndex;
            model->updateEntry(name, dlg.getReturnData(), "", NameTableEntry::NAME_NEW, CT_NEW, &newRowIndex);
            tabsNames->setCurrentIndex(newRowIndex);
            onSelectName(name);
        }

        return;
    }
    catch (std::exception& e) 
    {
        err_msg = e.what();
    }

    if (err_msg == "ABORTED")
        return;

    QMessageBox::warning(this, tr("Name registration failed"), err_msg);
}

void ManageNamesPage::on_destructButton_clicked()
{
    if (selectedCharacters.isEmpty())
        return;

    foreach (QString character, selectedCharacters)
    {
        Game::CharacterID chid = Game::CharacterID::Parse(character.toStdString());
        if (chid.player != selectedPlayer.toStdString())
            continue;

        if (chid == gameState.crownHolder)
        {
            QMessageBox::information(this, tr("Self-destruction"),
                  tr("Crown holder cannot self-destruct"),
                  QMessageBox::Ok);
            continue;
        }

        if (!ForkInEffect (FORK_LIFESTEAL, gameState.nHeight + 1))
          queuedMoves[chid.player][chid.index].waypoints.clear ();
        queuedMoves[chid.player][chid.index].destruct = true;
    }    

    UpdateQueuedMoves();
}

void ManageNamesPage::on_goButton_clicked()
{
    if (selectedPlayer.isEmpty())
        return;

    if (!walletModel)
        return;

    json_spirit::Object json;

    QString msg = ui->messageEdit->text();
    if (!msg.isEmpty())
        json.push_back(json_spirit::Pair("msg", msg.toStdString()));

    // Make sure the game state is up-to-date (otherwise it's only polled every 250 ms)
    model->updateGameState();

    std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(selectedPlayer.toStdString());
    if (it == gameState.players.end() || rewardAddr.toStdString() != it->second.address)
        json.push_back(json_spirit::Pair("address", rewardAddr.toStdString()));

    std::string strSelectedPlayer = selectedPlayer.toStdString();
    
    std::map<Game::PlayerID, Game::PlayerState>::const_iterator mi = gameState.players.find(strSelectedPlayer);

    const QueuedPlayerMoves &qpm = queuedMoves[strSelectedPlayer];

    BOOST_FOREACH(const PAIRTYPE(int, QueuedMove)& item, qpm)
    {
        // TODO: this can be extracted as a method QueuedMove::ToJsonValue
        json_spirit::Object obj;
        if (item.second.destruct)
            obj.push_back(json_spirit::Pair("destruct", json_spirit::Value(true)));

        const std::vector<Game::Coord> *p = NULL;
        if (mi != gameState.players.end())
        {
            std::map<int, Game::CharacterState>::const_iterator mi2 = mi->second.characters.find(item.first);
            if (mi2 == mi->second.characters.end())
                continue;
            const Game::CharacterState &ch = mi2->second;

            // Caution: UpdateQueuedPath can modify the array queuedMoves that we are iterating over
            p = UpdateQueuedPath(ch, queuedMoves, Game::CharacterID(strSelectedPlayer, item.first));
        }

        if (!p || p->empty())
            p = &item.second.waypoints;

        if (!p->empty())
          {
            assert (!item.second.destruct
                      || ForkInEffect (FORK_LIFESTEAL, gameState.nHeight + 1));

            json_spirit::Array arr;
            if (p->size() == 1)
            {
                // Single waypoint (which forces character to stop on the spot) is sent as is.
                // It's also possible to send an empty waypoint array for this, but the behavior will differ 
                // if it goes into the chain some blocks later (will stop on the current tile rather than
                // the clicked one).
                arr.push_back((*p)[0].x);
                arr.push_back((*p)[0].y);
            }
            else
                for (size_t i = 1, n = p->size(); i < n; i++)
                {
                    arr.push_back((*p)[i].x);
                    arr.push_back((*p)[i].y);
                }
            obj.push_back(json_spirit::Pair("wp", arr));
          }

        json.push_back(json_spirit::Pair(strprintf("%d", item.first), obj));
    }

    std::string data = json_spirit::write_string(json_spirit::Value(json), false);

    QString err_msg;
    try
    {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());
        if (!ctx.isValid())
            return;

        err_msg = walletModel->nameUpdate(selectedPlayer, data, transferTo);
    }
    catch (std::exception& e) 
    {
        err_msg = e.what();
    }

    if (!err_msg.isEmpty())
    {
        if (err_msg == "ABORTED")
            return;

        printf("Name update error for player %s: %s\n\tMove: %s\n", qPrintable(selectedPlayer), qPrintable(err_msg), data.c_str());
        QMessageBox::critical(this, tr("Name update error"), err_msg);
        return;
    }
    ui->messageEdit->setText(QString());
    transferTo = QString();

    queuedMoves[strSelectedPlayer].clear();

    UpdateQueuedMoves();
    SetPlayerMoveEnabled(false);
    rewardAddrChanged = false;
}

void ManageNamesPage::on_cancelButton_clicked()
{
    if (selectedCharacters.isEmpty())
        return;

    foreach (QString character, selectedCharacters)
    {
        Game::CharacterID chid = Game::CharacterID::Parse(character.toStdString());
        if (chid.player != selectedPlayer.toStdString())
            continue;

        queuedMoves[chid.player].erase(chid.index);
    }

    UpdateQueuedMoves();
}

void ManageNamesPage::UpdateQueuedMoves()
{
    if (characterTableModel)
        characterTableModel->MoveQueueChanged();
    gameMapView->SelectPlayer(selectedPlayer, gameState, queuedMoves);
}

void ManageNamesPage::onTileClicked(int x, int y, bool ctrlPressed)
{
    if (selectedCharacters.isEmpty())
        return;

    Game::Coord target(x, y);
    foreach (QString character, selectedCharacters)
    {
        Game::CharacterID chid = Game::CharacterID::Parse(character.toStdString());
        if (chid.player != selectedPlayer.toStdString())
            continue;

        std::map<Game::PlayerID, Game::PlayerState>::const_iterator mi = gameState.players.find(chid.player);
        if (mi == gameState.players.end())
            continue;

        std::map<int, Game::CharacterState>::const_iterator mi2 = mi->second.characters.find(chid.index);
        if (mi2 == mi->second.characters.end())
            continue;


        QueuedMove& m = queuedMoves[chid.player][chid.index];
        if (m.destruct && !ForkInEffect (FORK_LIFESTEAL, gameState.nHeight + 1))
          continue;

        Game::WaypointVector &cwp = m.waypoints;
        bool appendWP = (ctrlPressed && !cwp.empty());
        Game::Coord start = (appendWP) ? cwp.back() : mi2->second.coord;
        Game::WaypointVector wp = FindPath(start, target);
        
        if (wp.empty()) 
            continue;
            
        if (appendWP)
        {
            assert (wp.front () == cwp.back ());
            cwp.reserve((cwp.size() + wp.size()) - 1);
            cwp.insert(cwp.end(), wp.begin() + 1, wp.end());
        }
        else 
            cwp = wp;
    }
    UpdateQueuedMoves();
}

void ManageNamesPage::onClickedCharacter(const QModelIndex &index)
{
    QString selectedCharacter = index.sibling(index.row(), CharacterTableModel::Name).data(Qt::EditRole).toString();

    Game::CharacterID chid = Game::CharacterID::Parse(selectedCharacter.toStdString());
    
    std::map<Game::PlayerID, Game::PlayerState>::const_iterator mi = gameState.players.find(chid.player);
    if (mi != gameState.players.end())
    {
        std::map<int, Game::CharacterState>::const_iterator mi2 = mi->second.characters.find(chid.index);
        if (mi2 != mi->second.characters.end())
            gameMapView->CenterMapOnCharacter(mi2->second);
    }
}

void ManageNamesPage::RefreshCharacterList()
{
    if (characterTableModel)
        characterTableModel->deleteLater();

    std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(selectedPlayer.toStdString());
    if (it != gameState.players.end())
    {
        // Note: pointer to queuedMoves is saved and must stay valid while the character table is visible
        characterTableModel = new CharacterTableModel(it->first, it->second, queuedMoves[it->first], gameState.crownHolder);
    }
    else
        characterTableModel = NULL;

    // Delete old selection model, because setModel creates a new one
    if (ui->tableCharacters->selectionModel())
        ui->tableCharacters->selectionModel()->deleteLater();
    ui->tableCharacters->setModel(characterTableModel);

    if (!characterTableModel)
    {
        gameMapView->DeselectPlayer();
        return;
    }

    connect(ui->tableCharacters->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this, SLOT(onCharacterSelectionChanged(QItemSelection, QItemSelection)));

    // Set column widths
    ui->tableCharacters->horizontalHeader()->resizeSection(CharacterTableModel::Coins, COLUMN_WIDTH_COINS);
    ui->tableCharacters->horizontalHeader()->resizeSection(CharacterTableModel::Status, COLUMN_WIDTH_STATE);
    ui->tableCharacters->horizontalHeader()->resizeSection(CharacterTableModel::Time, COLUMN_WIDTH_TIME);
    ui->tableCharacters->horizontalHeader()->resizeSection(CharacterTableModel::Life, COLUMN_WIDTH_LIFE);
    ui->tableCharacters->horizontalHeader()->setResizeMode(CharacterTableModel::Name, QHeaderView::Stretch);
    ui->tableCharacters->horizontalHeader()->setResizeMode(CharacterTableModel::Coins, QHeaderView::Fixed);
    ui->tableCharacters->horizontalHeader()->setResizeMode(CharacterTableModel::Status, QHeaderView::Fixed);
    ui->tableCharacters->horizontalHeader()->setResizeMode(CharacterTableModel::Time, QHeaderView::Fixed);
    ui->tableCharacters->horizontalHeader()->setResizeMode(CharacterTableModel::Life, QHeaderView::Fixed);
    QItemSelection sel;
    foreach (QString character, selectedCharacters)
    {
        int row = characterTableModel->FindRow(character);
        if (row >= 0)
            sel.select(characterTableModel->index(row, 0), characterTableModel->index(row, 1));
    }
    ui->tableCharacters->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);

    gameMapView->SelectPlayer(selectedPlayer, gameState, queuedMoves);
}

void ManageNamesPage::SetPlayerEnabled(bool enable)
{
    ui->tableCharacters->setEnabled(enable);
    ui->configButton->setEnabled(enable);
    ui->destructButton->setEnabled(enable);
    ui->cancelButton->setEnabled(enable);
    ui->messageEdit->setEnabled(enable);

    if (model)
        model->updateGameState();
    SetPlayerMoveEnabled(enable);
}

void ManageNamesPage::SetPlayerMoveEnabled(bool enable /* = true */)
{
    if (!model)
        enable = false;

    if (enable)
    {
        // If there are pending operations on the name, disable buttons
        int row = tabsNames->currentIndex();
        if (row >= 0)
            enable = model->index(row, NameTableModel::Status).data(Qt::CheckStateRole).toBool();
        else
            enable = false;
    }

    ui->goButton->setEnabled(enable);
    if (characterTableModel)
        characterTableModel->SetPending(!enable);
}

void ManageNamesPage::PendingStatusChanged()
{
    if (!selectedPlayer.isEmpty())
        SetPlayerEnabled(true);
}

void ManageNamesPage::onSelectName(const QString &name)
{
    selectedPlayer = name;
        
    rewardAddrChanged = false;

    if (selectedPlayer.isEmpty())
    {
        selectedPlayer = QString();
        transferTo = QString();
        rewardAddr = QString();
        rewardAddrChanged = false;
        ui->messageEdit->setText(QString());
        SetPlayerEnabled(false);
        return;
    }

    transferTo = QString();
    ui->messageEdit->setText(QString());

    std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(selectedPlayer.toStdString());
    if (it != gameState.players.end())
        rewardAddr = QString::fromStdString(it->second.address);
    else
        rewardAddr = QString();

    selectedCharacters.clear();
    RefreshCharacterList();
    SetPlayerEnabled(true);
}

void ManageNamesPage::onCharacterSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    foreach (QModelIndex index, deselected.indexes())
    {
        QString deselectedCharacter = index.sibling(index.row(), CharacterTableModel::Name).data(Qt::EditRole).toString();
        selectedCharacters.removeAll(deselectedCharacter);
    }

    foreach (QModelIndex index, selected.indexes())
    {
        QString selectedCharacter = index.sibling(index.row(), CharacterTableModel::Name).data(Qt::EditRole).toString();
        if (selectedCharacters.contains(selectedCharacter))
            continue;

        selectedCharacters.append(selectedCharacter);
    }
}

void ManageNamesPage::updateGameState(const Game::GameState &gameState)
{
    chrononAnim->stop();

    this->gameState = gameState;

    // Update reward address from the game state, unless it was explicitly changed by the user and not yet committed (via Go button)
    if (!selectedPlayer.isEmpty() && !rewardAddrChanged)
    {
        std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(selectedPlayer.toStdString());
        if (it != gameState.players.end())
            rewardAddr = QString::fromStdString(it->second.address);
        else
            rewardAddr = QString();
    }

    chrononAnim->start();
    gameMapView->updateGameMap(gameState);
    RefreshCharacterList();
    SetPlayerMoveEnabled();
}

void ManageNamesPage::chrononAnimChanged(qreal t)
{
    const static qreal FADE_IN = qreal(0.35);
    if (t <= FADE_IN)
        t /= FADE_IN;
    else
        t = 1 - (t - FADE_IN) / (1 - FADE_IN);
    QColor c(int(t * 255), 0, 0);

    ui->chrononLabel->setText(
            "<font color='" + c.name() + "'><b>"
            + tr("Chronon: %1").arg(gameState.nHeight)
            + QString("</b></font>")
        );
}

void ManageNamesPage::chrononAnimFinished()
{
    ui->chrononLabel->setText(tr("Chronon: %1").arg(gameState.nHeight));
}

void ManageNamesPage::on_configButton_clicked()
{
    int row = tabsNames->currentIndex();
    if (row < 0)
        return;

    QString name = model->index(row, NameTableModel::Name).data(Qt::EditRole).toString();
    QString address = model->index(row, NameTableModel::Address).data(Qt::EditRole).toString();
    QByteArray raw_value = model->index(row, NameTableModel::Value).data(NameTableModel::RawStringData).toByteArray();
    std::string value(raw_value.constData(), raw_value.length());

    if (address.startsWith(NON_REWARD_ADDRESS_PREFIX))
        address = address.mid(NON_REWARD_ADDRESS_PREFIX.size());

    std::vector<unsigned char> vchName = vchFromString(name.toStdString());

    {
        LOCK(cs_main);
        ConfigureNameDialog2 dlg(name, address, rewardAddr, transferTo, this);
        dlg.setModel(walletModel);
        if (dlg.exec() == QDialog::Accepted)
        {
            rewardAddr = dlg.getRewardAddr();
            rewardAddrChanged = true;
            transferTo = dlg.getTransferTo();
            QMessageBox::information(this, tr("Name configured"), tr("To apply changes, you need to press Go button"));
        }
    }
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

    writer.setModel(model);
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
