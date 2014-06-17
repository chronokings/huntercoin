// Copyright (c) 2009-2010 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include "headers.h"
#include "db.h"
#include "net.h"
#include "auxpow.h" // Fixes a linker issue with GCC > 4.7.
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

using namespace std;
using namespace boost;


unsigned int nWalletDBUpdated;



//
// CDB
//

static CCriticalSection cs_db;
static bool fDbEnvInit = false;
DbEnv dbenv(0);
static map<string, int> mapFileUseCount;
static map<string, Db*> mapDb;

class CDBInit
{
public:
    CDBInit()
    {
    }
    ~CDBInit()
    {
        if (fDbEnvInit)
        {
            dbenv.close(0);
            fDbEnvInit = false;
        }
    }
}
instance_of_cdbinit;


CDB::CDB(const char* pszFile, const char* pszMode)
  : pdb(NULL), nVersion(VERSION)
{
    int ret;
    if (pszFile == NULL)
        return;

    fReadOnly = (!strchr(pszMode, '+') && !strchr(pszMode, 'w'));
    bool fCreate = strchr(pszMode, 'c');
    unsigned int nFlags = DB_THREAD;
    if (fCreate)
        nFlags |= DB_CREATE;

    CRITICAL_BLOCK(cs_db)
    {
        if (!fDbEnvInit)
        {
            if (fShutdown)
                return;
            string strDataDir = GetDataDir();
            string strLogDir = strDataDir + "/database";
            filesystem::create_directory(strLogDir.c_str());
            string strErrorFile = strDataDir + "/db.log";
            printf("dbenv.open strLogDir=%s strErrorFile=%s\n", strLogDir.c_str(), strErrorFile.c_str());

            int nDbCache = GetArg("-dbcache", 25);
            dbenv.set_lg_dir(strLogDir.c_str());
            dbenv.set_cachesize(nDbCache / 1024, (nDbCache % 1024)*1048576, 1);
            dbenv.set_lg_bsize(10485760);
            dbenv.set_lg_max(104857600);
            dbenv.set_lk_max_locks(1000000);
            dbenv.set_lk_max_objects(1000000);
            dbenv.set_errfile(fopen(strErrorFile.c_str(), "a")); /// debug
            dbenv.set_flags(DB_AUTO_COMMIT, 1);
            dbenv.set_flags(DB_TXN_WRITE_NOSYNC, 1);
            dbenv.log_set_config(DB_LOG_AUTO_REMOVE, 1);
            ret = dbenv.open(strDataDir.c_str(),
                             DB_CREATE     |
                             DB_INIT_LOCK  |
                             DB_INIT_LOG   |
                             DB_INIT_MPOOL |
                             DB_INIT_TXN   |
                             DB_THREAD     |
                             DB_PRIVATE     |
                             DB_RECOVER,
                             S_IRUSR | S_IWUSR);
            if (ret > 0)
                throw runtime_error(strprintf("CDB() : error %d opening database environment", ret));
            fDbEnvInit = true;
        }

        strFile = pszFile;
        ++mapFileUseCount[strFile];
        pdb = mapDb[strFile];
        if (pdb == NULL)
        {
            pdb = new Db(&dbenv, 0);

            ret = pdb->open(NULL,      // Txn pointer
                            pszFile,   // Filename
                            "main",    // Logical db name
                            DB_BTREE,  // Database type
                            nFlags,    // Flags
                            0);

            if (ret > 0)
            {
                delete pdb;
                pdb = NULL;
                CRITICAL_BLOCK(cs_db)
                    --mapFileUseCount[strFile];
                strFile = "";
                throw runtime_error(strprintf("CDB() : can't open database file %s, error %d", pszFile, ret));
            }

            if (fCreate && !Exists(string("version")))
            {
                bool fTmp = fReadOnly;
                fReadOnly = false;
                WriteVersion(VERSION);
                fReadOnly = fTmp;
            }

            mapDb[strFile] = pdb;
        }
    }
}

void
CDB::Close ()
{
  if (!pdb)
    return;
  if (!vTxn.empty () && ownTxn.front ())
    vTxn.front ()->abort ();
  vTxn.clear ();
  ownTxn.clear ();
  pdb = NULL;

  if (!fReadOnly)
    {
      /* Flush database activity from memory pool to disk log.
         wallet.dat is always flushed, the other files only every couple
         of minutes.
         Note: Namecoin has more .dat files than Bitcoin.  */

      unsigned int nMinutes = 2;
      if (strFile == "wallet.dat")
        nMinutes = 0;
      else if ((strFile == "blkindex.dat" || strFile == "game.dat"
                || strFile == "nameindexfull.dat")
               && IsInitialBlockDownload ())
        nMinutes = 5;

      dbenv.txn_checkpoint (nMinutes ? GetArg ("-dblogsize", 100) * 1024 : 0,
                            nMinutes, 0);
    }

  CRITICAL_BLOCK(cs_db)
    --mapFileUseCount[strFile];
}

void static CloseDb(const string& strFile)
{
    CRITICAL_BLOCK(cs_db)
    {
        if (mapDb[strFile] != NULL)
        {
            // Close the database handle
            Db* pdb = mapDb[strFile];
            pdb->close(0);
            delete pdb;
            mapDb[strFile] = NULL;
        }
    }
}

void static CheckpointLSN(const std::string &strFile)
{
    dbenv.txn_checkpoint(0, 0, 0);
    //if (fMockDb)
    //    return;
    dbenv.lsn_reset(strFile.c_str(), 0);
}

bool CDB::Rewrite(const string& strFile)
{
    while (!fShutdown)
    {
        CRITICAL_BLOCK(cs_db)
        {
            if (!mapFileUseCount.count(strFile) || mapFileUseCount[strFile] == 0)
            {
                // Flush log data to the dat file
                CloseDb(strFile);
                CheckpointLSN(strFile);
                mapFileUseCount.erase(strFile);

                bool fSuccess = true;
                printf("Rewriting %s...\n", strFile.c_str());
                string strFileRes = strFile + ".rewrite";
                { // surround usage of db with extra {}
                    CDB db(strFile.c_str(), "r");
                    Db* pdbCopy = new Db(&dbenv, 0);

                    int ret = pdbCopy->open(NULL,                 // Txn pointer
                                            strFileRes.c_str(),   // Filename
                                            "main",    // Logical db name
                                            DB_BTREE,  // Database type
                                            DB_CREATE,    // Flags
                                            0);
                    if (ret > 0)
                    {
                        printf("Cannot create database file %s\n", strFileRes.c_str());
                        fSuccess = false;
                    }

                    Dbc* pcursor = db.GetCursor();
                    if (pcursor)
                        while (fSuccess)
                        {
                            CDataStream ssKey(SER_DISK, VERSION);
                            CDataStream ssValue(SER_DISK, VERSION);
                            int ret = db.ReadAtCursor(pcursor, ssKey, ssValue, DB_NEXT);
                            if (ret == DB_NOTFOUND)
                            {
                                pcursor->close();
                                break;
                            }
                            else if (ret != 0)
                            {
                                pcursor->close();
                                fSuccess = false;
                                break;
                            }
                            Dbt datKey(&ssKey[0], ssKey.size());
                            Dbt datValue(&ssValue[0], ssValue.size());
                            int ret2 = pdbCopy->put(NULL, &datKey, &datValue, DB_NOOVERWRITE);
                            if (ret2 > 0)
                                fSuccess = false;
                        }
                    if (fSuccess)
                    {
                        db.Close();
                        CloseDb(strFile);
                        if (pdbCopy->close(0))
                            fSuccess = false;
                        delete pdbCopy;
                    }
                }
                if (fSuccess)
                {
                    Db dbA(&dbenv, 0);
                    if (dbA.remove(strFile.c_str(), NULL, 0))
                        fSuccess = false;
                    Db dbB(&dbenv, 0);
                    if (dbB.rename(strFileRes.c_str(), NULL, strFile.c_str(), 0))
                        fSuccess = false;
                }
                if (!fSuccess)
                    printf("Rewriting of %s FAILED!\n", strFileRes.c_str());
                return fSuccess;
            }
        }
        MilliSleep(100);
    }
    return false;
}

/* Internal struct to accumulate db stats.  */
struct DbstatsPerKeyData
{
  unsigned count;
  size_t totalKeySize, totalValueSize;
  size_t minKeySize, minValueSize;
  size_t maxKeySize, maxValueSize;
};

void
CDB::PrintStorageStats (const std::string& file)
{
  CCriticalBlock lock(cs_db);
  CDB db(file.c_str (), "r");

  /* Iterate over all entries in the database and keep track of how many
     there are per different "key" (first string of the key) and how
     large they are.  */
  typedef std::map<std::string, DbstatsPerKeyData> DataMap;
  DataMap data;

  Dbc* pcursor = db.GetCursor ();
  if (!pcursor)
    {
      printf ("Error creating cursor.\n");
      return;
    }

  while (true)
    {
      CDataStream ssKey(SER_DISK, VERSION);
      CDataStream ssValue(SER_DISK, VERSION);
      const int ret = db.ReadAtCursor (pcursor, ssKey, ssValue, DB_NEXT);
      if (ret == DB_NOTFOUND)
        {
          pcursor->close ();
          break;
        }
      if (ret != 0)
        {
          pcursor->close ();
          printf ("Error reading at cursor.\n");
          return;
        }

      std::string key;
      ssKey >> key;

      /* Note that size reported will be the size *without* the already
         read key string.  Should not matter much, as this information
         is, of course, known anyway.  */
      const size_t sizeKey = ssKey.size ();
      const size_t sizeValue = ssValue.size ();

      DataMap::iterator i = data.find (key);
      if (i == data.end ())
        {
          DbstatsPerKeyData dat;
          dat.count = 1;
          dat.totalKeySize = dat.minKeySize = dat.maxKeySize = sizeKey;
          dat.totalValueSize = dat.minValueSize = dat.maxValueSize = sizeValue;
          data.insert (std::make_pair (key, dat));
        }
      else
        {
          DbstatsPerKeyData& dat = i->second;
          assert (dat.count >= 1);
          ++dat.count;

          dat.totalKeySize += sizeKey;
          dat.totalValueSize += sizeValue;

          dat.minKeySize = std::min (dat.minKeySize, sizeKey);
          dat.minValueSize = std::min (dat.minValueSize, sizeValue);

          dat.maxKeySize = std::max (dat.maxKeySize, sizeKey);
          dat.maxValueSize = std::max (dat.maxValueSize, sizeValue);
        }
    }

  printf ("Database stats for '%s' collected:\n", file.c_str ());
  for (DataMap::const_iterator i = data.begin (); i != data.end (); ++i)
    {
      const DbstatsPerKeyData& dat = i->second;
      printf ("  %s: %u entries\n", i->first.c_str (), dat.count);
      printf ("    keys:   %u total, %u min, %u max\n",
              dat.totalKeySize, dat.minKeySize, dat.maxKeySize);
      printf ("    values: %u total, %u min, %u max\n",
              dat.totalValueSize, dat.minValueSize, dat.maxValueSize);
    }
}

void DBFlush(bool fShutdown)
{
    // Flush log data to the actual data file
    //  on all files that are not in use
    printf("DBFlush(%s)%s\n", fShutdown ? "true" : "false", fDbEnvInit ? "" : " db not started");
    if (!fDbEnvInit)
        return;
    CRITICAL_BLOCK(cs_db)
    {
        map<string, int>::iterator mi = mapFileUseCount.begin();
        while (mi != mapFileUseCount.end())
        {
            string strFile = (*mi).first;
            int nRefCount = (*mi).second;
            printf("%s refcount=%d\n", strFile.c_str(), nRefCount);
            if (nRefCount == 0)
            {
                // Move log data to the dat file
                CloseDb(strFile);
                dbenv.txn_checkpoint(0, 0, 0);
                printf("%s flush\n", strFile.c_str());
                dbenv.lsn_reset(strFile.c_str(), 0);
                mapFileUseCount.erase(mi++);
            }
            else
                mi++;
        }
        if (fShutdown)
        {
            char** listp;
            if (mapFileUseCount.empty())
                dbenv.log_archive(&listp, DB_ARCH_REMOVE);
            try
            {
                dbenv.close(0);
            }
            catch (const DbException& e)
            {
                printf("EnvShutdown exception: %s (%d)\n", e.what(), e.get_errno());
            }
            fDbEnvInit = false;
        }
    }
}






//
// CTxDB
//

bool CTxDB::ReadTxIndex(uint256 hash, CTxIndex& txindex)
{
    assert(!fClient);
    txindex.SetNull();
    return Read(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::UpdateTxIndex(uint256 hash, const CTxIndex& txindex)
{
    assert(!fClient);
    return Write(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::AddTxIndex(const CTransaction& tx, const CDiskTxPos& pos, int nHeight)
{
    assert(!fClient);

    // Add to tx index
    uint256 hash = tx.GetHash();
    CTxIndex txindex(pos, tx.vout.size());
    return Write(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::EraseTxIndex(const CTransaction& tx)
{
    assert(!fClient);
    uint256 hash = tx.GetHash();

    return Erase(make_pair(string("tx"), hash));
}

bool CTxDB::ContainsTx(uint256 hash)
{
    assert(!fClient);
    return Exists(make_pair(string("tx"), hash));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex)
{
    assert(!fClient);
    tx.SetNull();
    if (!ReadTxIndex(hash, txindex))
        return false;
    return (tx.ReadFromDisk(txindex.pos));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex)
{
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::WriteBlockIndex(const CDiskBlockIndex& blockindex)
{
    return Write(make_pair(string("blockindex"), blockindex.GetBlockHash()), blockindex);
}

bool CTxDB::EraseBlockIndex(uint256 hash)
{
    return Erase(make_pair(string("blockindex"), hash));
}

bool CTxDB::ReadHashBestChain(uint256& hashBestChain)
{
    return Read(string("hashBestChain"), hashBestChain);
}

bool CTxDB::WriteHashBestChain(uint256 hashBestChain)
{
    return Write(string("hashBestChain"), hashBestChain);
}

bool CTxDB::ReadBestInvalidWork(CBigNum& bnBestInvalidWork)
{
    return Read(string("bnBestInvalidWork"), bnBestInvalidWork);
}

bool CTxDB::WriteBestInvalidWork(CBigNum bnBestInvalidWork)
{
    return Write(string("bnBestInvalidWork"), bnBestInvalidWork);
}

CBlockIndex static * InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    // Return existing
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

#include <fstream>

bool CTxDB::LoadBlockIndex()
{
    // Get database cursor
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;

    // Load mapBlockIndex
    unsigned int fFlags = DB_SET_RANGE;
    loop
    {
        // Read next record
        CDataStream ssKey;
        if (fFlags == DB_SET_RANGE)
            ssKey << make_pair(string("blockindex"), uint256(0));
        CDataStream ssValue;
        int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
        fFlags = DB_NEXT;
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
            return false;

        // Unserialize
        string strType;
        ssKey >> strType;
        if (strType == "blockindex")
        {
            CDiskBlockIndex diskindex;
            SetStreamVersion (ssValue);
            ssValue >> diskindex;

            // Construct block index object
            CBlockIndex* pindexNew = InsertBlockIndex(diskindex.GetBlockHash());
            pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
            pindexNew->pnext          = InsertBlockIndex(diskindex.hashNext);
            pindexNew->nFile          = diskindex.nFile;
            pindexNew->nBlockPos      = diskindex.nBlockPos;
            pindexNew->nHeight        = diskindex.nHeight;
            pindexNew->nVersion       = diskindex.nVersion;
            pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
            pindexNew->hashGameMerkleRoot = diskindex.hashGameMerkleRoot;
            pindexNew->nTime          = diskindex.nTime;
            pindexNew->nBits          = diskindex.nBits;
            pindexNew->nNonce         = diskindex.nNonce;

            // Watch for genesis block
            if (pindexGenesisBlock == NULL && diskindex.GetBlockHash() == hashGenesisBlock)
                pindexGenesisBlock = pindexNew;
        }
        else
        {
            break;
        }
    }
    pcursor->close();

    // Calculate bnChainWork
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH(const PAIRTYPE(int, CBlockIndex*)& item, vSortedByHeight)
    {
        CBlockIndex* pindex = item.second;
        pindex->bnChainWork = (pindex->pprev ? pindex->pprev->bnChainWork : 0) + pindex->GetBlockWork();
    }

    // Load hashBestChain pointer to end of best chain
    if (!ReadHashBestChain(hashBestChain))
    {
        if (pindexGenesisBlock == NULL)
            return true;
        return error("CTxDB::LoadBlockIndex() : hashBestChain not loaded");
    }
    if (!mapBlockIndex.count(hashBestChain))
        return error("CTxDB::LoadBlockIndex() : hashBestChain not found in the block index");
    pindexBest = mapBlockIndex[hashBestChain];
    nBestHeight = pindexBest->nHeight;
    bnBestChainWork = pindexBest->bnChainWork;
    printf("LoadBlockIndex(): hashBestChain=%s  height=%d\n", hashBestChain.ToString().substr(0,20).c_str(), nBestHeight);

    // Load bnBestInvalidWork, OK if it doesn't exist
    ReadBestInvalidWork(bnBestInvalidWork);

    // Verify blocks in the best chain
    CBlockIndex* pindexFork = NULL;
    for (CBlockIndex* pindex = pindexBest; pindex && pindex->pprev; pindex = pindex->pprev)
    {
        if (pindex->nHeight < nBestHeight-100 && !mapArgs.count("-checkblocks"))
            break;
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("LoadBlockIndex() : block.ReadFromDisk failed");
        if (!block.CheckBlock(pindex->nHeight))
        {
            printf("LoadBlockIndex() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
            pindexFork = pindex->pprev;
        }
    }
    if (pindexFork)
    {
        // Reorg back to the fork
        printf("LoadBlockIndex() : *** moving best chain pointer back to block %d\n", pindexFork->nHeight);
        CBlock block;
        if (!block.ReadFromDisk(pindexFork))
            return error("LoadBlockIndex() : block.ReadFromDisk failed");

        DatabaseSet dbset;
        block.SetBestChain (dbset, pindexFork);
    }

    return true;
}

// Fix a bug in CDiskTxPos where nBlockFile was uninitialized. As of now we can safely set nBlockFile = 1.
bool CTxDB::FixTxIndexBug()
{
    // Get database cursor
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;

    // All updates are saved to map, because the db cursor implementation here is read-only
    std::map<uint256, CTxIndex> mapUpdate;

    unsigned int fFlags = DB_SET_RANGE;
    loop
    {
        // Read next record
        CDataStream ssKey;
        if (fFlags == DB_SET_RANGE)
            ssKey << make_pair(string("tx"), uint256(0));
        CDataStream ssValue;
        int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
        fFlags = DB_NEXT;
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
            return false;

        // Deserialize
        string strType;
        ssKey >> strType;
        if (strType == "tx")
        {
            uint256 hash;
            ssKey >> hash;
            CTxIndex txindex;
            ssValue >> txindex;
            if (txindex.pos.nBlockFile != 1 && txindex.pos.nTxFile == 1)
            {
                // Fix. Will become invalid when blk0001.dat comes to an end and blk0002.dat takes over.
                txindex.pos.nBlockFile = 1;
                mapUpdate[hash] = txindex;
            }
        }
        else
            break;
    }
    pcursor->close();

    BOOST_FOREACH(const PAIRTYPE(uint256, CTxIndex)& item, mapUpdate)
        if (!UpdateTxIndex(item.first, item.second))
            return error("FixTxIndexBug: UpdateTxIndex failed");

    if (!mapUpdate.empty())
        printf("FixTxIndexBug updated %d transactions\n", mapUpdate.size());

    if (!WriteVersion(VERSION))
        return error("FixTxIndexBug: WriteVersion failed");

  return true;
}

/* Rewrite all txindex objects in the DB to update the data format.  */
bool
CTxDB::RewriteTxIndex (int oldVersion)
{
  /* Load everything in memory first.  This avoids conflicts between reading
     from the cursor and writing to the DB.  */
  std::map<uint256, CTxIndex> txindex;

  /* Get database cursor.  */
  Dbc* pcursor = GetCursor ();
  if (!pcursor)
    return error ("RewriteTxIndex: could not get DB cursor");

  /* Load to memory.  This doesn't yet set the correct spent types,
     but sets each spent output to SPENT_UNKNOWN.  */
  printf ("Reading in old tx entries...\n");
  unsigned int fFlags = DB_SET_RANGE;
  loop
    {
      /* Read next record.  */
      CDataStream ssKey(SER_DISK, oldVersion);
      if (fFlags == DB_SET_RANGE)
        ssKey << std::make_pair (std::string ("tx"), uint256 (0));
      CDataStream ssValue(SER_DISK, oldVersion);
      int ret = ReadAtCursor (pcursor, ssKey, ssValue, fFlags);
      fFlags = DB_NEXT;
      if (ret == DB_NOTFOUND)
        break;
      if (ret != 0)
        return error ("RewriteTxIndex: ReadAtCursor failed, ret = %d", ret);

      /* Unserialize.  */

      std::string strType;
      ssKey >> strType;
      if (strType != "tx")
        break;
      uint256 hash;
      ssKey >> hash;

      CTxIndex obj;
      ssValue >> obj;

      /* Store in map.  */
      assert (txindex.find (hash) == txindex.end ());
      txindex.insert (std::make_pair (hash, obj));
    }
  pcursor->close ();

  /* Go through all possible spending transactions and set their inputs'
     spent type accordingly.  */
  if (oldVersion < 1001000)
    {
      printf ("Fixing tx spent types...\n");
      CBlockIndex* pindex = pindexGenesisBlock;
      for (const CBlockIndex* pindex = pindexGenesisBlock;
           pindex; pindex = pindex->pnext)
        {
          if (pindex->nHeight % 1000 == 0)
            printf ("  at height %d...\n", pindex->nHeight);

          CBlock block;
          block.ReadFromDisk (pindex);

          std::vector<const CTransaction*> vtx;
          vtx.reserve (block.vtx.size () + block.vgametx.size ());
          BOOST_FOREACH(const CTransaction& tx, block.vtx)
            {
              vtx.push_back (&tx);
            }
          BOOST_FOREACH(const CTransaction& tx, block.vgametx)
            {
              vtx.push_back (&tx);
            }

          BOOST_FOREACH(const CTransaction* tx, vtx)
            {
              for (unsigned i = 0; i < tx->vin.size (); ++i)
                if (!tx->vin[i].prevout.IsNull ())
                  {
                    const uint256& prevHash = tx->vin[i].prevout.hash;

                    if (txindex.find (prevHash) == txindex.end ())
                      return error ("RewriteTxIndex: Failed to find prev tx");
                    txindex[prevHash].SetSpent (tx->vin[i].prevout.n, *tx);
                  }
            }
        }

      BOOST_FOREACH(const PAIRTYPE(uint256, CTxIndex)& item, txindex)
        {
          for (unsigned i = 0; i < item.second.GetOutputCount (); ++i)
            if (item.second.GetSpent (i) == CTxIndex::SPENT_UNKNOWN)
              return error ("RewriteTxIndex: Still unknown spent type");
        }
    }

  /* Now write everything back.  */
  printf ("Writing everything back...\n");
  unsigned count = 0;
  const unsigned totalCount = txindex.size ();
  SetSerialisationVersion (VERSION);
  BOOST_FOREACH(const PAIRTYPE(uint256, CTxIndex)& item, txindex)
    {
      if (!UpdateTxIndex (item.first, item.second))
        return error ("RewriteTxIndex: UpdateTxIndex failed");

      ++count;
      if (count % 100000 == 0)
        printf ("  %dk / %dk tx done...\n", count / 1000, totalCount / 1000);
    }

  return true;
}





//
// CAddrDB
//

bool CAddrDB::WriteAddress(const CAddress& addr)
{
    return Write(make_pair(string("addr"), addr.GetKey()), addr);
}

bool CAddrDB::EraseAddress(const CAddress& addr)
{
    return Erase(make_pair(string("addr"), addr.GetKey()));
}

bool CAddrDB::LoadAddresses()
{
    CRITICAL_BLOCK(cs_mapAddresses)
    {
        // Load user provided addresses
        CAutoFile filein = fopen((GetDataDir() + "/addr.txt").c_str(), "rt");
        if (filein)
        {
            try
            {
                char psz[1000];
                while (fgets(psz, sizeof(psz), filein))
                {
                    CAddress addr(psz, NODE_NETWORK);
                    addr.nTime = 0; // so it won't relay unless successfully connected
                    if (addr.IsValid())
                        AddAddress(addr);
                }
            }
            catch (...) { }
        }

        // Get cursor
        Dbc* pcursor = GetCursor();
        if (!pcursor)
            return false;

        loop
        {
            // Read next record
            CDataStream ssKey;
            CDataStream ssValue;
            int ret = ReadAtCursor(pcursor, ssKey, ssValue);
            if (ret == DB_NOTFOUND)
                break;
            else if (ret != 0)
                return false;

            // Unserialize
            string strType;
            ssKey >> strType;
            if (strType == "addr")
            {
                CAddress addr;
                ssValue >> addr;
                mapAddresses.insert(make_pair(addr.GetKey(), addr));
            }
        }
        pcursor->close();

        printf("Loaded %d addresses\n", mapAddresses.size());
    }

    return true;
}

bool LoadAddresses()
{
    return CAddrDB("cr+").LoadAddresses();
}

void PrintSettingsToLog()
{
    printf("fGenerateBitcoins = %d\n", fGenerateBitcoins);
    printf("nTransactionFee = %"PRI64d"\n", nTransactionFee);
    printf("nMinimumInputValue = %"PRI64d"\n", nMinimumInputValue);
    printf("fMinimizeToTray = %d\n", fMinimizeToTray);
    printf("fMinimizeOnClose = %d\n", fMinimizeOnClose);
    printf("fUseProxy = %d\n", fUseProxy);
    printf("addrProxy = %s\n", addrProxy.ToString().c_str());
    if (fHaveUPnP)
        printf("fUseUPnP = %d\n", fUseUPnP);
}

void ThreadFlushWalletDB(void* parg)
{
    const string& strFile = ((const string*)parg)[0];
    static bool fOneThread;
    if (fOneThread)
        return;
    fOneThread = true;
    if (mapArgs.count("-noflushwallet"))
        return;

    unsigned int nLastSeen = nWalletDBUpdated;
    unsigned int nLastFlushed = nWalletDBUpdated;
    int64 nLastWalletUpdate = GetTime();
    while (!fShutdown)
    {
        MilliSleep(500);

        if (nLastSeen != nWalletDBUpdated)
        {
            nLastSeen = nWalletDBUpdated;
            nLastWalletUpdate = GetTime();
        }

        if (nLastFlushed != nWalletDBUpdated && GetTime() - nLastWalletUpdate >= 2)
        {
            TRY_CRITICAL_BLOCK(cs_db)
            {
                // Don't do this if any databases are in use
                int nRefCount = 0;
                map<string, int>::iterator mi = mapFileUseCount.begin();
                while (mi != mapFileUseCount.end())
                {
                    nRefCount += (*mi).second;
                    mi++;
                }

                if (nRefCount == 0 && !fShutdown)
                {
                    map<string, int>::iterator mi = mapFileUseCount.find(strFile);
                    if (mi != mapFileUseCount.end())
                    {
                        printf("%s ", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());
                        printf("Flushing wallet.dat\n");
                        nLastFlushed = nWalletDBUpdated;
                        int64 nStart = GetTimeMillis();

                        // Flush wallet.dat so it's self contained
                        CloseDb(strFile);
                        dbenv.txn_checkpoint(0, 0, 0);
                        dbenv.lsn_reset(strFile.c_str(), 0);

                        mapFileUseCount.erase(mi++);
                        printf("Flushed wallet.dat %"PRI64d"ms\n", GetTimeMillis() - nStart);
                    }
                }
            }
        }
    }
}

bool BackupWallet(const CWallet& wallet, const string& strDest)
{
    if (!wallet.fFileBacked)
        return false;
    while (!fShutdown)
    {
        CRITICAL_BLOCK(cs_db)
        {
            if (!mapFileUseCount.count(wallet.strWalletFile) || mapFileUseCount[wallet.strWalletFile] == 0)
            {
                // Flush log data to the dat file
                CloseDb(wallet.strWalletFile);
                dbenv.txn_checkpoint(0, 0, 0);
                dbenv.lsn_reset(wallet.strWalletFile.c_str(), 0);
                mapFileUseCount.erase(wallet.strWalletFile);

                // Copy wallet.dat
                filesystem::path pathSrc(GetDataDir() + "/" + wallet.strWalletFile);
                filesystem::path pathDest(strDest);
                if (filesystem::is_directory(pathDest))
                    pathDest = pathDest / wallet.strWalletFile;
                    
                try {
#if BOOST_VERSION >= 104000
                    filesystem::copy_file(pathSrc, pathDest, filesystem::copy_option::overwrite_if_exists);
#else
                    filesystem::copy_file(pathSrc, pathDest);
#endif
                    printf("copied wallet.dat to %s\n", pathDest.string().c_str());

                    return true;
                } catch(const filesystem::filesystem_error &e) {
                    printf("error copying wallet.dat to %s - %s\n", pathDest.string().c_str(), e.what());
                    return false;
                }
            }
        }
        MilliSleep(100);
    }
    return false;
}
