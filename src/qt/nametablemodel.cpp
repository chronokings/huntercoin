#include "nametablemodel.h"

#include "guiutil.h"
#include "walletmodel.h"
#include "guiconstants.h"

#include "../headers.h"
#include "../chronokings.h"
#include "../gamedb.h"
#include "../gamestate.h"
#include "ui_interface.h"

#include "../json/json_spirit_writer_template.h"

#include <QTimer>

extern std::map<std::vector<unsigned char>, PreparedNameFirstUpdate> mapMyNameFirstUpdate;

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

    // Here we rely on the fact that dummy height values are always negative
    return nNewHeight > nOldHeight;
}

// Private implementation
class NameTablePriv
{
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

        std::map< std::vector<unsigned char>, NameTableEntry > vNamesO;

        CRITICAL_BLOCK(cs_main)
        CRITICAL_BLOCK(wallet->cs_mapWallet)
        {
            CTxIndex txindex;
            uint256 hash;
            CTxDB txdb("r");
            CTransaction tx;

            std::vector<unsigned char> vchName;
            std::vector<unsigned char> vchValue;
            int nHeight;

            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, wallet->mapWallet)
            {
                hash = item.second.GetHash();
                bool fConfirmed;
                bool fTransferred = false;
                // TODO: Maybe CMerkleTx::GetDepthInMainChain() would be faster?
                if (!txdb.ReadDiskTx(hash, tx, txindex))
                {
                    tx = item.second;
                    fConfirmed = false;
                }
                else
                    fConfirmed = true;

                if (tx.nVersion != NAMECOIN_TX_VERSION)
                    continue;

                // name
                if (!GetNameOfTx(tx, vchName))
                    continue;

                // value
                if (!GetValueOfNameTx(tx, vchValue))
                    continue;

                if (!hooks->IsMine(wallet->mapWallet[tx.GetHash()]))
                    fTransferred = true;

                // height
                if (fConfirmed)
                    nHeight = GetTxPosHeight(txindex.pos);
                else
                    nHeight = NameTableEntry::NAME_UNCONFIRMED;

                // get last active name only
                std::map< std::vector<unsigned char>, NameTableEntry >::iterator mi = vNamesO.find(vchName);
                if (mi != vNamesO.end() && !NameTableEntry::CompareHeight(mi->second.nHeight, nHeight))
                    continue;

                std::string strAddress = "";
                GetNameAddress(tx, strAddress);

                vNamesO[vchName] = NameTableEntry(stringFromVch(vchName), stringFromVch(vchValue), strAddress, nHeight, fTransferred);
            }
        }        

        // Add existing names
        BOOST_FOREACH(const PAIRTYPE(std::vector<unsigned char>, NameTableEntry)& item, vNamesO)
            if (!item.second.transferred)
                cachedNameTable.append(item.second);

        // Add pending names (name_new)
        BOOST_FOREACH(const PAIRTYPE(std::vector<unsigned char>, PreparedNameFirstUpdate)& item, mapMyNameFirstUpdate)
        {
            std::string strAddress = "";
            GetNameAddress(item.second.wtx, strAddress);
            int nDummyHeight = NameTableEntry::NAME_NEW;
            if (item.second.fPostponed)
                nDummyHeight = NameTableEntry::NAME_NEW_POSTPONED;
            cachedNameTable.append(NameTableEntry(stringFromVch(item.first), stringFromVch(item.second.vchData), strAddress, nDummyHeight));
        }

        // qLowerBound() and qUpperBound() require our cachedNameTable list to be sorted in asc order
        qSort(cachedNameTable.begin(), cachedNameTable.end(), NameTableEntryLessThan());
    }

    void refreshName(const std::vector<unsigned char> &inName)
    {
        NameTableEntry nameObj(stringFromVch(inName), std::string(), std::string(), NameTableEntry::NAME_NON_EXISTING);

        CRITICAL_BLOCK(cs_main)
        CRITICAL_BLOCK(wallet->cs_mapWallet)
        {
            CTxIndex txindex;
            uint256 hash;
            CTxDB txdb("r");
            CTransaction tx;

            std::vector<unsigned char> vchName;
            std::vector<unsigned char> vchValue;
            int nHeight;

            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, wallet->mapWallet)
            {
                hash = item.second.GetHash();
                bool fConfirmed;
                bool fTransferred = false;

                if (!txdb.ReadDiskTx(hash, tx, txindex))
                {
                    tx = item.second;
                    fConfirmed = false;
                }
                else
                    fConfirmed = true;

                if (tx.nVersion != NAMECOIN_TX_VERSION)
                    continue;

                // name
                if (!GetNameOfTx(tx, vchName) || vchName != inName)
                    continue;

                // value
                if (!GetValueOfNameTx(tx, vchValue))
                    continue;

                if (!hooks->IsMine(wallet->mapWallet[tx.GetHash()]))
                    fTransferred = true;

                // height
                if (fConfirmed)
                    nHeight = GetTxPosHeight(txindex.pos);
                else
                    nHeight = NameTableEntry::NAME_UNCONFIRMED;

                // get last active name only
                if (!NameTableEntry::CompareHeight(nameObj.nHeight, nHeight))
                    continue;

                std::string strAddress = "";
                GetNameAddress(tx, strAddress);

                nameObj.value = QString::fromStdString(stringFromVch(vchValue));
                nameObj.address = QString::fromStdString(strAddress);
                nameObj.nHeight = nHeight;
                nameObj.transferred = fTransferred;
            }
        }

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

    void updateEntry(const QString &name, const QString &value, const QString &address, int nHeight, int status, int *outNewRowIndex = NULL)
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
        LOCK(cs_main);

        const Game::GameState &gameState = GetCurrentGameState();
        if (gameState.hashBlock == cachedLastBlock)
            return false;

        emit parent->gameStateChanged(gameState);

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

                    s = QString::fromStdString(json_spirit::write_string(it->second.ToJsonValue(), false));
                }
            }

            if (item->state != s)
            {
                item->state = s;
                fChanged = true;
            }
        }

        cachedLastBlock = gameState.hashBlock;
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

        if (tx.nVersion == GAME_TX_VERSION)
        {
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
                if (wallet->IsMine(txin))
                {
                    CTransaction txPrev;
                    uint256 hashPrev = 0;
                    if (GetTransaction(txin.prevout.hash, txPrev, hashPrev) &&
                            txin.prevout.n < txPrev.vout.size() &&
                            GetNameOfTx(tx, vchName))
                        priv->refreshName(vchName);
               }

            return;
        }
    }

    if (!GetNameOfTx(tx, vchName))
        return;   // Non-name transaction

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
        switch(index.column())
        {
        case Name:
            return rec->name;
        case Value:
            if (rec->nHeight == NameTableEntry::NAME_NEW_POSTPONED)
                return QString("");
            return rec->value;
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
                    return QString("Pending (new)");
                else if (rec->nHeight == NameTableEntry::NAME_NEW_POSTPONED)
                    return QString("Not configured");
                return QString("Pending (update)");
            }
            else
                return QString("OK");
        }
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
                return tr("Player name for Chrono Kings");
            case Value:
                return tr("Last move of the player");
            case Address:
                return tr("Chrono Kings address to which the name is registered.\n\nNote: rewards can go to another address, if specified in the player profile");
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
    //NameTableEntry *rec = static_cast<NameTableEntry*>(index.internalPointer());

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

void NameTableModel::updateEntry(const QString &name, const QString &value, const QString &address, int nHeight, int status, int *outNewRowIndex /* = NULL*/)
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
