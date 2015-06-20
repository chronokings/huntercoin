#include "nametablemodel.h"

#include "guiutil.h"
#include "walletmodel.h"
#include "guiconstants.h"

#include "../headers.h"
#include "../huntercoin.h"
#include "../gamedb.h"
#include "../gamestate.h"
#include "../gametx.h"
#include "ui_interface.h"

#include "gamechatview.h" // For ColorCSS

#include "../json/json_spirit_writer_template.h"

#include <QTimer>

struct NameTableEntryLessThan
{
    bool operator()(const NameTableEntry &a, const NameTableEntry &b) const
    {
        return a.name < b.name;
    }
    bool operator()(const NameTableEntry &a, const QString &b) const
    {
        return a.name < b;
    }
    bool operator()(const QString &a, const NameTableEntry &b) const
    {
        return a < b.name;
    }
};

// Returns true if new height is better
/*static*/ bool NameTableEntry::CompareHeight(int nOldHeight, int nNewHeight)
{
    if (nOldHeight == NAME_NON_EXISTING)
        return true;

    // We use optimistic way, assuming that unconfirmed transaction will eventually become confirmed,
    // so we update the name in the table immediately. Ideally we need a separate way of displaying
    // unconfirmed names (e.g. grayed out)
    if (nNewHeight == NAME_UNCONFIRMED)
        return true;
    else if (nOldHeight == NAME_UNCONFIRMED)
        return false;

    // Here we rely on the fact that dummy height values are always negative
    return nNewHeight > nOldHeight;
}

// Private implementation
class NameTablePriv
{
private:

    /* Internal implementation matching "name_list" that returns
       a list of NameTableEntry's.  This is used both in
       refreshNameTable and in refreshName.  */
    void listNames (std::map<vchType, NameTableEntry>& names,
                    const vchType* nameFilter = NULL) const;

public:
    CWallet *wallet;
    QList<NameTableEntry> cachedNameTable;
    NameTableModel *parent;
    uint256 cachedLastBlock;

    NameTablePriv(CWallet *wallet, NameTableModel *parent):
        wallet(wallet), parent(parent), cachedLastBlock(0)
    {
    }

    void refreshNameTable()
    {
        cachedNameTable.clear();

        std::map<vchType, NameTableEntry> vNamesO;
        listNames (vNamesO);

        // Add existing names
        BOOST_FOREACH(const PAIRTYPE(vchType, NameTableEntry)& item, vNamesO)
          {
            if (!item.second.transferred)
              cachedNameTable.append (item.second);
          }

        // qLowerBound() and qUpperBound() require our cachedNameTable list to be sorted in asc order
        qSort(cachedNameTable.begin(), cachedNameTable.end(), NameTableEntryLessThan());
    }

    void
    refreshName (const vchType& inName)
    {
        NameTableEntry nameObj;

        std::map<vchType, NameTableEntry> names;
        listNames (names, &inName);

        std::map<vchType, NameTableEntry>::const_iterator pos;
        pos = names.find (inName);
        if (pos == names.end ())
          nameObj = NameTableEntry(stringFromVch (inName),
                                   std::string(), std::string(),
                                   NameTableEntry::NAME_NON_EXISTING);
        else
          nameObj = pos->second;

        // Transferred name is not ours anymore - remove it from the table
        if (nameObj.transferred)
            nameObj.nHeight = NameTableEntry::NAME_NON_EXISTING;

        // Find name in model
        QList<NameTableEntry>::iterator lower = qLowerBound(
            cachedNameTable.begin(), cachedNameTable.end(), nameObj.name, NameTableEntryLessThan());
        QList<NameTableEntry>::iterator upper = qUpperBound(
            cachedNameTable.begin(), cachedNameTable.end(), nameObj.name, NameTableEntryLessThan());
        bool inModel = (lower != upper);

        if (inModel)
        {
            // In model - update or delete

            if (nameObj.nHeight != NameTableEntry::NAME_NON_EXISTING)
                updateEntry(nameObj, CT_UPDATED);
            else
                updateEntry(nameObj, CT_DELETED);
        }
        else
        {
            // Not in model - add or do nothing

            if (nameObj.nHeight != NameTableEntry::NAME_NON_EXISTING)
                updateEntry(nameObj, CT_NEW);
        }
    }

    void updateEntry(const NameTableEntry &nameObj, int status, int *outNewRowIndex = NULL)
    {
        updateEntry(nameObj.name, nameObj.value, nameObj.address, nameObj.nHeight, status, outNewRowIndex);
    }

    void updateEntry(const QString &name, const std::string &value, const QString &address, int nHeight, int status, int *outNewRowIndex = NULL)
    {
        // Find name in model
        QList<NameTableEntry>::iterator lower = qLowerBound(
            cachedNameTable.begin(), cachedNameTable.end(), name, NameTableEntryLessThan());
        QList<NameTableEntry>::iterator upper = qUpperBound(
            cachedNameTable.begin(), cachedNameTable.end(), name, NameTableEntryLessThan());
        int lowerIndex = (lower - cachedNameTable.begin());
        int upperIndex = (upper - cachedNameTable.begin());
        bool inModel = (lower != upper);

        switch(status)
        {
        case CT_NEW:
            if (inModel)
            {
                if (outNewRowIndex)
                {
                    *outNewRowIndex = parent->index(lowerIndex, 0).row();
                    // HACK: ManageNamesPage uses this to ensure updating and get selected row,
                    // so we do not write warning into the log in this case
                }
                else
                    OutputDebugStringF("Warning: NameTablePriv::updateEntry: Got CT_NOW, but entry is already in model\n");
                break;
            }
            parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex);
            cachedNameTable.insert(lowerIndex, NameTableEntry(name, value, address, nHeight));
            parent->endInsertRows();
            if (outNewRowIndex)
                *outNewRowIndex = parent->index(lowerIndex, 0).row();
            break;
        case CT_UPDATED:
            if (!inModel)
            {
                OutputDebugStringF("Warning: NameTablePriv::updateEntry: Got CT_UPDATED, but entry is not in model\n");
                break;
            }
            lower->name = name;
            lower->value = value;
            lower->address = address;
            lower->nHeight = nHeight;
            parent->emitDataChanged(lowerIndex);
            break;
        case CT_DELETED:
            if (!inModel)
            {
                OutputDebugStringF("Warning: NameTablePriv::updateEntry: Got CT_DELETED, but entry is not in model\n");
                break;
            }
            parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex - 1);
            cachedNameTable.erase(lower, upper);
            parent->endRemoveRows();
            break;
        }
    }

    bool updateGameState(bool &fRewardAddrChanged)
    {
        CTryCriticalBlock criticalBlock(cs_main);
        if (!criticalBlock.Entered())
            return false;

        const Game::GameState &gameState = GetCurrentGameState();
        if (gameState.hashBlock == cachedLastBlock)
            return false;

        bool fChanged = false;
        for (int i = 0, n = size(); i < n; i++)
        {
            NameTableEntry *item = index(i);
            QString s;

            if (item->HeightValid() || item->nHeight == NameTableEntry::NAME_UNCONFIRMED)
            {
                std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(item->name.toStdString());
                if (it != gameState.players.end())
                {
                    bool fRewardAddressDifferent = !it->second.address.empty() && item->address != it->second.address.c_str();

                    if (item->fRewardAddressDifferent != fRewardAddressDifferent)
                    {
                        item->fRewardAddressDifferent = fRewardAddressDifferent;
                        fRewardAddrChanged = true;
                    }

                    // Note: we do not provide crown_index here, so the JSON string won't contain has_crown field
                    s = QString::fromStdString(json_spirit::write_string(it->second.ToJsonValue(-1), false));
                }
            }

            if (item->state != s)
            {
                std::map<Game::PlayerID, Game::PlayerState>::const_iterator it = gameState.players.find(item->name.toStdString());
                if (it != gameState.players.end())
                    item->color = it->second.color;

                item->state = s;
                fChanged = true;
            }
        }

        cachedLastBlock = gameState.hashBlock;
        emit parent->gameStateChanged(gameState);

        return fChanged;
    }

    int size()
    {
        return cachedNameTable.size();
    }

    NameTableEntry *index(int idx)
    {
        if (idx >= 0 && idx < cachedNameTable.size())
        {
            return &cachedNameTable[idx];
        }
        else
        {
            return NULL;
        }
    }
};

void
NameTablePriv::listNames (std::map<vchType, NameTableEntry>& names,
                          const vchType* nameFilter) const
{
  names.clear ();

  /* Game transactions are not ordinarily processed, but we keep track
     of the latest height at which a name is killed.  This is used
     to set those names' "dead" flag in the end.  */
  std::map<vchType, int> killedAt;
  
  CRITICAL_BLOCK(cs_main)
  CRITICAL_BLOCK(wallet->cs_mapWallet)
    {
      CTxDB txdb("r");

      BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, wallet->mapWallet)
        {
          const CWalletTx& tx = item.second;
          vchType vchName;

          int nHeight = tx.GetHeightInMainChain ();
          const bool fConfirmed = (nHeight != -1);
          if (fConfirmed)
            assert (nHeight >= 0);
          else
            nHeight = NameTableEntry::NAME_UNCONFIRMED;

          /* Process game transactions to fill in the killedAt array.  Ignore
             unconfirmed ones, since they belong to orphaned blocks.  */
          if (tx.IsGameTx ())
            {
              if (fConfirmed)
                BOOST_FOREACH(const CTxIn& txi, tx.vin)
                  if (IsPlayerDeathInput (txi, vchName))
                    {
                      if (nameFilter && vchName != *nameFilter)
                        continue;

                      std::map<vchType, int>::iterator pos;
                      pos = killedAt.find (vchName);
                      if (pos == killedAt.end ())
                        killedAt.insert (std::make_pair (vchName, nHeight));
                      else if (pos->second < nHeight)
                        pos->second = nHeight;
                    }

              continue;
            }

          vchType vchValue;
          int nOut;
          if (!tx.GetNameUpdate (nOut, vchName, vchValue))
            continue;

          if (nameFilter && vchName != *nameFilter)
            continue;

          /* Get last active name only.  */
          std::map<vchType, NameTableEntry>::iterator mi;
          mi = names.find (vchName);
          if (mi != names.end ()
              && !NameTableEntry::CompareHeight (mi->second.nHeight,
                                                 nHeight))
            continue;

          const bool fTransferred = !hooks->IsMine (tx);
          /* Dead players are also considered transferred for our
             purposes, but this is done later on using killedAt.  */

          std::string strAddress = "";
          GetNameAddress(tx, strAddress);

          NameTableEntry entry(stringFromVch (vchName),
                               stringFromVch (vchValue),
                               strAddress, nHeight, fTransferred);
          names[vchName] = entry;
        }
    }

  /* Set killed players to "transferred".  */
  for (std::map<vchType, NameTableEntry>::iterator i = names.begin ();
       i != names.end (); ++i)
    {
      if (i->second.transferred)
        continue;

      std::map<vchType, int>::const_iterator pos = killedAt.find (i->first);
      if (pos != killedAt.end ()
          && !NameTableEntry::CompareHeight (pos->second,
                                             i->second.nHeight))
        i->second.transferred = true;
    }
}

NameTableModel::NameTableModel(CWallet *wallet, WalletModel *parent) :
    QAbstractTableModel(parent), walletModel(parent), wallet(wallet), priv(0), cachedNumBlocks(0)
{
    columns << tr("Name") << tr("Last Move") << tr("Address") << tr("State") << tr("Status");
    priv = new NameTablePriv(wallet, this);
    priv->refreshNameTable();

    QTimer *timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateGameState()));
    timer->start(MODEL_UPDATE_DELAY);
}

NameTableModel::~NameTableModel()
{
    delete priv;
}

void NameTableModel::updateGameState()
{
    bool fRewardAddrChanged = false;

    // If any item changed (State or Address), we refresh the entire column
    if (priv->updateGameState(fRewardAddrChanged))
        emit dataChanged(index(0, State), index(priv->size() - 1, State));
    if (fRewardAddrChanged)
        emit dataChanged(index(0, Address), index(priv->size() - 1, Address));
}

// Caution: this won't work with deleted transactions as it's impossible to get name from them.
// You must save name before deletion, then call refreshName instead.
//
void NameTableModel::updateTransaction(const QString &hash, int status)
{
    uint256 hash256;
    hash256.SetHex(hash.toStdString());

    CTransaction tx;
    std::vector<unsigned char> vchName;
    
    {
        LOCK(wallet->cs_wallet);

        // Find transaction in wallet
        std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(hash256);
        if (mi == wallet->mapWallet.end())
            return;    // Not our transaction
        tx = mi->second;
    }

    if (tx.IsGameTx())
    {
        BOOST_FOREACH(const CTxIn& txin, tx.vin)
        {
            CTransaction txPrev;
            uint256 hashBlock = 0;
            if (GetTransaction(txin.prevout.hash, txPrev, hashBlock) &&
                    hooks->IsMine(txPrev) &&
                    GetNameOfTx(txPrev, vchName))
            {
                priv->refreshName(vchName);
            }
        }

        return;
    }

    if (!GetNameOfTx(tx, vchName))
        return;   // Non-name transaction

    priv->refreshName(vchName);
}

// Alternative for updateTransaction that accepts a name instead of tx-hash
void NameTableModel::refreshName(const std::vector<unsigned char> &vchName)
{
    priv->refreshName(vchName);
}

int NameTableModel::rowCount(const QModelIndex &parent /* = QModelIndex()*/) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int NameTableModel::columnCount(const QModelIndex &parent /* = QModelIndex()*/) const
{
    Q_UNUSED(parent);
    return columns.length();
}

QVariant NameTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    NameTableEntry *rec = static_cast<NameTableEntry*>(index.internalPointer());

    if (role == Qt::DisplayRole || role == Qt::EditRole)
    {
        switch (index.column())
        {
        case Name:
            return rec->name;
        case Value:
            if (rec->nHeight == NameTableEntry::NAME_NEW_POSTPONED)
                return QString("");
            return QString::fromStdString(rec->value);
        case Address:
            if (rec->fRewardAddressDifferent)
                return NON_REWARD_ADDRESS_PREFIX + rec->address;
            return rec->address;
        case State:
            return rec->state;
        case Status:
            if (!rec->HeightValid())
            {
                if (rec->nHeight == NameTableEntry::NAME_NEW)
                    return tr("Pending (new)");
                else if (rec->nHeight == NameTableEntry::NAME_NEW_POSTPONED)
                    return tr("Not configured");
                return tr("Pending (update)");
            }
            return tr("OK");
        }
    }
    else if (role == RawStringData)
    {
        if (index.column() == Value)
            return QByteArray(rec->value.c_str(), rec->value.size());
    }
    else if (role == Qt::TextAlignmentRole)
        return QVariant(int(Qt::AlignLeft|Qt::AlignVCenter));
    else if (role == Qt::FontRole)
    {
        QFont font;
        if (index.column() == Address)
            font = GUIUtil::bitcoinAddressFont();
        return font;
    }
    else if (role == Qt::DecorationRole)
    {
        if (index.column() == Name && rec->color >= 0)
        {
            QString colorCSS = GameChatView::ColorCSS[rec->color];
            return QColor(colorCSS);
        }
    }
    else if (role == Qt::CheckStateRole && index.column() == Status)
    {
        // Return status (OK or pending) as boolean variable to enable/disable Go button
        return rec->HeightValid();
    }

    return QVariant();
}

QVariant NameTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal)
    {
        if (role == Qt::DisplayRole)
            return columns[section];
        else if (role == Qt::TextAlignmentRole)
            return QVariant(int(Qt::AlignLeft|Qt::AlignVCenter));
        else if (role == Qt::ToolTipRole)
        {
            switch(section)
            {
            case Name:
                return tr("Player name for Huntercoin");
            case Value:
                return tr("Last move of the player");
            case Address:
                return tr("Huntercoin address to which the name is registered.\n\nNote: rewards can go to another address, if specified in the player profile");
            case State:
                return tr("State of the player in the game");
            case Status:
                return tr("Status of the latest move transaction for this player");
            }
        }
    }
    return QVariant();
}

Qt::ItemFlags NameTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return 0;

    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

QModelIndex NameTableModel::index(int row, int column, const QModelIndex &parent /* = QModelIndex()*/) const
{
    Q_UNUSED(parent);
    NameTableEntry *data = priv->index(row);
    if (data)
    {
        return createIndex(row, column, priv->index(row));
    }
    else
    {
        return QModelIndex();
    }
}

void NameTableModel::updateEntry(const QString &name, const std::string &value, const QString &address, int nHeight, int status, int *outNewRowIndex /* = NULL*/)
{
    priv->updateEntry(name, value, address, nHeight, status, outNewRowIndex);
}

void NameTableModel::emitDataChanged(int idx)
{
    emit dataChanged(index(idx, 0), index(idx, columns.length() - 1));
}

void NameTableModel::emitGameStateChanged()
{
    LOCK(cs_main);
    emit gameStateChanged(GetCurrentGameState());
}
