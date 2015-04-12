#include "walletmodel.h"
#include "guiconstants.h"
#include "optionsmodel.h"
#include "addresstablemodel.h"
#include "nametablemodel.h"
#include "transactiontablemodel.h"

#include "../headers.h"
#include "../wallet.h"
#include "../base58.h"
#include "../huntercoin.h"
#include "ui_interface.h"

#include <QSet>
#include <QTimer>
#include <QDateTime>

std::map<uint160, vchType> mapMyNameHashes;

WalletModel::WalletModel(CWallet *wallet, OptionsModel *optionsModel, QObject *parent) :
    QObject(parent), wallet(wallet), optionsModel(optionsModel), addressTableModel(0),
    nameTableModel(0), transactionTableModel(0),
    cachedBalance(0), cachedUnconfirmedBalance(0), cachedImmatureBalance(0),
    cachedNumTransactions(0),
    cachedEncryptionStatus(Unencrypted),
    cachedNumBlocks(0)
{
    addressTableModel = new AddressTableModel(wallet, this);
    nameTableModel = new NameTableModel(wallet, this);
    transactionTableModel = new TransactionTableModel(wallet, this);

    // This timer will be fired repeatedly to update the balance
    pollTimer = new QTimer(this);
    connect(pollTimer, SIGNAL(timeout()), this, SLOT(pollBalanceChanged()));
    pollTimer->start(MODEL_UPDATE_DELAY);

    subscribeToCoreSignals();
}

WalletModel::~WalletModel()
{
    unsubscribeFromCoreSignals();
}

qint64 WalletModel::getBalance() const
{
    return wallet->GetBalance();
}

qint64 WalletModel::getUnconfirmedBalance() const
{
    return wallet->GetUnconfirmedBalance();
}

qint64 WalletModel::getImmatureBalance() const
{
    return wallet->GetImmatureBalance();
}

int WalletModel::getNumTransactions() const
{
    int numTransactions = 0;
    {
        LOCK(wallet->cs_wallet);
        numTransactions = wallet->mapWallet.size();
    }
    return numTransactions;
}

void WalletModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if(cachedEncryptionStatus != newEncryptionStatus)
        emit encryptionStatusChanged(newEncryptionStatus);
}

void WalletModel::pollBalanceChanged()
{
    if (nBestHeight != cachedNumBlocks)
    {
        // Balance and number of transactions might have changed
        cachedNumBlocks = nBestHeight;

        checkBalanceChanged();
    }
}

void WalletModel::checkBalanceChanged()
{
    qint64 newBalance = getBalance();
    qint64 newUnconfirmedBalance = getUnconfirmedBalance();
    qint64 newImmatureBalance = getImmatureBalance();

    if(cachedBalance != newBalance || cachedUnconfirmedBalance != newUnconfirmedBalance || cachedImmatureBalance != newImmatureBalance)
    {
        cachedBalance = newBalance;
        cachedUnconfirmedBalance = newUnconfirmedBalance;
        cachedImmatureBalance = newImmatureBalance;
        emit balanceChanged(newBalance, newUnconfirmedBalance, newImmatureBalance);
    }
}

void WalletModel::updateTransaction(const QString &hash, int status)
{
    if(transactionTableModel)
        transactionTableModel->updateTransaction(hash, status);

    if (nameTableModel)
        nameTableModel->updateTransaction(hash, status);

    // Balance and number of transactions might have changed
    checkBalanceChanged();

    int newNumTransactions = getNumTransactions();
    if(cachedNumTransactions != newNumTransactions)
    {
        cachedNumTransactions = newNumTransactions;
        emit numTransactionsChanged(newNumTransactions);
    }
}

void WalletModel::updateAddressBook(const QString &address, const QString &label, bool isMine, int status)
{
    if(addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, status);
}

bool WalletModel::validateAddress(const QString &address)
{
    CBitcoinAddress addressParsed(address.toStdString());
    return addressParsed.IsValid();
}

WalletModel::SendCoinsReturn WalletModel::sendCoins(const QList<SendCoinsRecipient> &recipients)
{
    qint64 total = 0;
    QSet<QString> setAddress;
    QString hex;

    if(recipients.empty())
    {
        return OK;
    }

    // Pre-check input data for validity
    foreach(const SendCoinsRecipient &rcp, recipients)
    {
        if(!validateAddress(rcp.address))
        {
            return InvalidAddress;
        }
        setAddress.insert(rcp.address);

        if(rcp.amount <= 0)
        {
            return InvalidAmount;
        }
        total += rcp.amount;
    }

    if(recipients.size() > setAddress.size())
    {
        return DuplicateAddress;
    }

    if(total > getBalance())
    {
        return AmountExceedsBalance;
    }

    if((total + nTransactionFee) > getBalance())
    {
        return SendCoinsReturn(AmountWithFeeExceedsBalance, nTransactionFee);
    }

    {
        LOCK2(cs_main, wallet->cs_wallet);

        // Sendmany
        std::vector<std::pair<CScript, int64> > vecSend;
        foreach(const SendCoinsRecipient &rcp, recipients)
        {
            CScript scriptPubKey;
            scriptPubKey.SetDestination(CBitcoinAddress(rcp.address.toStdString()).Get());
            vecSend.push_back(std::make_pair(scriptPubKey, rcp.amount));
        }

        CWalletTx wtx;
        CReserveKey keyChange(wallet);
        int64 nFeeRequired = 0;
        bool fCreated = wallet->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired);

        if(!fCreated)
        {
            if((total + nFeeRequired) > wallet->GetBalance())
            {
                return SendCoinsReturn(AmountWithFeeExceedsBalance, nFeeRequired);
            }
            return TransactionCreationFailed;
        }
        if(!uiInterface.ThreadSafeAskFee(nFeeRequired))
        {
            return Aborted;
        }
        if(!wallet->CommitTransaction(wtx, keyChange))
        {
            return TransactionCommitFailed;
        }
        hex = QString::fromStdString(wtx.GetHash().GetHex());
    }

    // Add addresses / update labels that we've sent to to the address book
    foreach(const SendCoinsRecipient &rcp, recipients)
    {
        std::string strAddress = rcp.address.toStdString();
        CTxDestination dest = CBitcoinAddress(strAddress).Get();
        std::string strLabel = rcp.label.toStdString();
        {
            LOCK(wallet->cs_wallet);

            std::map<CTxDestination, std::string>::iterator mi = wallet->mapAddressBook.find(dest);

            // Check if we have a new address or an updated label
            if (mi == wallet->mapAddressBook.end() || mi->second != strLabel)
            {
                wallet->SetAddressBookName(dest, strLabel);
            }
        }
    }

    return SendCoinsReturn(OK, 0, hex);
}

bool WalletModel::nameAvailable(const QString &name)
{
    std::string strName = name.toStdString();
    vchType vchName(strName.begin(), strName.end());

    DatabaseSet dbset("r");
    return NameAvailable (dbset, vchName);
}

QString WalletModel::nameRegister(const QString &name, const std::string &data)
{
  const std::string strName = name.toStdString();

  if (!ForkInEffect (FORK_CARRYINGCAP, nBestHeight))
    return tr("name_register is not yet available");

  if (!IsValidPlayerName (strName))
    throw tr("invalid player name");
  const vchType vchName(strName.begin(), strName.end());
  const vchType vchValue(data.begin(), data.end());

  CRITICAL_BLOCK (cs_main)
    {
      if (mapNamePending.count (vchName) && !mapNamePending[vchName].empty ())
        {
          error ("name_register: there are %d pending operations"
                 " on that name, including %s",
                 mapNamePending[vchName].size (),
                 mapNamePending[vchName].begin ()->GetHex ().c_str ());
          return tr("there are pending operations on that name");
        }

      CNameDB dbName("r");
      CTransaction tx;
      if (GetTxOfName (dbName, vchName, tx) && !tx.IsGameTx ())
        {
          error ("name_register: this name is already active with tx %s",
                 tx.GetHash ().GetHex ().c_str ());
          return tr("this name is already active");
        }
    }

  CWalletTx wtx;
  wtx.nVersion = NAMECOIN_TX_VERSION;

  CScript scriptPubKeyOrig;
  const vchType vchPubKey = wallet->GetKeyFromKeyPool ();
  scriptPubKeyOrig.SetBitcoinAddress (vchPubKey);

  CScript scriptPubKey;
  scriptPubKey << OP_NAME_FIRSTUPDATE << vchName << vchValue
               << OP_2DROP << OP_DROP;
  scriptPubKey += scriptPubKeyOrig;

  CRITICAL_BLOCK(cs_main)
    {
      const int64_t nCoinAmount = GetRequiredGameFee (vchName, vchValue);
      std::string strError = wallet->SendMoney (scriptPubKey, nCoinAmount,
                                                wtx, false);
      if (strError != "")
          return QString::fromStdString (strError);
      mapMyNames[vchName] = wtx.GetHash ();
    }

  return "";
}

QString WalletModel::nameUpdate(const QString &name, const std::string &data, const QString &transferToAddress)
{
    std::string strName = name.toStdString();
    std::vector<unsigned char> vchName(strName.begin(), strName.end());

    std::vector<unsigned char> vchValue(data.begin(), data.end());

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;

    CScript scriptPubKey;
    scriptPubKey << OP_NAME_UPDATE << vchName << vchValue << OP_2DROP << OP_DROP;

    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(wallet->cs_mapWallet)
    {
        if (mapNamePending.count(vchName) && mapNamePending[vchName].size())
        {
            error("name_update() : there are %d pending operations on that name, including %s",
                    mapNamePending[vchName].size(),
                    mapNamePending[vchName].begin()->GetHex().c_str());
            return tr("There are pending operations on that name");
        }

        CTransaction tx;
        {
          CNameDB dbName("r");
          if (!GetTxOfName (dbName, vchName, tx))
            return tr("Could not find a coin with this name");
        }

        const uint256 wtxInHash = tx.GetHash();
        if (!wallet->mapWallet.count(wtxInHash))
        {
            error("name_update() : this coin is not in your wallet %s",
                    wtxInHash.GetHex().c_str());
            return tr("This coin is not in your wallet");
        }

        CReserveKey newKey(wallet);
        CScript scriptPubKeyOrig;
        if (transferToAddress != "")
          {
            const std::string strAddress = transferToAddress.toStdString();
            uint160 hash160;
            const bool isValid = AddressToHash160(strAddress, hash160);
            if (!isValid)
                return tr("Invalid Huntercoin address");
            scriptPubKeyOrig.SetBitcoinAddress(strAddress);
          }
        else if (fAddressReuse)
          {
            uint160 hash160;
            GetNameAddress(tx, hash160);
            scriptPubKeyOrig.SetBitcoinAddress(hash160);
          }
        else
          {
            const vchType vchPubKey = newKey.GetReservedKey ();
            assert (wallet->HaveKey (vchPubKey));
            scriptPubKeyOrig.SetBitcoinAddress (vchPubKey);
          }
        scriptPubKey += scriptPubKeyOrig;

        /* Find amount locked in this name and add required game fee.  */
        const int nTxOut = IndexOfNameOutput (tx);
        int64_t nCoinAmount = tx.vout[nTxOut].nValue;
        nCoinAmount += GetRequiredGameFee (vchName, vchValue);

        /* Don't ask for fee confirmation here since name_update includes
           a mandatory minimum fee.  */
        CWalletTx& wtxIn = wallet->mapWallet[wtxInHash];
        std::string strError;
        strError = SendMoneyWithInputTx (scriptPubKey, nCoinAmount,
                                         wtxIn, wtx, false);

        /* Make sure to keep the (possibly) reserved key in case
           of a successful transaction!  */
        if (strError == "")
          newKey.KeepKey (); 

        return QString::fromStdString (strError);
    }
}

OptionsModel *WalletModel::getOptionsModel()
{
    return optionsModel;
}

AddressTableModel *WalletModel::getAddressTableModel()
{
    return addressTableModel;
}

NameTableModel *WalletModel::getNameTableModel()
{
    return nameTableModel;
}

TransactionTableModel *WalletModel::getTransactionTableModel()
{
    return transactionTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if(!wallet->IsCrypted())
    {
        return Unencrypted;
    }
    else if(wallet->IsLocked())
    {
        return Locked;
    }
    else
    {
        return Unlocked;
    }
}

bool WalletModel::setWalletEncrypted(bool encrypted, const SecureString &passphrase)
{
    if(encrypted)
    {
        // Encrypt
        return wallet->EncryptWallet(passphrase);
    }
    else
    {
        // Decrypt -- TODO; not supported yet
        return false;
    }
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase)
{
    if(locked)
    {
        // Lock
        return wallet->Lock();
    }
    else
    {
        // Unlock
        return wallet->Unlock(passPhrase);
    }
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
    bool retval;
    {
        LOCK(wallet->cs_wallet);
        wallet->Lock(); // Make sure wallet is locked before attempting pass change
        retval = wallet->ChangeWalletPassphrase(oldPass, newPass);
    }
    return retval;
}

bool WalletModel::backupWallet(const QString &filename)
{
    return BackupWallet(*wallet, filename.toLocal8Bit().data());
}

// Handlers for core signals
static void NotifyKeyStoreStatusChanged(WalletModel *walletmodel, CKeyStore *wallet)
{
    OutputDebugStringF("NotifyKeyStoreStatusChanged\n");
    QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
}

static void NotifyAddressBookChanged(WalletModel *walletmodel, CWallet *wallet, const CTxDestination &address, const std::string &label, bool isMine, ChangeType status)
{
    OutputDebugStringF("NotifyAddressBookChanged %s %s isMine=%i status=%i\n", CBitcoinAddress(address).ToString().c_str(), label.c_str(), isMine, status);
    QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(CBitcoinAddress(address).ToString())),
                              Q_ARG(QString, QString::fromStdString(label)),
                              Q_ARG(bool, isMine),
                              Q_ARG(int, status));
}

static void NotifyTransactionChanged(WalletModel *walletmodel, CWallet *wallet, const uint256 &hash, ChangeType status)
{
    if (!hash)
        return;     // Ignore coinbase transactions
    OutputDebugStringF("NotifyTransactionChanged %s status=%i\n", hash.GetHex().c_str(), status);
    QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(hash.GetHex())),
                              Q_ARG(int, status));
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    wallet->NotifyStatusChanged.connect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyAddressBookChanged.connect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5));
    wallet->NotifyTransactionChanged.connect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    wallet->NotifyStatusChanged.disconnect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyAddressBookChanged.disconnect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5));
    wallet->NotifyTransactionChanged.disconnect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock()
{
    bool was_locked = getEncryptionStatus() == Locked;
    if(was_locked)
    {
        // Request UI to unlock wallet
        emit requireUnlock();
    }
    // If wallet is still locked, unlock was failed or cancelled, mark context as invalid
    bool valid = getEncryptionStatus() != Locked;

    return UnlockContext(this, valid, was_locked);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *wallet, bool valid, bool relock):
        wallet(wallet),
        valid(valid),
        relock(relock)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if(valid && relock)
    {
        wallet->setWalletLocked(true);
    }
}

void WalletModel::UnlockContext::CopyFrom(const UnlockContext& rhs)
{
    // Transfer context; old object no longer relocks wallet
    *this = rhs;
    rhs.relock = false;
}

bool WalletModel::DeleteTransaction(const QString &strHash, QString &retMsg)
{
    uint256 hash;
    hash.SetHex(strHash.toStdString());

    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(wallet->cs_mapWallet)
    {
        if (!wallet->mapWallet.count(hash))
        {
            retMsg = tr("FAILED: transaction not in wallet");
            return false;
        }

        if (!mapTransactions.count(hash))
        {
            CTransaction tx;
            uint256 hashBlock = 0;
            if (GetTransaction(hash, tx, hashBlock /*, true*/) && hashBlock != 0)
            {
                retMsg = tr("FAILED: transaction is already in block (%1)").arg(QString::fromStdString(hashBlock.GetHex()));
                return false;
            }
        }
        CWalletTx wtx = wallet->mapWallet[hash];
        UnspendInputs(wtx);

        // We are not removing from mapTransactions because this can cause memory corruption
        // during mining. The user should restart to clear the tx from memory.
        wtx.RemoveFromMemoryPool();
        wallet->EraseFromWallet(wtx.GetHash());

        std::vector<unsigned char> vchName;
        if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName))
        {
            mapNamePending[vchName].erase(wtx.GetHash());

            if (nameTableModel)
                nameTableModel->refreshName(vchName);

            retMsg = tr("Success, removed from pending");
        }
        else
            retMsg = tr("Success");

        return true;
    }
}

bool WalletModel::RebroadcastTransaction(const QString &strHash, QString &retMsg)
{
    uint256 hash;
    hash.SetHex(strHash.toStdString());

    CTransaction tx;
    uint256 hashBlock = 0;
    if (!GetTransaction(hash, tx, hashBlock /*, true*/))
    {
        retMsg = tr("FAILED: no information available about transaction");
        return false;
    }
    if (hashBlock != 0)
    {
        retMsg = tr("FAILED: transaction is already in block (%1)").arg(QString::fromStdString(hashBlock.GetHex()));
        return false;
    }
    RelayMessage(CInv(MSG_TX, hash), tx);
    retMsg = tr("Success");
    return true;
}
