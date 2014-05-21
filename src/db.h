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

class CTxIndex;
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


extern void DBFlush(bool fShutdown);
void ThreadFlushWalletDB(void* parg);
bool BackupWallet(const CWallet& wallet, const std::string& strDest);
void PrintSettingsToLog();



class CDB
{
protected:

    /* Allow transparent compression/decompression when writing/reading
       the DB values.  The database stores the compression settings used
       in a special "compression" value, similar to the "version" one.  The
       values below are the ones possible.  "version" and "compression"
       themselves are not compressed.  */

    /* No compression at all, this is the old format and the default.  */
    static const int COMPRESS_NONE = 0;
    /* Compress using zlib's compress/uncompress functions, but only if
       it gives a benefit.  Each value is prefixed with four bytes containing
       the uncompressed length.  If it is equal to the remaining data size,
       then no compression was used.  If it is greater, then decompress
       the remaining data using zlib.  */
    static const int COMPRESS_ZLIB = 1;

private:

    /* This database's compression setting.  */
    int compression;
    /* Desired compression setting.  */
    int desiredCompression;

protected:
    Db* pdb;
    std::string strFile;
    std::vector<DbTxn*> vTxn;
    bool fReadOnly;

    /* Store version of the DB here that will be set as version
       for serialisation on the streams.  */
    int nVersion;

    explicit CDB(const char* pszFile, const char* pszMode = "r+",
                 int comp = COMPRESS_NONE);
    ~CDB() { Close(); }
public:
    void Close();
private:
    CDB(const CDB&);
    void operator=(const CDB&);

    /**
     * Check if a data stream key matches a given string.
     * @param ssKey Key stream.
     * @param str Literal string value.
     * @return True iff the key matches the given string.
     */
    static inline bool
    CompareKeyString (const CDataStream& ssKey, const char* str)
    {
      const size_t len = strlen (str);
      assert (len > 0);
      if (ssKey.size () != len + 1 || ssKey[0] != len)
        return false;

      return (strncmp (&ssKey[1], str, len) == 0);
    }

    /**
     * Get the compression setting to use for the given key.  If it is
     * "compression", always use COMPRESS_NONE.  Otherwise, use the
     * given setting.
     * @param sKey Key stream.
     * @param comp Compression setting to use in the default case.
     * @return comp if the key is not "compress", COMPRESS_NONE if it is.
     */
    static inline bool
    UseCompression (const CDataStream& ssKey, int comp)
    {
      if (CompareKeyString (ssKey, "compression"))
        return COMPRESS_NONE;
      return comp;
    }

    /**
     * Uncompress data from a Dbt value into a CDataStream.
     * @param in Dbt record containing the data.
     * @param out CDataStream will be initialised.
     * @param comp Compression setting.
     */
    static void Uncompress (const Dbt& in, CDataStream& out, int comp);

    /**
     * Compress data from a CDataStream to a Dbt value.
     * @param in CDataStream containing the data to compress.
     * @param out Dbt value to initialise.
     * @param comp Compression setting.
     */
    static void Compress (const CDataStream& in, Dbt& out, int comp);

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
        const int usedComp = UseCompression (ssKey, compression);
        Dbt datKey(&ssKey[0], ssKey.size());

        // Read
        Dbt datValue;
        datValue.set_flags(DB_DBT_MALLOC);
        int ret = pdb->get(GetTxn(), &datKey, &datValue, 0);
        memset(datKey.get_data(), 0, datKey.get_size());
        if (datValue.get_data() == NULL)
            return false;

        // Unserialize value
        CDataStream ssValue(SER_DISK, nVersion);
        Uncompress (datValue, ssValue, usedComp);
        ssValue >> value;

        // Clear and free memory
        memset(datValue.get_data(), 0, datValue.get_size());
        free(datValue.get_data());
        return (ret == 0);
    }

    template<typename K, typename T>
    bool Write(const K& key, const T& value, bool fOverwrite = true)
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
        Dbt datValue;
        Compress (ssValue, datValue, UseCompression (ssKey, compression));

        // Write
        int ret = pdb->put(GetTxn(), &datKey, &datValue, (fOverwrite ? 0 : DB_NOOVERWRITE));

        // Clear memory in case it was a private key
        memset(datKey.get_data(), 0, datKey.get_size());
        memset(datValue.get_data(), 0, datValue.get_size());

        // Free memory in datValue, which is a copy (because it needs to be
        // modified in case of compression).  It is allocated in Compress.
        delete[] static_cast<char*> (datValue.get_data ());

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
        Uncompress (datValue, ssValue, UseCompression (ssKey, compression));

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

    bool TxnBegin()
    {
        if (!pdb)
            return false;
        DbTxn* ptxn = NULL;
        int ret = dbenv.txn_begin(GetTxn(), &ptxn, DB_TXN_NOSYNC);
        if (!ptxn || ret != 0)
            return false;
        vTxn.push_back(ptxn);
        return true;
    }

    bool TxnCommit()
    {
        if (!pdb)
            return false;
        if (vTxn.empty())
            return false;
        int ret = vTxn.back()->commit(0);
        vTxn.pop_back();
        return (ret == 0);
    }

    bool TxnAbort()
    {
        if (!pdb)
            return false;
        if (vTxn.empty())
            return false;
        int ret = vTxn.back()->abort();
        vTxn.pop_back();
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
    
    bool static Rewrite(const std::string& strFile,
                        int desiredComp = COMPRESS_NONE);

    /* Rewrite the DB with this database's name.  This closes it.  */
    inline bool
    Rewrite ()
    {
      Close ();
      Rewrite (strFile);
    }

};








class CTxDB : public CDB
{
public:
    CTxDB(const char* pszMode="r+") : CDB("blkindex.dat", pszMode) { }
private:
    CTxDB(const CTxDB&);
    void operator=(const CTxDB&);
public:
    bool ReadTxIndex(uint256 hash, CTxIndex& txindex);
    bool UpdateTxIndex(uint256 hash, const CTxIndex& txindex);
    bool AddTxIndex(const CTransaction& tx, const CDiskTxPos& pos, int nHeight);
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
    bool LoadBlockIndex();
    bool FixTxIndexBug();
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
