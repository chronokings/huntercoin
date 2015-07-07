// Copyright (c) 2009-2010 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_DB_H
#define BITCOIN_DB_H

#include "key.h"

#include <map>
#include <string>
#include <vector>

#include <db_cxx.h>

class CNameIndex;
class CTxIndex;
class CUtxoEntry;
class COutPoint;
class CDiskBlockIndex;
class CDiskTxPos;
class COutPoint;
class CAddress;
class CWalletTx;
class CWallet;
class CAccount;
class CAccountingEntry;
class CBlockLocator;


extern unsigned int nWalletDBUpdated;
extern DbEnv dbenv;
extern bool fDetachDB;

extern void DBFlush(bool fShutdown);
void ThreadFlushWalletDB(void* parg);
bool BackupWallet(const CWallet& wallet, const std::string& strDest);
void PrintSettingsToLog();



class CDB
{
protected:
    Db* pdb;
    std::string strFile;

    /* Keep track of the database transactions open.  Also remember for
       each one whether or not it is owned by us or was instead passed
       to TxnBegin.  If it is the latter, it should not be committed/aborted
       but only removed from the vector when done.  */
    std::vector<DbTxn*> vTxn;
    std::vector<bool> ownTxn;

    bool fReadOnly;

    /* Store version of the DB here that will be set as version
       for serialisation on the streams.  */
    int nVersion;

    explicit CDB(const char* pszFile, const char* pszMode="r+");
    ~CDB() { Close(); }
public:
    void Close();
private:
    CDB(const CDB&);
    void operator=(const CDB&);

protected:
    template<typename K, typename T>
    bool Read(const K& key, T& value)
    {
        if (!pdb)
            return false;

        // Key
        CDataStream ssKey(SER_DISK, nVersion);
        ssKey.reserve(1000);
        ssKey << key;
        Dbt datKey(&ssKey[0], ssKey.size());

        // Read
        Dbt datValue;
        datValue.set_flags(DB_DBT_MALLOC);
        int ret = pdb->get(GetTxn(), &datKey, &datValue, 0);
        memset(datKey.get_data(), 0, datKey.get_size());
        if (datValue.get_data() == NULL)
            return false;

        // Unserialize value
        CDataStream ssValue((char*)datValue.get_data(),
                            (char*)datValue.get_data() + datValue.get_size(),
                            SER_DISK, nVersion);
        ssValue >> value;

        // Clear and free memory
        memset(datValue.get_data(), 0, datValue.get_size());
        free(datValue.get_data());
        return (ret == 0);
    }

    template<typename K, typename T>
    bool Write(const K& key, const T& value, bool fOverwrite=true)
    {
        if (!pdb)
            return false;
        if (fReadOnly)
            assert(("Write called on database in read-only mode", false));

        // Key
        CDataStream ssKey(SER_DISK, nVersion);
        ssKey.reserve(1000);
        ssKey << key;
        Dbt datKey(&ssKey[0], ssKey.size());

        // Value
        CDataStream ssValue(SER_DISK, nVersion);
        ssValue.reserve(10000);
        ssValue << value;
        Dbt datValue(&ssValue[0], ssValue.size());

        // Write
        int ret = pdb->put(GetTxn(), &datKey, &datValue, (fOverwrite ? 0 : DB_NOOVERWRITE));

        // Clear memory in case it was a private key
        memset(datKey.get_data(), 0, datKey.get_size());
        memset(datValue.get_data(), 0, datValue.get_size());
        return (ret == 0);
    }

    template<typename K>
    bool Erase(const K& key)
    {
        if (!pdb)
            return false;
        if (fReadOnly)
            assert(("Erase called on database in read-only mode", false));

        // Key
        CDataStream ssKey(SER_DISK, nVersion);
        ssKey.reserve(1000);
        ssKey << key;
        Dbt datKey(&ssKey[0], ssKey.size());

        // Erase
        int ret = pdb->del(GetTxn(), &datKey, 0);

        // Clear memory
        memset(datKey.get_data(), 0, datKey.get_size());
        return (ret == 0 || ret == DB_NOTFOUND);
    }

    template<typename K>
    bool Exists(const K& key)
    {
        if (!pdb)
            return false;

        // Key
        CDataStream ssKey(SER_DISK, nVersion);
        ssKey.reserve(1000);
        ssKey << key;
        Dbt datKey(&ssKey[0], ssKey.size());

        // Exists
        int ret = pdb->exists(GetTxn(), &datKey, 0);

        // Clear memory
        memset(datKey.get_data(), 0, datKey.get_size());
        return (ret == 0);
    }

    Dbc* GetCursor()
    {
        if (!pdb)
            return NULL;
        Dbc* pcursor = NULL;
        int ret = pdb->cursor(NULL, &pcursor, 0);
        if (ret != 0)
            return NULL;
        return pcursor;
    }

    int ReadAtCursor(Dbc* pcursor, CDataStream& ssKey, CDataStream& ssValue, unsigned int fFlags=DB_NEXT)
    {
        // Read at cursor
        Dbt datKey;
        if (fFlags == DB_SET || fFlags == DB_SET_RANGE || fFlags == DB_GET_BOTH || fFlags == DB_GET_BOTH_RANGE)
        {
            datKey.set_data(&ssKey[0]);
            datKey.set_size(ssKey.size());
        }
        Dbt datValue;
        if (fFlags == DB_GET_BOTH || fFlags == DB_GET_BOTH_RANGE)
        {
            datValue.set_data(&ssValue[0]);
            datValue.set_size(ssValue.size());
        }
        datKey.set_flags(DB_DBT_MALLOC);
        datValue.set_flags(DB_DBT_MALLOC);
        int ret = pcursor->get(&datKey, &datValue, fFlags);
        if (ret != 0)
            return ret;
        else if (datKey.get_data() == NULL || datValue.get_data() == NULL)
            return 99999;

        // Convert to streams
        ssKey.SetType(SER_DISK);
        ssKey.clear();
        ssKey.write((char*)datKey.get_data(), datKey.get_size());
        ssValue.SetType(SER_DISK);
        ssValue.clear();
        ssValue.write((char*)datValue.get_data(), datValue.get_size());

        // Clear and free memory
        memset(datKey.get_data(), 0, datKey.get_size());
        memset(datValue.get_data(), 0, datValue.get_size());
        free(datKey.get_data());
        free(datValue.get_data());
        return 0;
    }

    /* Update the stream to our serialisation version.  This is useful
       for ReadAtCursor users.  */
    inline void
    SetStreamVersion (CDataStream& ss) const
    {
      ss.nVersion = nVersion;
    }

public:
    DbTxn* GetTxn()
    {
        if (!vTxn.empty())
            return vTxn.back();
        else
            return NULL;
    }

    /* Start a new atomic DB transaction.  Optionally use the passed one
       instead, which can be used to synchronise between multiple DBs.  */
    inline bool
    TxnBegin (DbTxn* ptxn = NULL)
    {
      const bool own = (ptxn == NULL);

      if (!pdb)
        return false;
      if (!ptxn)
        {
          const int ret = dbenv.txn_begin (GetTxn (), &ptxn, DB_TXN_NOSYNC);
          if (!ptxn || ret != 0)
            return false;
        }
      vTxn.push_back (ptxn);
      ownTxn.push_back (own);

      return true;
    }

    bool TxnCommit()
    {
        if (!pdb)
            return false;
        if (vTxn.empty())
            return false;

        int ret = 0;
        if (ownTxn.back ())
          ret = vTxn.back()->commit(0);
        vTxn.pop_back();
        ownTxn.pop_back ();
        return (ret == 0);
    }

    bool TxnAbort()
    {
        if (!pdb)
            return false;
        if (vTxn.empty())
            return false;

        int ret = 0;
        if (ownTxn.back ())
          ret = vTxn.back()->abort();
        vTxn.pop_back();
        ownTxn.pop_back ();
        return (ret == 0);
    }

    bool ReadVersion(int& nVersion)
    {
        nVersion = 0;
        return Read(std::string("version"), nVersion);
    }

    bool WriteVersion(int nVersion)
    {
        return Write(std::string("version"), nVersion);
    }

    /**
     * Set version for stream serialisation.
     * @param v Version to use for serialisation streams.
     */
    inline void
    SetSerialisationVersion (int v)
    {
      nVersion = v;
    }
    
    bool static Rewrite(const std::string& strFile);

    /* Rewrite the DB with this database's name.  This closes it.  */
    inline bool
    Rewrite ()
    {
      Close ();
      Rewrite (strFile);
    }

    /**
     * Print some storage stats about the database file for debugging
     * purposes.
     * @param file Database file to analyse.
     */
    static void PrintStorageStats (const std::string& file);

};








class CTxDB : public CDB
{
public:
    CTxDB(const char* pszMode="r+") : CDB("blkindex.dat", pszMode) { }
private:
    CTxDB(const CTxDB&);
    void operator=(const CTxDB&);

    /* The txindex is immutable (only storing disk pos which doesn't change)
       during normal operation, but for updating the storage format
       we need this internally.  */
    bool UpdateTxIndex (const uint256& hash, const CTxIndex& txindex);

public:
    bool ReadTxIndex(uint256 hash, CTxIndex& txindex);
    bool AddTxIndex(const CTransaction& tx, const CDiskTxPos& pos);
    bool EraseTxIndex(const CTransaction& tx);
    bool ContainsTx(uint256 hash);
    bool ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex);
    bool ReadDiskTx(uint256 hash, CTransaction& tx);
    bool ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex);
    bool ReadDiskTx(COutPoint outpoint, CTransaction& tx);
    bool WriteBlockIndex(const CDiskBlockIndex& blockindex);
    bool EraseBlockIndex(uint256 hash);
    bool ReadHashBestChain(uint256& hashBestChain);
    bool WriteHashBestChain(uint256 hashBestChain);
    bool ReadBestInvalidWork(CBigNum& bnBestInvalidWork);
    bool WriteBestInvalidWork(CBigNum bnBestInvalidWork);

    /* Read/write number of "reserved" (but not yet used) bytes in the
       block files.  */
    unsigned ReadBlockFileReserved (unsigned num);
    bool WriteBlockFileReserved (unsigned num, unsigned size);

    bool LoadBlockIndex();

    /* Update txindex to new data format.  */
    bool RewriteTxIndex (int oldVersion);
};







/**
 * Name index.  Non-inline implementation code is in namecoin.cpp, but the
 * class is declared here because it will be used for the "wrapper" database
 * set class below and in general makes sense here.
 */
class CNameDB : public CDB
{
private:

    inline bool
    EraseName (const vchType& name)
    {
      return Erase (std::make_pair (std::string("namei"), name));
    }

    inline bool
    WriteName (const vchType& name, const std::vector<CNameIndex>& vtxPos)
    {
      if (vtxPos.empty ())
        return EraseName (name);

      return Write (std::make_pair (std::string("namei"), name), vtxPos);
    }

public:

    explicit inline CNameDB (const char* pszMode="r+")
      : CDB("nameindexfull.dat", pszMode)
    {}

    /* This is the main interface for reading the name index.  It returns
       the active CNameIndex object.  */
    bool ReadName (const vchType& name, CNameIndex& nidx);

    /* Return all states of the name in the index.  This is used for
       name_history but nothing else (except things like name_debug1 which
       do not really matter).  It may be incomplete if some
       entries have been pruned.  */
    inline bool
    ReadNameVec (const vchType& name, std::vector<CNameIndex>& vtxPos)
    {
      return Read (std::make_pair (std::string("namei"), name), vtxPos);
    }

    bool ExistsName(const vchType& name)
    {
        return Exists(make_pair(std::string("namei"), name));
    }

    /* Add a new CNameIndex entry for the given name.  This either creates
       a completely new entry, or pushes the value onto the vector stored
       in the database.  */
    bool PushEntry (const vchType& name, const CNameIndex& value);

    /* Remove CNameIndex entries (when rolling back the chain) up until
       (including) a given height.  It should usually be the case that
       the given height matches exactly the currently last entry.
       But we do not insist on that.  */
    bool PopEntry (const vchType& name, int nHeight);

    /* Read and write information about what heights have been pruned.  The
       value here is the earliest height which has not been removed.  It is -1
       if no pruning has been done so far.  */
    int ReadPrunedHeight ();
    bool WritePrunedHeight (int nHeight);

    /* Prune all index entries older than the given height.  */
    void Prune (unsigned nHeight);

    bool ScanNames(
            const vchType& vchName,
            int nMax,
            std::vector<std::pair<vchType, CNameIndex> >& nameScan);

    bool ReconstructNameIndex();
};





class CUtxoDB : public CDB
{
public:
    CUtxoDB(const char* pszMode="r+") : CDB("utxo.dat", pszMode) { }

    /** Set of COutPoints as used in the DB verification.  */
    typedef std::set<COutPoint> OutPointSet;

private:
    CUtxoDB(const CUtxoDB&);
    void operator=(const CUtxoDB&);

    /** Type used as key into the DB.  */
    typedef std::pair<std::string, COutPoint> KeyType;

    /* Construct the look-up key for a given COutPoint.  This just prepends
       the key-string "txo" to it.  */
    KeyType GetKey (const COutPoint& pos);

    /* Internal routine that shares code for scanning all transactions
       in the blockchain.  It can be used (depending on the passed flags)
       to build the UTXO DB from it, or to verify it against the
       blockchain.  The caller must hold the cs_main lock in any case.  */
    bool InternalRescan (bool fVerify, OutPointSet* outPoints = NULL);

public:

    /* Try to look up an entry in the UTXO set.  Return true if it is found
       (i. e., still unspent).  If this is the case, the CTxOut will be set
       accordingly.  */
    bool ReadUtxo (const COutPoint& pos, CUtxoEntry& txo);

    /* Insert an entry into the UTXO set.  This assumes that it doesn't already
       exist, and gives an internal error if it does.  */
    bool InsertUtxo (const COutPoint& pos, const CUtxoEntry& txo);
    bool InsertUtxo (const CTransaction& tx, unsigned n, int height);
    bool InsertUtxo (const CTransaction& tx, int height);

    /* Remove (i. e., mark spent) a given output.  */
    bool RemoveUtxo (const COutPoint& pos);
    /* Remove an entire transaction.  This is used to disconnect blocks.  */
    bool RemoveUtxo (const CTransaction& tx);

    /* Rescan the blockchain to build the UTXO set from scratch.  */
    bool Rescan ();

    /* Scan the blockchain and verify that the UTXO set in the database
       is correct.  This is used to check that the updating in ConnectBlock
       and DisconnectBlock works as it should.  */
    bool Verify ();

    /* Read all entries to analyse the total money supply as well as
       the number of entries.  */
    bool Analyse (unsigned& nUtxo, int64_t& amount, int64_t& inNames);

    /* Return UTXO set.  */
    bool GetUtxoSet (OutPointSet& res);
};





/**
 * Multiple databases (blkindex, nameindex) are used to represent the "current
 * blockchain state".  They are updated during block connecting/disconnecting,
 * and this should happen atomically.  This class encapsulates all those
 * databases into a single object for simplicity.
 */
class DatabaseSet
{

private:

  CTxDB txDb;
  CUtxoDB utxoDb;
  CNameDB nameDb;

public:

  inline DatabaseSet (const char* pszMode = "r+")
    : txDb(pszMode), utxoDb(pszMode), nameDb(pszMode)
  {}

  /* Expose the bundled databases.  */

  inline CTxDB&
  tx ()
  {
    return txDb;
  }

  inline CUtxoDB&
  utxo ()
  {
    return utxoDb;
  }

  inline CNameDB&
  name ()
  {
    return nameDb;
  }

  /* Transaction handling.  */

  inline bool
  TxnBegin ()
  {
    if (!txDb.TxnBegin ())
      return false;
    if (!utxoDb.TxnBegin (txDb.GetTxn ()))
      return error ("Failed to start child transaction in UTXO-DB!");
    if (!nameDb.TxnBegin (txDb.GetTxn ()))
      return error ("Failed to start child transaction in NameDB!");

    return true;
  }

  inline bool
  TxnAbort ()
  {
    if (!nameDb.TxnAbort ())
      return error ("Failed to abort child transaction in NameDB!");
    if (!utxoDb.TxnAbort ())
      return error ("Failed to abort child transaction in UTXO-DB!");
    return txDb.TxnAbort ();
  }

  inline bool
  TxnCommit ()
  {
    if (!nameDb.TxnCommit ())
      return error ("Failed to commit child transaction in NameDB!");
    if (!utxoDb.TxnCommit ())
      return error ("Failed to commit child transaction in UTXO-DB!");
    return txDb.TxnCommit ();
  }

};






class CAddrDB : public CDB
{
public:
    CAddrDB(const char* pszMode="r+") : CDB("addr.dat", pszMode) { }
private:
    CAddrDB(const CAddrDB&);
    void operator=(const CAddrDB&);
public:
    bool WriteAddress(const CAddress& addr);
    bool EraseAddress(const CAddress& addr);
    bool LoadAddresses();
};

bool LoadAddresses();



class CKeyPool
{
public:
    int64 nTime;
    std::vector<unsigned char> vchPubKey;

    CKeyPool()
    {
        nTime = GetTime();
    }

    CKeyPool(const std::vector<unsigned char>& vchPubKeyIn)
    {
        nTime = GetTime();
        vchPubKey = vchPubKeyIn;
    }

    IMPLEMENT_SERIALIZE
    (
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(nTime);
        READWRITE(vchPubKey);
    )
};

#endif
