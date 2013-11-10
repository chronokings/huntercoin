#ifndef NAMETABLEMODEL_H
#define NAMETABLEMODEL_H

#include <QAbstractTableModel>
#include <QStringList>

class NameTablePriv;
class CWallet;
class WalletModel;

extern const QString NON_REWARD_ADDRESS_PREFIX;

namespace Game
{
    class GameState;
}

/**
   Qt model for "Manage Names" page.
 */
class NameTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit NameTableModel(CWallet *wallet, WalletModel *parent = 0);
    ~NameTableModel();

    enum ColumnIndex {
        Name = 0,
        Value = 1,
        Address = 2,
        State = 3,
        Status = 4
    };

    /** @name Methods overridden from QAbstractTableModel
        @{*/
    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    int columnCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const;
    Qt::ItemFlags flags(const QModelIndex &index) const;
    /*@}*/

    // Ensures that all listeners update game state to then one returned by GetCurrentGameState()
    void emitGameStateChanged();

private:
    WalletModel *walletModel;
    CWallet *wallet;
    QStringList columns;
    NameTablePriv *priv;
    int cachedNumBlocks;

    /** Notify listeners that data changed. */
    void emitDataChanged(int index);

signals:
    void gameStateChanged(const Game::GameState &gameState);

public slots:
    void updateEntry(const QString &name, const QString &value, const QString &address, int nHeight, int status, int *outNewRowIndex = NULL);
    void updateGameState();
    void updateTransaction(const QString &hash, int status);
    friend class NameTablePriv;
};

struct NameTableEntry
{
    QString name;
    QString value;
    QString address;
    bool fRewardAddressDifferent;
    QString state;
    bool moving;
    int color;
    int nHeight;
    bool transferred;

    static const int NAME_NEW = -1;             // Dummy nHeight value for not-yet-created names
    static const int NAME_NEW_POSTPONED = -2;   // Same as above, but "OK" was not pressed in the configure dialog
    static const int NAME_NON_EXISTING = -3;    // Dummy nHeight value for unitinialized entries
    static const int NAME_UNCONFIRMED = -4;     // Dummy nHeight value for unconfirmed name transactions

    bool HeightValid() { return nHeight >= 0; }
    static bool CompareHeight(int nOldHeight, int nNewHeight);    // Returns true if new height is better

    NameTableEntry() : nHeight(NAME_NON_EXISTING), transferred(false) {}
    NameTableEntry(const QString &name, const QString &value, const QString &address, int nHeight, bool transferred = false) :
        name(name), value(value), address(address),
        fRewardAddressDifferent(false), nHeight(nHeight), transferred(transferred), color(-1), moving(false)
    {
    }

    NameTableEntry(const std::string &name, const std::string &value, const std::string &address, int nHeight, bool transferred = false) :
        name(QString::fromStdString(name)), value(QString::fromStdString(value)), address(QString::fromStdString(address)),
        fRewardAddressDifferent(false), nHeight(nHeight), transferred(transferred), color(-1), moving(false)
    {
    }
};

#endif // NAMETABLEMODEL_H
