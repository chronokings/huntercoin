// Copyright (c) 2009-2010 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_MAIN_H
#define BITCOIN_MAIN_H

#include "bignum.h"
#include "net.h"
#include "key.h"
#include "script.h"
#include "walletdb.h"

#include "scrypt.h"

#include <list>
#ifndef Q_MOC_RUN
#include <boost/shared_ptr.hpp>
#endif

#ifdef __WXMSW__
#include <io.h> /* for _commit */
#elif !defined(MAC_OSX)
#include <sys/prctl.h>
#endif

class CBlock;
class CBlockIndex;
class CWalletTx;
class CWallet;
class CKeyItem;
class CReserveKey;
class CWalletDB;
class CTestPool;

class CMessageHeader;
class CAddress;
class CInv;
class CRequestTracker;
class CNode;
class CBlockIndex;
class CHooks;

class CAuxPow;

static const unsigned int MAX_BLOCK_SIZE = 1000000;
static const unsigned int MAX_BLOCK_SIZE_GEN = MAX_BLOCK_SIZE/2;
static const int MAX_BLOCK_SIGOPS = MAX_BLOCK_SIZE/50;
static const int64 COIN = 100000000;
static const int64 CENT = 1000000;
static const int64 MIN_TX_FEE = 500000;
static const int64 MIN_RELAY_TX_FEE = 10000;
static const int64 MAX_MONEY = 43000000 * COIN; // Allow for premine.
inline bool MoneyRange(int64 nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }
static const int COINBASE_MATURITY = 100;
static const int COINBASE_MATURITY_DISPLAY = COINBASE_MATURITY + 20;
static const int GAME_REWARD_MATURITY = 100;
static const int GAME_REWARD_MATURITY_DISPLAY = GAME_REWARD_MATURITY + 20;
#ifdef USE_UPNP
static const int fHaveUPnP = true;
#else
static const int fHaveUPnP = false;
#endif

static const int NAMECOIN_TX_VERSION = 0x7100;
static const int GAME_TX_VERSION = 0x87100;


enum { ALGO_SHA256D = 0, ALGO_SCRYPT = 1, NUM_ALGOS };
extern int miningAlgo;


extern CCriticalSection cs_main;
extern CCriticalSection cs_AppendBlockFile;
extern CCriticalSection cs_mapTransactions;
extern std::map<uint256, CBlockIndex*> mapBlockIndex;
extern uint256 hashGenesisBlock;
extern CBigNum bnProofOfWorkLimit[NUM_ALGOS], bnInitialHashTarget[NUM_ALGOS];
extern CBlockIndex* pindexGenesisBlock;
extern int nBestHeight;
extern CBigNum bnBestChainWork;
extern CBigNum bnBestInvalidWork;
extern uint256 hashBestChain;
extern CBlockIndex* pindexBest;
extern unsigned int nTransactionsUpdated;
extern double dHashesPerSec;
extern const std::string strMessageMagic;
extern int64 nHPSTimerStart;
extern int64 nTimeBestReceived;
extern CCriticalSection cs_setpwalletRegistered;
extern std::set<CWallet*> setpwalletRegistered;

// Settings
extern int fGenerateBitcoins, fGenerationAlgo;
extern int64 nTransactionFee;
extern int64 nMinimumInputValue;
extern int fLimitProcessors;
extern int nLimitProcessors;
extern int fMinimizeToTray;
extern int fMinimizeOnClose;
extern int fUseUPnP;

/* Handle fork heights.  The function checks whether a fork is in effect
   at the given height -- and may use different heights for testnet
   and mainnet, or for a "testing mode".  */
enum Fork
{

  /* Poison disaster, increased general cost 1 HUC -> 10 HUC, just general
     as initial character.  */
  FORK_POISON,

  /* Maximum carrying-capacity introduced, removed spawn death,
     new-style name registration, stricter rule checks for transaction
     version and auxpow (in parallel to Namecoin).  */
  FORK_CARRYINGCAP,

  /* Update parameters (general 10 HUC -> 200 HUC, carrying capacity increased
     to 2000 HUC, heart spawn rate reduced to 1/500, general explosion
     radius only 1).  */
  FORK_LESSHEARTS,

  /* Implement "life steal".  This adds a game fee for destructs (5 HUC),
     completely disables hearts and removes all "hearted" hunters.  It also
     randomises spawn and banking locations.  */
  FORK_LIFESTEAL,

  /* "timesave"  This makes hunters and banks spawn always near harvest areas.
     It also adds protection for newly spawned hunters and a spectator mode.
     Fee for a new hunter and destruct fee is set to 1 HUC.
     The refundable fee per hunter is set to 100 HUC   */
  FORK_TIMESAVE,
};
bool ForkInEffect (Fork type, unsigned nHeight);

/* Check whether the height is *exactly* when the fork starts to take effect.
   This is used sometimes to trigger special events in the game.  */
bool IsForkHeight (Fork type, unsigned nHeight);


extern CHooks* hooks;


class CReserveKey;
class CTxDB;
class CTxIndex;

void RegisterWallet(CWallet* pwalletIn);
void UnregisterWallet(CWallet* pwalletIn);
/** Push an updated transaction to all registered wallets */
void SyncWithWallets(const CTransaction& tx, const CBlock* pblock = NULL, bool fUpdate = false);
bool ProcessBlock(CNode* pfrom, CBlock* pblock);
bool CheckDiskSpace (uint64 nAdditionalBytes = 0);
FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode="rb");
FILE* AppendBlockFile (DatabaseSet& dbset, unsigned int& nFileRet,
                       unsigned size);
void FlushBlockFile(FILE *f);
bool LoadBlockIndex(bool fAllowNew=true);
void PrintBlockTree();
CBlockIndex* FindBlockByHeight(int nHeight);
bool ProcessMessages(CNode* pfrom);
bool SendMessages(CNode* pto, bool fSendTrickle);
void GenerateBitcoins(bool fGenerate, CWallet* pwallet);
CBlock* CreateNewBlock(CReserveKey& reservekey, int algo);
int64 GetBlockValue(int nHeight, int64 nFees);
CBlock* CreateNewBlock(CReserveKey& reservekey);
void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce, int64& nPrevTime);
void IncrementExtraNonceWithAux(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce, int64& nPrevTime, std::vector<unsigned char>& vchAux);
void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1);
const CBlockIndex* GetLastBlockIndexForAlgo(const CBlockIndex* pindex, int algo);
bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey);
bool CheckProofOfWork(uint256 hash, unsigned int nBits, int algo);
int GetTotalBlocksEstimate();
int GetNumBlocksOfPeers();
bool IsInitialBlockDownload();
std::string GetWarnings(std::string strFor);
/** Retrieve a transaction (from memory pool, or from disk, if possible) */
bool GetTransaction(const uint256 &hash, CTransaction &txOut, uint256 &hashBlock /*, bool fAllowSlow = false*/  );










bool GetWalletFile(CWallet* pwallet, std::string &strWalletFileOut);

template<typename T>
bool WriteSetting(const std::string& strKey, const T& value)
{
    bool fOk = false;
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
    {
        std::string strWalletFile;
        if (!GetWalletFile(pwallet, strWalletFile))
            continue;
        fOk |= CWalletDB(strWalletFile).WriteSetting(strKey, value);
    }
    return fOk;
}


class CDiskTxPos
{
public:
    unsigned int nBlockFile;
    unsigned int nBlockPos;
    unsigned int nTxFile;    // Game transactions are stored outside of blocks, so the file may be different
    unsigned int nTxPos;

    CDiskTxPos()
    {
        SetNull();
    }

    CDiskTxPos(unsigned int nFileIn, unsigned int nBlockPosIn, unsigned int nTxPosIn)
    {
        nBlockFile = nTxFile = nFileIn;
        nBlockPos = nBlockPosIn;
        nTxPos = nTxPosIn;
    }

    CDiskTxPos(unsigned int nBlockFileIn, unsigned int nBlockPosIn, unsigned int nTxFileIn, unsigned int nTxPosIn)
    {
        nBlockFile = nBlockFileIn;
        nBlockPos = nBlockPosIn;
        nTxFile = nTxFileIn;
        nTxPos = nTxPosIn;
    }

    IMPLEMENT_SERIALIZE( READWRITE(FLATDATA(*this)); )
    void SetNull() { nBlockFile = nTxFile = -1; nBlockPos = 0; nTxPos = 0; }
    bool IsNull() const { return (nBlockFile == -1); }

    friend bool operator==(const CDiskTxPos& a, const CDiskTxPos& b)
    {
        return (a.nBlockFile == b.nBlockFile &&
                a.nBlockPos  == b.nBlockPos &&
                a.nTxFile    == b.nTxFile &&
                a.nTxPos     == b.nTxPos);
    }

    friend bool operator!=(const CDiskTxPos& a, const CDiskTxPos& b)
    {
        return !(a == b);
    }

    std::string ToString() const
    {
        if (IsNull())
            return "null";
        else
            return strprintf("(nBlockFile=%d, nBlockPos=%d, nTxFile=%d, nTxPos=%d)", nBlockFile, nBlockPos, nTxFile, nTxPos);
    }

    void print() const
    {
        printf("%s", ToString().c_str());
    }
};

class CNameIndex
{
public:
    CDiskTxPos txPos;
    unsigned int nHeight;
    std::vector<unsigned char> vValue;

    CNameIndex()
    {
    }

    CNameIndex(CDiskTxPos txPosIn, unsigned int nHeightIn, std::vector<unsigned char> vValueIn)
    {
        txPos = txPosIn;
        nHeight = nHeightIn;
        vValue = vValueIn;
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(txPos);
        READWRITE(nHeight);
        READWRITE(vValue);
    )
};




class CInPoint
{
public:
    CTransaction* ptx;
    unsigned int n;

    CInPoint() { SetNull(); }
    CInPoint(CTransaction* ptxIn, unsigned int nIn) { ptx = ptxIn; n = nIn; }
    void SetNull() { ptx = NULL; n = -1; }
    bool IsNull() const { return (ptx == NULL && n == -1); }
};




class COutPoint
{
public:
    uint256 hash;
    unsigned int n;

    COutPoint() { SetNull(); }
    COutPoint(uint256 hashIn, unsigned int nIn) { hash = hashIn; n = nIn; }
    IMPLEMENT_SERIALIZE( READWRITE(FLATDATA(*this)); )
    void SetNull() { hash = 0; n = -1; }
    bool IsNull() const { return (hash == 0 && n == -1); }

    friend bool operator<(const COutPoint& a, const COutPoint& b)
    {
        return (a.hash < b.hash || (a.hash == b.hash && a.n < b.n));
    }

    friend bool operator==(const COutPoint& a, const COutPoint& b)
    {
        return (a.hash == b.hash && a.n == b.n);
    }

    friend bool operator!=(const COutPoint& a, const COutPoint& b)
    {
        return !(a == b);
    }

    std::string ToString() const
    {
        return strprintf("COutPoint(%s, %d)", hash.ToString().substr(0,10).c_str(), n);
    }

    void print() const
    {
        printf("%s\n", ToString().c_str());
    }
};




//
// An input of a transaction.  It contains the location of the previous
// transaction's output that it claims and a signature that matches the
// output's public key.
//
class CTxIn
{
public:
    COutPoint prevout;
    CScript scriptSig;
    unsigned int nSequence;

    CTxIn()
    {
        nSequence = UINT_MAX;
    }

    explicit CTxIn(COutPoint prevoutIn, CScript scriptSigIn=CScript(), unsigned int nSequenceIn=UINT_MAX)
    {
        prevout = prevoutIn;
        scriptSig = scriptSigIn;
        nSequence = nSequenceIn;
    }

    CTxIn(uint256 hashPrevTx, unsigned int nOut, CScript scriptSigIn=CScript(), unsigned int nSequenceIn=UINT_MAX)
    {
        prevout = COutPoint(hashPrevTx, nOut);
        scriptSig = scriptSigIn;
        nSequence = nSequenceIn;
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(prevout);
        READWRITE(scriptSig);
        READWRITE(nSequence);
    )

    bool IsFinal() const
    {
        return (nSequence == UINT_MAX);
    }

    friend bool operator==(const CTxIn& a, const CTxIn& b)
    {
        return (a.prevout   == b.prevout &&
                a.scriptSig == b.scriptSig &&
                a.nSequence == b.nSequence);
    }

    friend bool operator!=(const CTxIn& a, const CTxIn& b)
    {
        return !(a == b);
    }

    std::string ToString() const
    {
        std::string str;
        str += "CTxIn(";
        str += prevout.ToString();
        if (prevout.IsNull())
            str += strprintf(", coinbase %s", HexStr(scriptSig).c_str());
        else
            str += strprintf(", scriptSig=%s", scriptSig.ToString().substr(0,24).c_str());
        if (nSequence != UINT_MAX)
            str += strprintf(", nSequence=%u", nSequence);
        str += ")";
        return str;
    }

    void print() const
    {
        printf("%s\n", ToString().c_str());
    }
};




//
// An output of a transaction.  It contains the public key that the next input
// must be able to sign with to claim it.
//
class CTxOut
{
public:
    int64 nValue;
    CScript scriptPubKey;

    CTxOut()
    {
        SetNull();
    }

    inline CTxOut (int64 nValueIn, const CScript& scriptPubKeyIn)
      : nValue(nValueIn), scriptPubKey (scriptPubKeyIn)
    {}

    IMPLEMENT_SERIALIZE
    (
        READWRITE(nValue);
        READWRITE(scriptPubKey);
    )

    void SetNull()
    {
        nValue = -1;
        scriptPubKey.clear();
    }

    bool IsNull()
    {
        return (nValue == -1);
    }

    uint256 GetHash() const
    {
        return SerializeHash(*this);
    }

    friend bool operator==(const CTxOut& a, const CTxOut& b)
    {
        return (a.nValue       == b.nValue &&
                a.scriptPubKey == b.scriptPubKey);
    }

    friend bool operator!=(const CTxOut& a, const CTxOut& b)
    {
        return !(a == b);
    }

    inline bool
    IsUnspendable () const
    {
      return scriptPubKey.IsUnspendable ();
    }

    bool IsStandard () const;

    std::string ToString() const
    {
        if (scriptPubKey.size() < 6)
            return "CTxOut(error)";
        return strprintf("CTxOut(nValue=%"PRI64d".%08"PRI64d", scriptPubKey=%s)", nValue / COIN, nValue % COIN, scriptPubKey.ToString().substr(0,30).c_str());
    }

    void print() const
    {
        printf("%s\n", ToString().c_str());
    }
};

/**
 * An entry in the UTXO set.  This is basically a CTxOut, but it also
 * contains other information that is needed in ConnectInputs
 * for checking validity of spending the output.
 */
class CUtxoEntry
{
public:

  CTxOut txo;
  int height;
  bool isCoinbase;
  bool isGameTx;

public:

  /* Needed to declare variables as CUtxoEntry and assign to them later.  */
  inline CUtxoEntry ()
  {}

  CUtxoEntry (const CTransaction& tx, unsigned n, int h);
  
  IMPLEMENT_SERIALIZE
  (
    READWRITE (txo);
    READWRITE (height);
    READWRITE (isCoinbase);
    READWRITE (isGameTx);
  )

  friend inline bool
  operator== (const CUtxoEntry& a, const CUtxoEntry& b)
  {
    return (a.txo == b.txo && a.height == b.height
            && a.isCoinbase == b.isCoinbase && a.isGameTx == b.isGameTx);
  }

  friend inline bool
  operator!= (const CUtxoEntry& a, const CUtxoEntry& b)
  {
    return !(a == b);
  }

  inline bool
  IsUnspendable () const
  {
    return txo.IsUnspendable ();
  }

};


//
// The basic transaction that is broadcasted on the network and contained in
// blocks.  A transaction can contain multiple inputs and outputs.
//
class CTransaction
{
public:
    int nVersion;
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    unsigned int nLockTime;


    CTransaction()
    {
        SetNull();
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(vin);
        READWRITE(vout);
        READWRITE(nLockTime);
    )

    void SetNull()
    {
        nVersion = 1;
        vin.clear();
        vout.clear();
        nLockTime = 0;
    }

    bool IsNull() const
    {
        return (vin.empty() && vout.empty());
    }

    uint256 GetHash() const
    {
        return SerializeHash(*this);
    }

    inline const char*
    GetHashForLog () const
    {
        return GetHash ().ToLogString ();
    }

    bool IsFinal(int nBlockHeight=0, int64 nBlockTime=0) const
    {
        // Time based nLockTime implemented in 0.1.6
        if (nLockTime == 0)
            return true;
        if (nBlockHeight == 0)
            nBlockHeight = nBestHeight;
        if (nBlockTime == 0)
            nBlockTime = GetAdjustedTime();
        if ((int64)nLockTime < (nLockTime < 500000000 ? (int64)nBlockHeight : nBlockTime))
            return true;
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (!txin.IsFinal())
                return false;
        return true;
    }

    bool IsNewerThan(const CTransaction& old) const
    {
        if (vin.size() != old.vin.size())
            return false;
        for (int i = 0; i < vin.size(); i++)
            if (vin[i].prevout != old.vin[i].prevout)
                return false;

        bool fNewer = false;
        unsigned int nLowest = UINT_MAX;
        for (int i = 0; i < vin.size(); i++)
        {
            if (vin[i].nSequence != old.vin[i].nSequence)
            {
                if (vin[i].nSequence <= nLowest)
                {
                    fNewer = false;
                    nLowest = vin[i].nSequence;
                }
                if (old.vin[i].nSequence < nLowest)
                {
                    fNewer = true;
                    nLowest = old.vin[i].nSequence;
                }
            }
        }
        return fNewer;
    }

    bool IsCoinBase() const
    {
        return !IsGameTx() && vin.size() == 1 && vin[0].prevout.IsNull();
    }
    
    bool IsGameTx() const
    {
        return nVersion == GAME_TX_VERSION;
    }

    void SetGameTx()
    {
        nVersion = GAME_TX_VERSION;
    }

    int GetSigOpCount() const
    {
        int n = 0;
        BOOST_FOREACH(const CTxIn& txin, vin)
            n += txin.scriptSig.GetSigOpCount();
        BOOST_FOREACH(const CTxOut& txout, vout)
            n += txout.scriptPubKey.GetSigOpCount();
        return n;
    }

    bool IsStandard() const
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (!txin.scriptSig.IsPushOnly())
                return error("nonstandard txin: %s", txin.scriptSig.ToString().c_str());
        BOOST_FOREACH(const CTxOut& txout, vout)
            if (!txout.IsStandard ())
                return error("nonstandard txout: %s", txout.scriptPubKey.ToString().c_str());
        return true;
    }

    int64 GetValueOut() const
    {
        int64 nValueOut = 0;
        BOOST_FOREACH(const CTxOut& txout, vout)
        {
            nValueOut += txout.nValue;
            if (!MoneyRange(txout.nValue) || !MoneyRange(nValueOut))
                throw std::runtime_error("CTransaction::GetValueOut() : value out of range");
        }
        return nValueOut;
    }

    static bool AllowFree(double dPriority)
    {
        // Large (in bytes) low-priority (new, small-coin) transactions
        // need a fee.
        return dPriority > COIN * 144 / 250;
    }

    int64 GetMinFee(unsigned int nBlockSize=1, bool fAllowFree=true, bool fForRelay=false) const;

    bool ReadFromDisk(CDiskTxPos pos)
    {
        CAutoFile filein = OpenBlockFile(pos.nTxFile, 0, "rb");
        if (!filein)
            return error("CTransaction::ReadFromDisk() : OpenBlockFile failed");

        // Read transaction
        if (fseek(filein, pos.nTxPos, SEEK_SET) != 0)
            return error("CTransaction::ReadFromDisk() : fseek failed");
        filein >> *this;
        return true;
    }

    friend bool operator==(const CTransaction& a, const CTransaction& b)
    {
        return (a.nVersion  == b.nVersion &&
                a.vin       == b.vin &&
                a.vout      == b.vout &&
                a.nLockTime == b.nLockTime);
    }

    friend bool operator!=(const CTransaction& a, const CTransaction& b)
    {
        return !(a == b);
    }


    std::string ToString() const
    {
        std::string str;
        str += strprintf("CTransaction(hash=%s, ver=%d, vin.size=%d, vout.size=%d, nLockTime=%d)\n",
            GetHash().ToString().substr(0,10).c_str(),
            nVersion,
            vin.size(),
            vout.size(),
            nLockTime);
        for (int i = 0; i < vin.size(); i++)
            str += "    " + vin[i].ToString() + "\n";
        for (int i = 0; i < vout.size(); i++)
            str += "    " + vout[i].ToString() + "\n";
        return str;
    }

    void print() const
    {
        printf("%s", ToString().c_str());
    }


    bool ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet);
    bool ReadFromDisk(CTxDB& txdb, COutPoint prevout);
    bool ReadFromDisk(COutPoint prevout);
    bool DisconnectInputs (DatabaseSet& dbset, CBlockIndex* pindex);
    
    bool ConnectInputs(DatabaseSet& dbset, CTestPool& testPool,
                       CDiskTxPos posThisTx, CBlockIndex* pindexBlock,
                       int64& nFees, bool fBlock, bool fMiner, int64 nMinFee=0);
    bool ClientConnectInputs();
    bool CheckTransaction() const;
    bool AcceptToMemoryPool(DatabaseSet& dbset, bool fCheckInputs=true,
                            bool* pfMissingInputs=NULL);
    bool AcceptToMemoryPool(bool fCheckInputs=true, bool* pfMissingInputs=NULL);
protected:
    bool AddToMemoryPoolUnchecked();
public:
    bool RemoveFromMemoryPool();
};





//
// A transaction with a merkle branch linking it to the block chain
//
class CMerkleTx : public CTransaction
{
public:
    uint256 hashBlock;
    std::vector<uint256> vMerkleBranch;
    int nIndex;

    // memory only
    mutable bool fMerkleVerified;


    CMerkleTx()
    {
        Init();
    }

    CMerkleTx(const CTransaction& txIn) : CTransaction(txIn)
    {
        Init();
    }

    void Init()
    {
        hashBlock = 0;
        nIndex = -1;
        fMerkleVerified = false;
    }


    IMPLEMENT_SERIALIZE
    (
        nSerSize += SerReadWrite(s, *(CTransaction*)this, nType, nVersion, ser_action);
        nVersion = this->nVersion;
        READWRITE(hashBlock);
        READWRITE(vMerkleBranch);
        READWRITE(nIndex);
    )


    int SetMerkleBranch(const CBlock* pblock=NULL);
    int GetDepthInMainChain(int& nHeightRet) const;
    int GetDepthInMainChain() const { int nHeight; return GetDepthInMainChain(nHeight); }
    bool IsInMainChain() const { return GetDepthInMainChain() > 0; }
    int GetBlocksToMaturity() const;

    inline int
    GetHeightInMainChain() const
    {
      int nHeight;
      if (GetDepthInMainChain (nHeight) == 0)
        return -1;

      return nHeight;
    }

    bool AcceptToMemoryPool (DatabaseSet& dbset, bool fCheckInputs = true);
    bool AcceptToMemoryPool();
};




//
// A txdb record that contains the disk location of a transaction and an array
// of flags whether its outputs have already been spent.
//
class CTxIndex
{
public:
    CDiskTxPos pos;

public:

    inline CTxIndex ()
    {
      SetNull ();
    }

    inline explicit
    CTxIndex (const CDiskTxPos& posIn)
      : pos(posIn)
    {}

    IMPLEMENT_SERIALIZE
    (
        assert (nType == SER_DISK);
        if (nVersion < 1001000)
          {
            assert (fRead);
            int nVersionDummy;
            READWRITE(nVersionDummy);
            assert (nVersionDummy < 1001000);
          }
        READWRITE(pos);

        if (nVersion < 1001000)
          {
            assert (fRead); 
            std::vector<CDiskTxPos> vSpent;
            READWRITE (vSpent);
          }
        else if (nVersion < 1001300)
          {
            assert (fRead);
            std::vector<unsigned char> vSpent;
            READWRITE (vSpent);
          }
    )

    void SetNull()
    {
        pos.SetNull();
    }

    bool IsNull()
    {
        return pos.IsNull();
    }

    friend bool operator==(const CTxIndex& a, const CTxIndex& b)
    {
        return (a.pos == b.pos);
    }

    friend bool operator!=(const CTxIndex& a, const CTxIndex& b)
    {
        return !(a == b);
    }

    const CBlockIndex* GetContainingBlock () const;
    int GetHeight () const;
    int GetDepthInMainChain () const;
};


/* Test pool while creating a block (for miners).  This records (somehow)
   the changes made to the UTXO set -- new transactions as well as
   spent outputs.  */
class CTestPool
{
public:

  /* New transactions available.  This contains only their hash, since
     the full tx is available in mapTransactions.  */
  std::set<uint256> includedTx;

  /* Spent outpoints so far.  */
  std::set<COutPoint> spent;

  /* Construct an empty test pool.  */
  inline CTestPool ()
    : includedTx(), spent()
  {}

  /* Copy constructor.  */
  inline CTestPool (const CTestPool& p)
    : includedTx(p.includedTx), spent(p.spent)
  {}

  /* Add a transaction to the test pool.  */
  inline void
  AddTx (const CTransaction& tx)
  {
    const uint256 hash = tx.GetHash ();
    assert (includedTx.count (hash) == 0);
    includedTx.insert (hash);
  }

  /* Mark an outpoint as spent.  */
  inline void
  SetSpent (const COutPoint& out)
  {
    assert (!IsSpent (out));
    spent.insert (out);
  }

  /* See if an out point has already been spent.  */
  inline bool
  IsSpent (const COutPoint& out) const
  {
    return spent.count (out) > 0;
  }

  /* Swap with another test pool.  */
  inline void
  swap (CTestPool& pool)
  {
    includedTx.swap (pool.includedTx);
    spent.swap (pool.spent);
  }

};


template <typename Stream>
int ReadWriteAuxPow(Stream& s, const boost::shared_ptr<CAuxPow>& auxpow, int nType, int nVersion, CSerActionSerialize ser_action);

template <typename Stream>
int ReadWriteAuxPow(Stream& s, boost::shared_ptr<CAuxPow>& auxpow, int nType, int nVersion, CSerActionUnserialize ser_action);

template <typename Stream>
int ReadWriteAuxPow(Stream& s, const boost::shared_ptr<CAuxPow>& auxpow, int nType, int nVersion, CSerActionGetSerializeSize ser_action);

enum
{
    // primary version
    // (1 << 0) ... not FORK_TIMESAVE capable
    // (1 << 1) ... FORK_TIMESAVE capable
    BLOCK_VERSION_DEFAULT        = (1 << 1),

    // modifiers
    BLOCK_VERSION_AUXPOW         = (1 << 8),
    BLOCK_VERSION_SCRYPT         = (1 << 9),

    // bits allocated for chain ID
    BLOCK_VERSION_CHAIN_START    = (1 << 16),
    BLOCK_VERSION_CHAIN_END      = (1 << 30),
};

inline int GetAlgo(int nVersion)
{
    return nVersion & BLOCK_VERSION_SCRYPT ? ALGO_SCRYPT : ALGO_SHA256D;
}


//
// Nodes collect new transactions into a block, hash them into a hash tree,
// and scan through nonce values to make the block's hash satisfy proof-of-work
// requirements.  When they solve the proof-of-work, they broadcast the block
// to everyone and the block is added to the block chain.  The first transaction
// in the block is a special one that creates a new coin owned by the creator
// of the block.
//
// Blocks are appended to blk0001.dat files on disk.  Their location on disk
// is indexed by CBlockIndex objects in memory.
//
class CBlock
{
public:
    // header
    int nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    unsigned int nTime;
    unsigned int nBits;
    unsigned int nNonce;

    // network and disk
    std::vector<CTransaction> vtx;

    // header
    boost::shared_ptr<CAuxPow> auxpow;

    // memory only
    mutable std::vector<uint256> vMerkleTree, vGameMerkleTree;
    
    // Game data
    uint256 hashGameMerkleRoot;            // disk, disk+header
    std::vector<CTransaction> vgametx;     // disk only
    unsigned int nGameTxFile, nGameTxPos;  // disk only

    CBlock()
    {
        SetNull();
    }

    // 0 - SHA-256d
    // 1 - scrypt
    int GetAlgo() const { return ::GetAlgo(nVersion); }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(hashPrevBlock);
        READWRITE(hashMerkleRoot);
        READWRITE(nTime);
        READWRITE(nBits);
        READWRITE(nNonce);

        nSerSize += ReadWriteAuxPow(s, auxpow, nType, nVersion, ser_action);

        if ((nType & SER_DISK) && (nType & SER_GETHASH))
            printf("CBlock serialization error: nType contains both SER_DISK and SER_GETHASH\n");

        if (nType & SER_DISK)
            READWRITE(hashGameMerkleRoot);
        else if (fRead)
            const_cast<CBlock*>(this)->hashGameMerkleRoot = 0;

        // ConnectBlock depends on vtx being last so it can calculate offset
        if (!(nType & SER_BLOCKHEADERONLY))
        {
            READWRITE(vtx);
            if (nType & SER_DISK)
            {
                // vgametx must not participate in hashing, because the game state may depend on the hash (it is used as random seed)
                // This is not a problem, because vgametx is re-created deterministically from the game state in ConnectBlock
                READWRITE(nGameTxFile);
                READWRITE(nGameTxPos);
            }
            else if (fRead)
            {
                const_cast<CBlock*>(this)->nGameTxFile = -1;
                const_cast<CBlock*>(this)->nGameTxPos = -1;
                const_cast<CBlock*>(this)->vgametx.clear();
            }
        }
        else if (fRead)
        {
            const_cast<CBlock*>(this)->vtx.clear();
            const_cast<CBlock*>(this)->nGameTxFile = -1;
            const_cast<CBlock*>(this)->nGameTxPos = -1;
            const_cast<CBlock*>(this)->vgametx.clear();
        }
    )

    int GetChainID() const
    {
        return nVersion / BLOCK_VERSION_CHAIN_START;
    }

    void SetAuxPow(CAuxPow* pow);

    void SetNull();

    bool IsNull() const
    {
        return (nBits == 0);
    }

    uint256 GetHash() const
    {
        return Hash(BEGIN(nVersion), END(nNonce));
    }

    // Note: we use explicitly provided algo instead of the one returned by GetAlgo(), because this can be a block
    // from foreign chain (parent block in merged mining) which does not encode algo in its nVersion field.
    uint256 GetPoWHash(int algo) const
    {
        if (algo == ALGO_SHA256D)
            return GetHash();
        else
        {
            uint256 thash;
            // Caution: scrypt_1024_1_1_256 assumes fixed length of 80 bytes
            scrypt_1024_1_1_256(BEGIN(nVersion), BEGIN(thash));
            return thash;
        }
    }

    int64 GetBlockTime() const
    {
        return (int64)nTime;
    }

    int GetSigOpCount() const
    {
        int n = 0;
        BOOST_FOREACH(const CTransaction& tx, vtx)
            n += tx.GetSigOpCount();
        return n;
    }


    uint256 BuildMerkleTree(bool game) const
    {
        std::vector<uint256> &vMerkleTree = game ? this->vGameMerkleTree : this->vMerkleTree;
        const std::vector<CTransaction> &vtx = game ? this->vgametx : this->vtx;
        vMerkleTree.clear();
        BOOST_FOREACH(const CTransaction& tx, vtx)
            vMerkleTree.push_back(tx.GetHash());
        int j = 0;
        for (int nSize = vtx.size(); nSize > 1; nSize = (nSize + 1) / 2)
        {
            for (int i = 0; i < nSize; i += 2)
            {
                int i2 = std::min(i+1, nSize-1);
                vMerkleTree.push_back(Hash(BEGIN(vMerkleTree[j+i]),  END(vMerkleTree[j+i]),
                                           BEGIN(vMerkleTree[j+i2]), END(vMerkleTree[j+i2])));
            }
            j += nSize;
        }
        return (vMerkleTree.empty() ? 0 : vMerkleTree.back());
    }

    std::vector<uint256> GetMerkleBranch(int nIndex, bool game) const
    {
        std::vector<uint256> &vMerkleTree = game ? this->vGameMerkleTree : this->vMerkleTree;
        const std::vector<CTransaction> &vtx = game ? this->vgametx : this->vtx;
        if (vMerkleTree.empty())
            BuildMerkleTree(game);
        std::vector<uint256> vMerkleBranch;
        int j = 0;
        for (int nSize = vtx.size(); nSize > 1; nSize = (nSize + 1) / 2)
        {
            int i = std::min(nIndex^1, nSize-1);
            vMerkleBranch.push_back(vMerkleTree[j+i]);
            nIndex >>= 1;
            j += nSize;
        }
        return vMerkleBranch;
    }

    static uint256 CheckMerkleBranch(uint256 hash, const std::vector<uint256>& vMerkleBranch, int nIndex)
    {
        if (nIndex == -1)
            return 0;
        BOOST_FOREACH(const uint256& otherside, vMerkleBranch)
        {
            if (nIndex & 1)
                hash = Hash(BEGIN(otherside), END(otherside), BEGIN(hash), END(hash));
            else
                hash = Hash(BEGIN(hash), END(hash), BEGIN(otherside), END(otherside));
            nIndex >>= 1;
        }
        return hash;
    }

    bool WriteToDisk(unsigned int& nFileRet, unsigned int& nBlockPosRet);

    bool CheckProofOfWork(int nHeight) const;

    bool ReadFromDisk(unsigned int nFile, unsigned int nBlockPos, bool fReadTransactions = true);

    void print() const
    {
        printf("CBlock(hash=%s, ver=%d, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%d)\n",
            GetHash().ToString().substr(0,20).c_str(),
            nVersion,
            hashPrevBlock.ToString().substr(0,20).c_str(),
            hashMerkleRoot.ToString().substr(0,10).c_str(),
            nTime, nBits, nNonce,
            vtx.size());
        for (int i = 0; i < vtx.size(); i++)
        {
            printf("  ");
            vtx[i].print();
        }
        printf("  vMerkleTree: ");
        for (int i = 0; i < vMerkleTree.size(); i++)
            printf("%s ", vMerkleTree[i].ToString().substr(0,10).c_str());
        if (!vgametx.empty())
        {
            printf("\n  vgametx: ");
            for (int i = 0; i < vgametx.size(); i++)
            {
                printf("  ");
                vgametx[i].print();
            }
            printf("  vGameMerkleTree (hashGameMerkleRoot=%s): ", hashGameMerkleRoot.ToString().substr(0,10).c_str());
            for (int i = 0; i < vGameMerkleTree.size(); i++)
                printf("%s ", vGameMerkleTree[i].ToString().substr(0,10).c_str());
        }

        printf("\n");
    }


    bool DisconnectBlock (DatabaseSet& dbset, CBlockIndex* pindex);
    bool ConnectBlock (DatabaseSet& dbset, CBlockIndex* pindex);
    bool ReadFromDisk(const CBlockIndex* pindex);
    bool SetBestChain (DatabaseSet& dbset, CBlockIndex* pindexNew);
    bool AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos);
    bool CheckBlock(int nHeight) const;
    bool AcceptBlock();

    /* Put all outpoints spent by this block into the set.  This is used
       to later remove transactions that are double-spends of them
       from the mempool in ClearDoubleSpendings.  */
    void GetSpentOutputs (std::set<COutPoint>& outs) const;

};






//
// The block chain is a tree shaped structure starting with the
// genesis block at the root, with each block potentially having multiple
// candidates to be the next block.  pprev and pnext link a path through the
// main/longest chain.  A blockindex may have multiple pprev pointing back
// to it, but pnext will only point forward to the longest branch, or will
// be null if the block is not part of the longest chain.
//
class CBlockIndex
{
public:
    const uint256* phashBlock;
    CBlockIndex* pprev;
    CBlockIndex* pnext;
    unsigned int nFile;
    unsigned int nBlockPos;
    int nHeight;
    CBigNum bnChainWork;

    // block header
    int nVersion;
    uint256 hashMerkleRoot, hashGameMerkleRoot;
    unsigned int nTime;
    unsigned int nBits;
    unsigned int nNonce;

    CBlockIndex()
    {
        phashBlock = NULL;
        pprev = NULL;
        pnext = NULL;
        nFile = 0;
        nBlockPos = 0;
        nHeight = 0;
        bnChainWork = 0;

        nVersion       = 0;
        hashMerkleRoot = 0;
        hashGameMerkleRoot = 0;
        nTime          = 0;
        nBits          = 0;
        nNonce         = 0;
    }

    CBlockIndex(unsigned int nFileIn, unsigned int nBlockPosIn, CBlock& block)
    {
        phashBlock = NULL;
        pprev = NULL;
        pnext = NULL;
        nFile = nFileIn;
        nBlockPos = nBlockPosIn;
        nHeight = 0;
        bnChainWork = 0;

        nVersion       = block.nVersion;
        hashMerkleRoot = block.hashMerkleRoot;
        hashGameMerkleRoot = block.hashGameMerkleRoot;
        nTime          = block.nTime;
        nBits          = block.nBits;
        nNonce         = block.nNonce;
    }

    /* GetBlockHeader is never actually used in the code, thus disable
       it since it can't reliably be tested.  It can be re-enabled
       when necessary later.  */
    CBlock GetBlockHeader() const
    {
        CBlock block;
        if (!block.ReadFromDisk (nFile, nBlockPos, false))
          throw std::runtime_error ("CBlock::ReadFromDisk failed while"
                                    " retrieving auxpow");

        return block;
    }
    
    // 0 - SHA-256d
    // 1 - scrypt
    int GetAlgo() const { return ::GetAlgo(nVersion); }

    uint256 GetBlockHash() const
    {
        return *phashBlock;
    }

    int64 GetBlockTime() const
    {
        return (int64)nTime;
    }

    CBigNum GetBlockWork() const;

    bool IsInMainChain() const
    {
        return (pnext || this == pindexBest);
    }

    enum { nMedianTimeSpan=11 };

    int64 GetMedianTimePast() const
    {
        int64 pmedian[nMedianTimeSpan];
        int64* pbegin = &pmedian[nMedianTimeSpan];
        int64* pend = &pmedian[nMedianTimeSpan];

        const CBlockIndex* pindex = this;
        for (int i = 0; i < nMedianTimeSpan && pindex; i++, pindex = pindex->pprev)
            *(--pbegin) = pindex->GetBlockTime();

        std::sort(pbegin, pend);
        return pbegin[(pend - pbegin)/2];
    }

    int64 GetMedianTime() const
    {
        const CBlockIndex* pindex = this;
        for (int i = 0; i < nMedianTimeSpan/2; i++)
        {
            if (!pindex->pnext)
                return GetBlockTime();
            pindex = pindex->pnext;
        }
        return pindex->GetMedianTimePast();
    }

    /* Calculate total block rewards up to this one, including the genesis
       block premine and coins put on the map.  */
    int64 GetTotalRewards () const;

    std::string ToString() const;

    void print() const
    {
        printf("%s\n", ToString().c_str());
    }
};



//
// Used to marshal pointers into hashes for db storage.
//
class CDiskBlockIndex : public CBlockIndex
{
public:
    uint256 hashPrev;
    uint256 hashNext;

    CDiskBlockIndex()
    {
        hashPrev = 0;
        hashNext = 0;
    }

    explicit CDiskBlockIndex(CBlockIndex* pindex) : CBlockIndex(*pindex)
    {
        hashPrev = (pprev ? pprev->GetBlockHash() : 0);
        hashNext = (pnext ? pnext->GetBlockHash() : 0);
    }

    IMPLEMENT_SERIALIZE
    (
        /* This is only written to disk.  */
        assert (nType & SER_DISK);
        /* If the version is not up-to-date (with the latest format change
           for this class), then it means we're upgrading and thus reading
           and old-format entry.  */
        assert (nVersion >= 1000800 || fRead);

        /* Previously, the version was stored in each entry.  This is
           now replaced with having serialisation version set.  In the old
           format, read and ignore the version.  */
        if (nVersion < 1000800)
          {
            assert (fRead);
            int nDummyVersion;
            READWRITE(nDummyVersion);
            assert (nDummyVersion < 1000800);
          }

        READWRITE(hashNext);
        READWRITE(nFile);
        READWRITE(nBlockPos);
        READWRITE(nHeight);

        // block header
        READWRITE(this->nVersion);
        READWRITE(hashPrev);
        READWRITE(hashMerkleRoot);
        READWRITE(hashGameMerkleRoot);
        READWRITE(nTime);
        READWRITE(nBits);
        READWRITE(nNonce);

        /* In the old format, the auxpow is stored.  Load it and ignore.  */
        if (nVersion < 1000800)
          {
            assert (fRead);
            boost::shared_ptr<CAuxPow> auxpow;
            ReadWriteAuxPow (s, auxpow, nType, this->nVersion, ser_action);
          }
    )

    uint256 GetBlockHash() const
    {
        CBlock block;
        block.nVersion        = nVersion;
        block.hashPrevBlock   = hashPrev;
        block.hashMerkleRoot  = hashMerkleRoot;
        block.nTime           = nTime;
        block.nBits           = nBits;
        block.nNonce          = nNonce;
        return block.GetHash();
    }


    std::string ToString() const
    {
        std::string str = "CDiskBlockIndex(";
        str += CBlockIndex::ToString();
        str += strprintf("\n                hashBlock=%s, hashPrev=%s, hashNext=%s)",
            GetBlockHash().ToString().c_str(),
            hashPrev.ToString().substr(0,20).c_str(),
            hashNext.ToString().substr(0,20).c_str());
        return str;
    }

    void print() const
    {
        printf("%s\n", ToString().c_str());
    }
};








//
// Describes a place in the block chain to another node such that if the
// other node doesn't have the same branch, it can find a recent common trunk.
// The further back it is, the further before the fork it may be.
//
class CBlockLocator
{
protected:
    std::vector<uint256> vHave;
public:

    CBlockLocator()
    {
    }

    explicit CBlockLocator(const CBlockIndex* pindex)
    {
        Set(pindex);
    }

    explicit CBlockLocator(uint256 hashBlock)
    {
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end())
            Set((*mi).second);
    }

    IMPLEMENT_SERIALIZE
    (
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(vHave);
    )

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull()
    {
        return vHave.empty();
    }

    void Set(const CBlockIndex* pindex)
    {
        vHave.clear();
        int nStep = 1;
        while (pindex)
        {
            vHave.push_back(pindex->GetBlockHash());

            // Exponentially larger steps back
            for (int i = 0; pindex && i < nStep; i++)
                pindex = pindex->pprev;
            if (vHave.size() > 10)
                nStep *= 2;
        }
        vHave.push_back(hashGenesisBlock);
    }

    int GetDistanceBack()
    {
        // Retrace how far back it was in the sender's branch
        int nDistance = 0;
        int nStep = 1;
        BOOST_FOREACH(const uint256& hash, vHave)
        {
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;
                if (pindex->IsInMainChain())
                    return nDistance;
            }
            nDistance += nStep;
            if (nDistance > 10)
                nStep *= 2;
        }
        return nDistance;
    }

    CBlockIndex* GetBlockIndex()
    {
        // Find the first block the caller has in the main chain
        BOOST_FOREACH(const uint256& hash, vHave)
        {
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;
                if (pindex->IsInMainChain())
                    return pindex;
            }
        }
        return pindexGenesisBlock;
    }

    uint256 GetBlockHash()
    {
        // Find the first block the caller has in the main chain
        BOOST_FOREACH(const uint256& hash, vHave)
        {
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;
                if (pindex->IsInMainChain())
                    return hash;
            }
        }
        return hashGenesisBlock;
    }

    int GetHeight()
    {
        CBlockIndex* pindex = GetBlockIndex();
        if (!pindex)
            return 0;
        return pindex->nHeight;
    }
};









//
// Alerts are for notifying old versions if they become too obsolete and
// need to upgrade.  The message is displayed in the status bar.
// Alert messages are broadcast as a vector of signed data.  Unserializing may
// not read the entire buffer if the alert is for a newer version, but older
// versions can still relay the original data.
//
class CUnsignedAlert
{
public:
    int nVersion;
    int64 nRelayUntil;      // when newer nodes stop relaying to newer nodes
    int64 nExpiration;
    int nID;
    int nCancel;
    std::set<int> setCancel;
    int nMinVer;            // lowest version inclusive
    int nMaxVer;            // highest version inclusive
    std::set<std::string> setSubVer;  // empty matches all
    int nPriority;

    // Actions
    std::string strComment;
    std::string strStatusBar;
    std::string strReserved;

    IMPLEMENT_SERIALIZE
    (
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(nRelayUntil);
        READWRITE(nExpiration);
        READWRITE(nID);
        READWRITE(nCancel);
        READWRITE(setCancel);
        READWRITE(nMinVer);
        READWRITE(nMaxVer);
        READWRITE(setSubVer);
        READWRITE(nPriority);

        READWRITE(strComment);
        READWRITE(strStatusBar);
        READWRITE(strReserved);
    )

    void SetNull()
    {
        nVersion = 1;
        nRelayUntil = 0;
        nExpiration = 0;
        nID = 0;
        nCancel = 0;
        setCancel.clear();
        nMinVer = 0;
        nMaxVer = 0;
        setSubVer.clear();
        nPriority = 0;

        strComment.clear();
        strStatusBar.clear();
        strReserved.clear();
    }

    std::string ToString() const
    {
        std::string strSetCancel;
        BOOST_FOREACH(int n, setCancel)
            strSetCancel += strprintf("%d ", n);
        std::string strSetSubVer;
        BOOST_FOREACH(std::string str, setSubVer)
            strSetSubVer += "\"" + str + "\" ";
        return strprintf(
                "CAlert(\n"
                "    nVersion     = %d\n"
                "    nRelayUntil  = %"PRI64d"\n"
                "    nExpiration  = %"PRI64d"\n"
                "    nID          = %d\n"
                "    nCancel      = %d\n"
                "    setCancel    = %s\n"
                "    nMinVer      = %d\n"
                "    nMaxVer      = %d\n"
                "    setSubVer    = %s\n"
                "    nPriority    = %d\n"
                "    strComment   = \"%s\"\n"
                "    strStatusBar = \"%s\"\n"
                ")\n",
            nVersion,
            nRelayUntil,
            nExpiration,
            nID,
            nCancel,
            strSetCancel.c_str(),
            nMinVer,
            nMaxVer,
            strSetSubVer.c_str(),
            nPriority,
            strComment.c_str(),
            strStatusBar.c_str());
    }

    void print() const
    {
        printf("%s", ToString().c_str());
    }
};

class CAlert : public CUnsignedAlert
{
public:
    std::vector<unsigned char> vchMsg;
    std::vector<unsigned char> vchSig;

    CAlert()
    {
        SetNull();
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(vchMsg);
        READWRITE(vchSig);
    )

    void SetNull()
    {
        CUnsignedAlert::SetNull();
        vchMsg.clear();
        vchSig.clear();
    }

    bool IsNull() const
    {
        return (nExpiration == 0);
    }

    uint256 GetHash() const
    {
        return SerializeHash(*this);
    }

    bool IsInEffect() const
    {
        return (GetAdjustedTime() < nExpiration);
    }

    bool Cancels(const CAlert& alert) const
    {
        if (!IsInEffect())
            return false; // this was a no-op before 31403
        return (alert.nID <= nCancel || setCancel.count(alert.nID));
    }

    bool AppliesTo(int nVersion, std::string strSubVerIn) const
    {
        return (IsInEffect() &&
                nMinVer <= nVersion && nVersion <= nMaxVer &&
                (setSubVer.empty() || setSubVer.count(strSubVerIn)));
    }

    bool AppliesToMe() const
    {
        return AppliesTo(VERSION, ::pszSubVer);
    }

    bool RelayTo(CNode* pnode) const
    {
        if (!IsInEffect())
            return false;
        // returns true if wasn't already contained in the set
        if (pnode->setKnown.insert(GetHash()).second)
        {
            if (AppliesTo(pnode->nVersion, pnode->strSubVer) ||
                AppliesToMe() ||
                GetAdjustedTime() < nRelayUntil)
            {
                pnode->PushMessage("alert", *this);
                return true;
            }
        }
        return false;
    }

    bool CheckSignature();

    bool ProcessAlert();
};









extern std::map<uint256, CTransaction> mapTransactions;
extern CHooks* hooks;

void MineGenesisBlock(CBlock *pblock, bool fUpdateBlockTime = true);
int64 GetBlockValue(int nHeight, int64 nFees);

#endif
