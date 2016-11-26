// Copyright (c) 2009-2010 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
#include "headers.h"
#include "db.h"
#include "net.h"
#include "init.h"
#include "auxpow.h"
#include "cryptopp/sha.h"
#include "gamedb.h"
#include "huntercoin.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <cassert>

using namespace std;
using namespace boost;

//
// Global state
//

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

// Game can append transactions to the block file
CCriticalSection cs_AppendBlockFile;

map<uint256, CTransaction> mapTransactions;
CCriticalSection cs_mapTransactions;
unsigned int nTransactionsUpdated = 0;
map<COutPoint, CInPoint> mapNextTx;

map<uint256, CBlockIndex*> mapBlockIndex;
uint256 hashGenesisBlock;
CBigNum bnProofOfWorkLimit[NUM_ALGOS] = { CBigNum(~uint256(0) >> 32), CBigNum(~uint256(0) >> 20) };
CBigNum bnInitialHashTarget[NUM_ALGOS] = { CBigNum(~uint256(0) >> 32), CBigNum(~uint256(0) >> 20) };
const int nInitialBlockThreshold = 0; // Regard blocks up until N-threshold as "initial download"
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;
CBigNum bnBestChainWork = 0;
CBigNum bnBestInvalidWork = 0;
uint256 hashBestChain = 0;
CBlockIndex* pindexBest = NULL;
int64 nTimeBestReceived = 0;
int miningAlgo = ALGO_SHA256D;

CMedianFilter<int> cPeerBlockCounts(8, 0); // Amount of blocks that other nodes claim to have

map<uint256, CBlock*> mapOrphanBlocks;
multimap<uint256, CBlock*> mapOrphanBlocksByPrev;

map<uint256, CDataStream*> mapOrphanTransactions;
multimap<uint256, CDataStream*> mapOrphanTransactionsByPrev;

const std::string strMessageMagic = "Bitcoin Signed Message:\n";
static const unsigned BLOCKFILE_MAX_SIZE = 0x7F000000;
static const unsigned BLOCKFILE_CHUNK_SIZE = 16 * (1 << 20);

double dHashesPerSec;
int64 nHPSTimerStart;

// Settings
int fGenerateBitcoins = false;
int64 nTransactionFee = 0;
int64 nMinimumInputValue = 1;
int fLimitProcessors = false;
int nLimitProcessors = 1;
int fMinimizeToTray = true;
int fMinimizeOnClose = true;
#if USE_UPNP
int fUseUPnP = true;
#else
int fUseUPnP = false;
#endif


CHooks* hooks;



/* Configure the fork heights.  */
bool
ForkInEffect (Fork type, unsigned nHeight)
{
  switch (type)
    {
    case FORK_POISON:
      return nHeight >= (fTestNet ? 190000 : 255000);

    case FORK_CARRYINGCAP:
      return nHeight >= (fTestNet ? 200000 : 500000);

    case FORK_LESSHEARTS:
      return nHeight >= (fTestNet ? 240000 : 590000);

    case FORK_LIFESTEAL:
      return nHeight >= (fTestNet ? 301000 : 795000);

    case FORK_TIMESAVE:
      return nHeight >= (fTestNet ? 331500 : 1521500);

    default:
      assert (false);
    }
}

bool
IsForkHeight (Fork type, unsigned nHeight)
{
  if (nHeight == 0)
    return false;

  return ForkInEffect (type, nHeight) && !ForkInEffect (type, nHeight - 1);
}




//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

void RegisterWallet(CWallet* pwalletIn)
{
    CRITICAL_BLOCK(cs_setpwalletRegistered)
    {
        setpwalletRegistered.insert(pwalletIn);
    }
}

void UnregisterWallet(CWallet* pwalletIn)
{
    CRITICAL_BLOCK(cs_setpwalletRegistered)
    {
        setpwalletRegistered.erase(pwalletIn);
    }
}

bool static IsFromMe(CTransaction& tx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->IsFromMe(tx))
            return true;
    return false;
}

bool static GetTransaction(const uint256& hashTx, CWalletTx& wtx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->GetTransaction(hashTx,wtx))
            return true;
    return false;
}

void static EraseFromWallets(uint256 hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->EraseFromWallet(hash);
}

void SyncWithWallets(const CTransaction& tx, const CBlock* pblock, bool fUpdate)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->AddToWalletIfInvolvingMe(tx, pblock, fUpdate);
}

void static SetBestChain(const CBlockLocator& loc)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->SetBestChain(loc);
}

void static UpdatedTransaction(const uint256& hashTx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->UpdatedTransaction(hashTx);
}

void static PrintWallets(const CBlock& block)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->PrintWallet(block);
}

void static Inventory(const uint256& hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->Inventory(hash);
}

void static ResendWalletTransactions()
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->ResendWalletTransactions();
}







//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

void static AddOrphanTx(const CDataStream& vMsg)
{
    CTransaction tx;
    CDataStream(vMsg) >> tx;
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return;
    CDataStream* pvMsg = mapOrphanTransactions[hash] = new CDataStream(vMsg);
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev.insert(make_pair(txin.prevout.hash, pvMsg));
}

void static EraseOrphanTx(uint256 hash)
{
    if (!mapOrphanTransactions.count(hash))
        return;
    const CDataStream* pvMsg = mapOrphanTransactions[hash];
    CTransaction tx;
    CDataStream(*pvMsg) >> tx;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        for (multimap<uint256, CDataStream*>::iterator mi = mapOrphanTransactionsByPrev.lower_bound(txin.prevout.hash);
             mi != mapOrphanTransactionsByPrev.upper_bound(txin.prevout.hash);)
        {
            if ((*mi).second == pvMsg)
                mapOrphanTransactionsByPrev.erase(mi++);
            else
                mi++;
        }
    }
    delete pvMsg;
    mapOrphanTransactions.erase(hash);
}



//////////////////////////////////////////////////////////////////////////////
//
// Transaction inputs and outputs
//



CUtxoEntry::CUtxoEntry (const CTransaction& tx, unsigned n, int h)
  : txo(tx.vout[n]), height(h),
    isCoinbase(tx.IsCoinBase ()),
    isGameTx(tx.IsGameTx ())
{}


/* Transaction outputs are standard if their scripts are standard
   or if they are tags with sufficiently many coins locked.  */
bool
CTxOut::IsStandard () const
{
  if (::IsStandard (scriptPubKey))
    return true;

  std::string tag;
  if (scriptPubKey.GetTag (tag))
    {
      if (tag.size () > OPRETURN_MAX_STRLEN)
        return error ("%s: too long tag string", __func__);
      if (nValue < OPRETURN_MIN_LOCKED)
        return error ("%s: not enough locked coins in the tag", __func__);

      return true;
    }

  return false;
}







//////////////////////////////////////////////////////////////////////////////
//
// CTransaction and CTxIndex
//

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet)
{
    SetNull();
    if (!txdb.ReadTxIndex(prevout.hash, txindexRet))
        return false;
    if (!ReadFromDisk(txindexRet.pos))
        return false;
    if (prevout.n >= vout.size())
    {
        SetNull();
        return false;
    }
    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout)
{
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::ReadFromDisk(COutPoint prevout)
{
    CTxDB txdb("r");
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    if (fClient)
    {
        if (hashBlock == 0)
            return 0;
    }
    else
    {
        CBlock blockTmp;
        if (pblock == NULL)
        {
            // Load the block this tx is in
            CTxIndex txindex;
            if (!CTxDB("r").ReadTxIndex(GetHash(), txindex))
                return 0;
            if (!blockTmp.ReadFromDisk(txindex.pos.nBlockFile, txindex.pos.nBlockPos))
                return 0;
            pblock = &blockTmp;
        }

        // Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        // Locate the transaction
        if (!IsGameTx())
        {
            for (nIndex = 0; nIndex < pblock->vtx.size(); nIndex++)
                if (pblock->vtx[nIndex] == *(CTransaction*)this)
                    break;

            if (nIndex == pblock->vtx.size())
                nIndex = -1;
        }
        else
        {
            for (nIndex = 0; nIndex < pblock->vgametx.size(); nIndex++)
                if (pblock->vgametx[nIndex] == *(CTransaction*)this)
                    break;

            if (nIndex == pblock->vgametx.size())
                nIndex = -1;
        }

        if (nIndex == -1)
        {
            vMerkleBranch.clear();
            printf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        // Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex, IsGameTx());
    }

    // Is the tx in a block that's in the main chain
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return pindexBest->nHeight - pindex->nHeight + 1;
}

int64 CTransaction::GetMinFee(unsigned int nBlockSize/* =1*/, bool fAllowFree/* =true*/, bool fForRelay/* =false*/) const
{
    // Base fee is either MIN_TX_FEE or MIN_RELAY_TX_FEE
    int64 nBaseFee = fForRelay ? MIN_RELAY_TX_FEE : MIN_TX_FEE;

    unsigned int nBytes = ::GetSerializeSize(*this, SER_NETWORK);
    unsigned int nNewBlockSize = nBlockSize + nBytes;
    int64 nMinFee = (1 + (int64)nBytes / 1000) * nBaseFee;

    if (fAllowFree)
    {
        if (nBlockSize == 1)
        {
            // Transactions under 1K are free
            if (nBytes < 1000)
                nMinFee = 0;
        }
        else
        {
            // Free transaction area
            if (nNewBlockSize < 9000)
                nMinFee = 0;
        }
    }
    hooks->GetMinFee(nMinFee, nBaseFee, *this, nBlockSize, fAllowFree, fForRelay, nBytes, nNewBlockSize);

    // To limit dust spam, require MIN_TX_FEE/MIN_RELAY_TX_FEE for any output less than 0.01
    BOOST_FOREACH(const CTxOut& txout, vout)
        if (txout.nValue < CENT)
            nMinFee += nBaseFee;

    // Raise the price as the block approaches full
    if (nBlockSize != 1 && nNewBlockSize >= MAX_BLOCK_SIZE_GEN/2)
    {
        if (nNewBlockSize >= MAX_BLOCK_SIZE_GEN)
            return MAX_MONEY;
        nMinFee *= MAX_BLOCK_SIZE_GEN / (MAX_BLOCK_SIZE_GEN - nNewBlockSize);
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}






bool CTransaction::CheckTransaction() const
{
    if (IsGameTx())
        return error("CTransaction::CheckTransaction() : game tx encountered");

    // Basic checks that don't depend on any context
    if (vin.empty() || vout.empty())
        return error("CTransaction::CheckTransaction() : vin or vout empty");

    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK) > MAX_BLOCK_SIZE)
        return error("CTransaction::CheckTransaction() : size limits failed");

    // Check for negative or overflow output values
    int64 nValueOut = 0;
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        if (txout.nValue < 0)
            return error("CTransaction::CheckTransaction() : txout.nValue negative");
        if (txout.nValue > MAX_MONEY)
            return error("CTransaction::CheckTransaction() : txout.nValue too high");
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return error("CTransaction::CheckTransaction() : txout total out of range");
    }

    if (IsCoinBase())
    {
        if (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 230)
            return error("CTransaction::CheckTransaction() : coinbase script size");
    }
    else
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (txin.prevout.IsNull())
                return error("CTransaction::CheckTransaction() : prevout is null");
    }

    return hooks->CheckTransaction(*this);
}

bool
CTransaction::AcceptToMemoryPool (DatabaseSet& dbset, bool fCheckInputs,
                                  bool* pfMissingInputs)
{
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!CheckTransaction())
        return error("AcceptToMemoryPool() : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (IsCoinBase())
        return error("AcceptToMemoryPool() : coinbase as individual tx");
    if (IsGameTx())
        return error("AcceptToMemoryPool() : gametx as individual tx");

    // To help v0.1.5 clients who would see it as a negative number
    if ((int64)nLockTime > INT_MAX)
        return error("AcceptToMemoryPool() : not accepting nLockTime beyond 2038 yet");

    // Safety limits
    unsigned int nSize = ::GetSerializeSize(*this, SER_NETWORK);
    // Checking ECDSA signatures is a CPU bottleneck, so to avoid denial-of-service
    // attacks disallow transactions with more than one SigOp per 34 bytes.
    // 34 bytes because a TxOut is:
    //   20-byte address + 8 byte bitcoin amount + 5 bytes of ops + 1 byte script length
    if (GetSigOpCount() > nSize / 34 || nSize < 100)
        return error("AcceptToMemoryPool() : nonstandard transaction");

    // Rather not work on nonstandard transactions (unless -testnet)
    if (!fTestNet && !IsStandard())
        return error("AcceptToMemoryPool() : nonstandard transaction type");

    // Do we already have it?
    uint256 hash = GetHash();
    CRITICAL_BLOCK(cs_mapTransactions)
        if (mapTransactions.count(hash))
            return false;
    if (fCheckInputs)
        if (dbset.tx ().ContainsTx(hash))
            return false;

    // Check for conflicts with in-memory transactions
    CTransaction* ptxOld = NULL;
    for (int i = 0; i < vin.size(); i++)
    {
        COutPoint outpoint = vin[i].prevout;
        if (mapNextTx.count(outpoint))
        {
            ptxOld = mapNextTx[outpoint].ptx;

            // Disable replacement feature for now
            return error ("AcceptToMemoryPool: can't replace %s by %s",
                          ptxOld->GetHash ().ToString ().c_str (),
                          GetHash ().ToString ().c_str ());

            // Allow replacing with a newer version of the same transaction
            if (i != 0)
                return false;
            if (ptxOld->IsFinal())
                return false;
            if (!IsNewerThan(*ptxOld))
                return false;
            for (int i = 0; i < vin.size(); i++)
            {
                COutPoint outpoint = vin[i].prevout;
                if (!mapNextTx.count(outpoint) || mapNextTx[outpoint].ptx != ptxOld)
                    return false;
            }
            break;
        }
    }

    if (fCheckInputs)
    {
        // Check against previous transactions
        CTestPool poolUnused;
        int64 nFees = 0;
        if (!ConnectInputs (dbset, poolUnused, CDiskTxPos(1,1,1), pindexBest,
                            nFees, false, false))
        {
            if (pfMissingInputs)
                *pfMissingInputs = true;
            return error("AcceptToMemoryPool() : ConnectInputs failed %s", hash.ToString().substr(0,10).c_str());
        }

        // Don't accept it if it can't get into a block
        if (nFees < GetMinFee(1000, true, true))
            return error("AcceptToMemoryPool() : not enough fees");
        if (!hooks->CheckFees (*this, nFees))
            return error("AcceptToMemoryPool() : not enough fees for hook");

        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make other's transactions take longer to confirm.
        if (nFees < MIN_RELAY_TX_FEE)
        {
            static CCriticalSection cs;
            static double dFreeCount;
            static int64 nLastTime;
            int64 nNow = GetTime();

            CRITICAL_BLOCK(cs)
            {
                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount > GetArg("-limitfreerelay", 60)*10*1000 && !IsFromMe(*this))
                    return error("AcceptToMemoryPool() : free transaction rejected by rate limiter");
                if (fDebug)
                    printf("Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                dFreeCount += nSize;
            }
        }
    }

    if (!hooks->AcceptToMemoryPool (dbset, *this))
        return error("%s: hook failed", __func__);

    // Store transaction in memory
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        if (ptxOld)
        {
            /* This is disabled for now and should not happen due to
               an earlier error being returned above.  */
            assert (false);

            printf("AcceptToMemoryPool() : replacing tx %s with new version\n", ptxOld->GetHash().ToString().c_str());
            ptxOld->RemoveFromMemoryPool();
        }
        AddToMemoryPoolUnchecked();
    }

    ///// are we sure this is ok when loading transactions or restoring block txes
    // If updated, erase old tx from wallet
    if (ptxOld)
        EraseFromWallets(ptxOld->GetHash());

    if (fDebug)
        printf("AcceptToMemoryPool(): accepted %s\n",
               hash.ToString().substr(0,10).c_str());
    return true;
}

bool CTransaction::AcceptToMemoryPool(bool fCheckInputs, bool* pfMissingInputs)
{
    DatabaseSet dbset("r");
    return AcceptToMemoryPool (dbset, fCheckInputs, pfMissingInputs);
}

bool CTransaction::AddToMemoryPoolUnchecked()
{
    // Add to memory pool without checking anything.  Don't call this directly,
    // call AcceptToMemoryPool to properly check the transaction first.
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        uint256 hash = GetHash();
        mapTransactions[hash] = *this;
        for (int i = 0; i < vin.size(); i++)
            mapNextTx[vin[i].prevout] = CInPoint(&mapTransactions[hash], i);
        nTransactionsUpdated++;
    }
    return true;
}


bool CTransaction::RemoveFromMemoryPool()
{
    hooks->RemoveFromMemoryPool(*this);

    // Remove transaction from memory pool
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
            mapNextTx.erase(txin.prevout);
        mapTransactions.erase(GetHash());
        nTransactionsUpdated++;
    }
    return true;
}

/* Remove transactions from the mempool that are double-spends of the given
   outpoints.  This is used to clear the mempool with unsatisfiable transactions
   once a new block is connected.  */
static void
ClearDoubleSpendings (const std::set<COutPoint>& outs)
{
  /* Keep also track of hashes to prevent duplicate deletion of
     the same transaction.  This could happen otherwise if it spends
     *two* of the outputs at the same time.  */

  std::set<uint256> deleteHash;
  std::vector<CTransaction> vDelete;
  CRITICAL_BLOCK(cs_mapTransactions)
    {
      BOOST_FOREACH (const COutPoint& out, outs)
        {
          std::map<COutPoint, CInPoint>::const_iterator mi;
          mi = mapNextTx.find (out);
          if (mi != mapNextTx.end ())
            {
              const CTransaction& tx = *mi->second.ptx;
              const uint256 hash = tx.GetHash ();
              if (deleteHash.count (hash) == 0)
                {
                  deleteHash.insert (hash);
                  vDelete.push_back (tx);
                }
            }
        }
    }

  assert (deleteHash.size () == vDelete.size ());
  if (!vDelete.empty ())
    {
      printf ("ClearDoubleSpendings: removing %d transactions\n",
              vDelete.size ());
      BOOST_FOREACH (CTransaction& tx, vDelete)
        tx.RemoveFromMemoryPool ();
    }
}






int CMerkleTx::GetDepthInMainChain(int& nHeightRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;

    // Find the block it claims to be in
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != (IsGameTx() ? pindex->hashGameMerkleRoot : pindex->hashMerkleRoot))
            return 0;
        fMerkleVerified = true;
    }

    nHeightRet = pindex->nHeight;
    return pindexBest->nHeight - pindex->nHeight + 1;
}


int CMerkleTx::GetBlocksToMaturity() const
{
    if (IsGameTx())
    {
        return max(0, GAME_REWARD_MATURITY_DISPLAY - GetDepthInMainChain());
    }

    if (!IsCoinBase())
        return 0;
    if (hashBlock == hashGenesisBlock)   // Genesis block is immediately spendable
        return 0;
    return max(0, COINBASE_MATURITY_DISPLAY - GetDepthInMainChain());
}


bool
CMerkleTx::AcceptToMemoryPool (DatabaseSet& dbset, bool fCheckInputs)
{
    if (fClient)
    {
        if (!IsInMainChain() && !ClientConnectInputs())
            return false;
        return CTransaction::AcceptToMemoryPool (dbset, false);
    }
    else
    {
        return CTransaction::AcceptToMemoryPool (dbset, fCheckInputs);
    }
}

bool CMerkleTx::AcceptToMemoryPool()
{
    DatabaseSet dbset("r");
    return AcceptToMemoryPool (dbset);
}



bool
CWalletTx::AcceptWalletTransaction (DatabaseSet& dbset, bool fCheckInputs)
{
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        // Add previous supporting transactions first
        BOOST_FOREACH(CMerkleTx& tx, vtxPrev)
        {
            if (!tx.IsCoinBase())
            {
                uint256 hash = tx.GetHash();
                if (!mapTransactions.count (hash)
                    && !dbset.tx ().ContainsTx (hash))
                  tx.AcceptToMemoryPool (dbset, fCheckInputs);
            }
        }
        return AcceptToMemoryPool (dbset, fCheckInputs);
    }
    return false;
}

bool CWalletTx::AcceptWalletTransaction()
{
    DatabaseSet dbset("r");
    return AcceptWalletTransaction (dbset);
}

const CBlockIndex*
CTxIndex::GetContainingBlock () const
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(pos.nBlockFile, pos.nBlockPos, false))
        return NULL;

    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return NULL;

    return mi->second;
}

int
CTxIndex::GetHeight () const
{
  const CBlockIndex* pindex = GetContainingBlock ();
  if (!pindex)
    return -1;

  return pindex->nHeight;
}

int
CTxIndex::GetDepthInMainChain () const
{
  const CBlockIndex* pindex = GetContainingBlock ();
  if (!pindex || !pindex->IsInMainChain ())
    return 0;

  return 1 + nBestHeight - pindex->nHeight;
}


// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &txOut, uint256 &hashBlock)
{
    CRITICAL_BLOCK(cs_main)
    {
        CRITICAL_BLOCK(cs_mapTransactions)
        {
            std::map<uint256, CTransaction>::iterator mi = mapTransactions.find(hash);
            if (mi != mapTransactions.end())
            {
                txOut = mi->second;
                return true;
            }
        }

        CTxDB txdb("r");
        CTxIndex txindex;

        if (txdb.ReadTxIndex(hash, txindex) && txOut.ReadFromDisk(txindex.pos))
        {
            if (txOut.GetHash() != hash)
                return error("%s() : txid mismatch", __PRETTY_FUNCTION__);

            CBlock block;
            if (block.ReadFromDisk(txindex.pos.nBlockFile, txindex.pos.nBlockPos, false))
                hashBlock = block.GetHash();
            return true;
        }
    }

    return false;
}







//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static CBlockIndex* pblockindexFBBHLast;
CBlockIndex* FindBlockByHeight(int nHeight)
{
    CBlockIndex *pblockindex;
    if (nHeight < nBestHeight / 2)
        pblockindex = pindexGenesisBlock;
    else
        pblockindex = pindexBest;
    if (pblockindexFBBHLast && abs(nHeight - pblockindex->nHeight) > abs(nHeight - pblockindexFBBHLast->nHeight))
        pblockindex = pblockindexFBBHLast;
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;
    while (pblockindex->nHeight < nHeight)
        pblockindex = pblockindex->pnext;
    pblockindexFBBHLast = pblockindex;
    return pblockindex;
}

bool CBlock::ReadFromDisk(unsigned int nFile, unsigned int nBlockPos, bool fReadTransactions /* = true*/)
{
    SetNull();

    // Open history file to read
    CAutoFile filein = OpenBlockFile(nFile, nBlockPos, "rb");
    if (!filein)
        return error("CBlock::ReadFromDisk() : OpenBlockFile failed");
    if (!fReadTransactions)
        filein.nType |= SER_BLOCKHEADERONLY;

    // Read block
    filein >> *this;

    // Check the header
    if (!CheckProofOfWork(INT_MAX))
        return error("CBlock::ReadFromDisk() : errors in block header");

    if (fReadTransactions && nGameTxFile != -1)
    {
        // If same file, do not reopen
        if (nFile == nGameTxFile)
        {
            if (fseek(filein, nGameTxPos, SEEK_SET) != 0)
                return error("CBlock::ReadFromDisk() : fseek failed when trying to read game transactions (nFile=%d, nBlockPos=%d, nGameTxFile=%d, nGameTxPos=%d)", nFile, nBlockPos, nGameTxFile, nGameTxPos);
            filein >> vgametx;
        }
        else
        {
            CAutoFile filein2 = OpenBlockFile(nGameTxFile, nGameTxPos, "rb");
            if (!filein2)
                return error("CBlock::ReadFromDisk() : OpenBlockFile failed when trying to read game transactions (nFile=%d, nBlockPos=%d, nGameTxFile=%d, nGameTxPos=%d)", nFile, nBlockPos, nGameTxFile, nGameTxPos);
            filein2 >> vgametx;
        }
    }
    else
        vgametx.clear();
    return true;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex)
{
    if (!ReadFromDisk(pindex->nFile, pindex->nBlockPos, true))
        return false;
    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");
    return true;
}

void CBlock::SetAuxPow(CAuxPow* pow)
{
    if (pow != NULL)
    {
        nVersion |= BLOCK_VERSION_AUXPOW;
        if (GetAlgo() != pow->algo)
            error("CBlock::SetAuxPow() : mismatching algorithms");
    }
    else
        nVersion &= ~BLOCK_VERSION_AUXPOW;
    auxpow.reset(pow);
}


uint256 static GetOrphanRoot(const CBlock* pblock)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblock->hashPrevBlock))
        pblock = mapOrphanBlocks[pblock->hashPrevBlock];
    return pblock->GetHash();
}

int64 GetBlockValue(int nHeight, int64 nFees)
{
    // Miner: 10% + fee + tax
    // Game : 90%

    int64 nSubsidy = 1 * COIN;

    // Subsidy is cut in half every 2100000 blocks
    // Total amount of coins: 42000000
    // Initially coins per block: 10 (1 to miner, 9 to game treasure)
    // Thus reward halving period is 42000000 / 2 / 10 = 2100000
    nSubsidy >>= (nHeight / 2100000);

    return nSubsidy + nFees;
}

const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, int algo)
{
    while (pindex && pindex->pprev && (pindex->GetAlgo() != algo))
        pindex = pindex->pprev;
    return pindex;
}

const CBlockIndex* GetLastBlockIndexForAlgo(const CBlockIndex* pindex, int algo)
{
    for (;;)
    {
        if (!pindex)
            return NULL;
        if (pindex->GetAlgo() == algo)
            return pindex;
        pindex = pindex->pprev;
    }
}

// PPCoin-style retarget with dual algo support,
// i.e. PoW/PoW instead of PPCoin's PoW/PoS
unsigned int static GetNextWorkRequired(const CBlockIndex* pindexLast, int algo)
{
    if (pindexLast == NULL)
        return bnInitialHashTarget[algo].GetCompact(); // genesis block

    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, algo);
    if (pindexPrev->pprev == NULL)
        return bnInitialHashTarget[algo].GetCompact(); // first block
    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, algo);
    if (pindexPrevPrev->pprev == NULL)
        return bnInitialHashTarget[algo].GetCompact(); // second block

    int64 nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();

    // ppcoin: target change every block
    // ppcoin: retarget with exponential moving toward target spacing
    CBigNum bnNew;
    bnNew.SetCompact(pindexPrev->nBits);

    const int64 nTargetSpacing = 60 * NUM_ALGOS;         // A block every minute for all algos in total
    const int64 nInterval = 2016;
    const int64 nTargetTimespan = nTargetSpacing * nInterval;

    bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nTargetSpacing);

    if (bnNew > bnProofOfWorkLimit[algo])
        bnNew = bnProofOfWorkLimit[algo];

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, int algo)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > bnProofOfWorkLimit[algo])
        return error("CheckProofOfWork(algo=%d) : nBits below minimum work", algo);

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork(algo=%s) : hash doesn't match nBits", algo);

    return true;
}

// Return conservative estimate of total number of blocks, 0 if unknown
int GetTotalBlocksEstimate()
{
    return hooks->LockinHeight();
}

// Return maximum amount of blocks that other nodes claim to have
int GetNumBlocksOfPeers()
{
    return std::max(cPeerBlockCounts.median(), GetTotalBlocksEstimate());
}

bool IsInitialBlockDownload()
{
    if (pindexBest == NULL || nBestHeight < (GetTotalBlocksEstimate()-nInitialBlockThreshold))
        return true;
    static int64 nLastUpdate;
    static CBlockIndex* pindexLastBest;
    if (pindexBest != pindexLastBest)
    {
        pindexLastBest = pindexBest;
        nLastUpdate = GetTime();
    }
    return (GetTime() - nLastUpdate < 60 &&
            pindexBest->GetBlockTime() < GetTime() - 3600);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->bnChainWork > bnBestInvalidWork)
    {
        bnBestInvalidWork = pindexNew->bnChainWork;
        CTxDB().WriteBestInvalidWork(bnBestInvalidWork);
#ifdef GUI
        uiInterface.NotifyBlocksChanged();
#endif
    }
    printf("InvalidChainFound: invalid block=%s  height=%d  work=%s\n", pindexNew->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->nHeight, pindexNew->bnChainWork.ToString().c_str());
    printf("InvalidChainFound:  current best=%s  height=%d  work=%s\n", hashBestChain.ToString().substr(0,20).c_str(), nBestHeight, bnBestChainWork.ToString().c_str());
    if (pindexBest && bnBestInvalidWork > bnBestChainWork + pindexBest->GetBlockWork() * 6)
        printf("InvalidChainFound: WARNING: Displayed transactions may not be correct!  You may need to upgrade, or other nodes may need to upgrade.\n");
}











bool
CTransaction::DisconnectInputs (DatabaseSet& dbset, CBlockIndex* pindex)
{
    if (!hooks->DisconnectInputs (dbset, *this, pindex))
        return false;

    const bool fGameTx = IsGameTx ();

    // Relinquish previous transactions' spent pointers
    if (!IsCoinBase())
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
        {
            COutPoint prevout = txin.prevout;

            // Game transactions can be like coin-base, i.e. produce coins out of nothing
            if (fGameTx && prevout.IsNull())
                continue;

            // Get prev txindex from disk
            CTxIndex txindex;
            if (!dbset.tx ().ReadTxIndex (prevout.hash, txindex))
                return error("DisconnectInputs() : ReadTxIndex failed");

            /* Put the previous txo back to the UTXO set.  This requires loading
               the transaction from disk first.  We could avoid that by keeping
               "not too old" UTXO entries somewhere, but for now this
               should do well enough.  DisconnectInputs shouldn't happen
               too much during normal operation anyway.  */
            CTransaction txPrev;
            if (!txPrev.ReadFromDisk (txindex.pos))
              return error ("DisconnectInputs: %s ReadFromDisk"
                            " prev tx %s failed",
                            GetHash ().ToString ().c_str (),
                            prevout.hash.ToString ().c_str ());
            assert (!txPrev.vout[prevout.n].IsUnspendable ());
            if (!dbset.utxo ().InsertUtxo (txPrev, prevout.n,
                                           txindex.GetHeight ()))
              return error ("DisconnectInputs: Failed to InsertUtxo");
        }
    }

    // Remove transaction from index
    if (!dbset.utxo ().RemoveUtxo (*this))
      return error ("DisconnectInputs: Failed to RemoveUtxo");
    if (!dbset.tx ().EraseTxIndex (*this))
      return error ("DisconnectInputs: EraseTxPos failed");

    return true;
}

bool
CTransaction::ConnectInputs (DatabaseSet& dbset, CTestPool& testPool,
    CDiskTxPos posThisTx, CBlockIndex* pindexBlock, int64& nFees,
    bool fBlock, bool fMiner, int64 nMinFee)
{
    // Take over previous transactions' spent pointers
    if (!IsCoinBase())
    {
        std::vector<CUtxoEntry> vTxoPrev;

        int64 nValueIn = 0;
        for (int i = 0; i < vin.size(); i++)
        {
            const COutPoint prevout = vin[i].prevout;

            /* If we are mining, check that the prevout is not yet
               spent in the testpool.  */
            if (!prevout.IsNull () && fMiner
                && testPool.IsSpent (prevout))
              return error ("ConnectInputs: %s prevout #%d is already"
                            " spent in test pool",
                            GetHashForLog (), prevout.n);

            /* Check if the prev tx comes from memory (otherwise, it is
               expected to be in the UTXO database later on).  If we
               are mining, memory is only allowed if the tx is in the testPool.
               If we are confirming a block, from memory is never allowed.  */
            bool prevTxFromMemory = false;
            if (!prevout.IsNull () && !fBlock
                && mapTransactions.count (prevout.hash))
              {
                if (fMiner)
                  {
                    if (testPool.includedTx.count (prevout.hash))
                      prevTxFromMemory = true;
                  }
                else
                  prevTxFromMemory = true;
              }

            /* If this is a game transaction, we can bypass most of the
               logic below.  But we have to potentially mark the outpoint
               spent (if this is a player death).  */
            if (!IsGameTx ())
            {
                if (prevout.IsNull ())
                    return error ("ConnectInputs() : prevout is null for"
                                  " non-game and non-coinbase transaction");

                /* Read the previous input from either the memory
                   pool or the UTXO set.  */
                CUtxoEntry txo;
                if (prevTxFromMemory)
                {
                    /* Be paranoid about block validation.  */
                    assert (!fBlock);

                    // Get prev tx from single transactions in memory
                    CRITICAL_BLOCK(cs_mapTransactions)
                    {
                        CTransaction txPrev;
                        assert (mapTransactions.count (prevout.hash));
                        txPrev = mapTransactions[prevout.hash];

                        if (prevout.n >= txPrev.vout.size ())
                          return error ("ConnectInputs() : %s prevout.n %d out"
                                        " of range %d prev tx %s\n%s",
                                        GetHashForLog (),
                                        prevout.n, txPrev.vout.size (),
                                        prevout.hash.ToLogString (),
                                        txPrev.ToString ().c_str ());

                        txo = CUtxoEntry(txPrev, prevout.n,
                                         pindexBlock->nHeight);
                    }
                }
                else
                {
                    /* Read the UTXO set.  */
                    if (!dbset.utxo ().ReadUtxo (prevout, txo))
                      return error ("ConnectInputs () : %s failed to find prev"
                                    "out %s in UTXO set",
                                    GetHash ().ToLogString (),
                                    prevout.ToString ().c_str ());
                }

                /* Get height difference between the output and the
                   current block.  This is used to check for maturity
                   later, and it depends on whether or not the prevout
                   is already in some confirmed block.  */
                const int heightDiff = pindexBlock->nHeight - txo.height;
                if (heightDiff < 0)
                  return error ("ConnectInputs: height difference is negative");

                /* If prev is coinbase or a game tx, check that it's matured.
                   The premine coins are an exception.  */
                if (txo.isCoinbase && txo.height > 0)
                  {
                    if (heightDiff < COINBASE_MATURITY)
                      return error ("ConnectInputs: tried to spend coinbase at"
                                    " depth %d", heightDiff);
                  }
                else if (txo.isGameTx)
                  {
                    if (heightDiff < GAME_REWARD_MATURITY)
                      return error ("ConnectInputs: tried to spend game reward"
                                    " at depth %d", heightDiff);
                  }

                // Verify signature
                if (!VerifySignature (txo.txo, *this, i))
                    return error("ConnectInputs() : %s VerifySignature failed", GetHash().ToString().substr(0,10).c_str());

                // Check for negative or overflow input values
                nValueIn += txo.txo.nValue;
                if (!MoneyRange (txo.txo.nValue) || !MoneyRange (nValueIn))
                    return error("ConnectInputs() : txin values out of range");

                /* Fill in vectors with previous outputs.  They are not
                   used for game transactions and can thus stay empty
                   for those.  */
                vTxoPrev.push_back (txo);
            }

            /* Mark previous outpoints as spent.  This is only necessary
               when either fBlock or fMiner.  */
            if (!prevout.IsNull () && (fBlock || fMiner))
            {
                if (fBlock)
                {
                    if (!dbset.utxo ().RemoveUtxo (prevout))
                        return error ("ConnectInputs() : RemoveUtxo failed");
                }
                else
                {
                    assert (fMiner);
                    testPool.SetSpent (prevout);
                }
            }
        }

        if (!hooks->ConnectInputs (dbset, testPool, *this, vTxoPrev,
                                   pindexBlock, posThisTx, fBlock, fMiner))
            return false;

        if (!IsGameTx())
        {
            // Tally transaction fees
            int64 nTxFee = nValueIn - GetValueOut();
            if (nTxFee < 0)
                return error("ConnectInputs() : %s nTxFee < 0", GetHash().ToString().substr(0,10).c_str());
            if (nTxFee < nMinFee)
                return error("ConnectInputs() : %s nTxFee < nMinFee", GetHash().ToString().substr(0,10).c_str());;
            nFees += nTxFee;
            if (!MoneyRange(nFees))
                return error("ConnectInputs() : nFees out of range");
        }
    }

    if (fBlock)
    {
        // Add transaction to disk index
        if (!dbset.utxo ().InsertUtxo (*this, pindexBlock->nHeight))
            return error ("ConnectInputs() : failed to InsertUtxo");
        if (!dbset.tx ().AddTxIndex (*this, posThisTx))
            return error("ConnectInputs() : AddTxPos failed");
    }
    else if (fMiner)
    {
        // Add transaction to test pool
        testPool.AddTx (*this);
    }

    return true;
}


bool CTransaction::ClientConnectInputs()
{
    if (IsCoinBase() || IsGameTx())
        return false;

    // Take over previous transactions' spent pointers
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        int64 nValueIn = 0;
        for (int i = 0; i < vin.size(); i++)
        {
            // Get prev tx from single transactions in memory
            const COutPoint& prevout = vin[i].prevout;
            if (!mapTransactions.count(prevout.hash))
                return false;
            const CTransaction& txPrev = mapTransactions[prevout.hash];

            if (prevout.n >= txPrev.vout.size())
                return false;
            const CTxOut& txoPrev = txPrev.vout[prevout.n];

            // Verify signature
            if (!VerifySignature (txoPrev, *this, i))
                return error("ConnectInputs() : VerifySignature failed");

            ///// this is redundant with the mapNextTx stuff, not sure which I want to get rid of
            ///// this has to go away now that posNext is gone
            // // Check for conflicts
            // if (!txoPrev.posNext.IsNull())
            //     return error("ConnectInputs() : prev tx already used");
            //
            // // Flag outpoints as used
            // txoPrev.posNext = posThisTx;

            nValueIn += txoPrev.nValue;
            if (!MoneyRange (txoPrev.nValue) || !MoneyRange (nValueIn))
                return error("ClientConnectInputs() : txin values out of range");
        }

        if (GetValueOut() > nValueIn)
            return false;
    }

    return true;
}




bool
CBlock::DisconnectBlock (DatabaseSet& dbset, CBlockIndex* pindex)
{
    // Disconnect in reverse order
    for (int i = vgametx.size()-1; i >= 0; i--)
        if (!vgametx[i].DisconnectInputs (dbset, pindex))
            return false;

    // FIXME: if in the same block a player made a move and died, the vgametx.DisconnectInputs call above
    // will delete both entries from the name index (because it scans by block height).
    // The call to vtx.DisconnectInputs below will print a warning to debug.log then.

    for (int i = vtx.size()-1; i >= 0; i--)
        if (!vtx[i].DisconnectInputs (dbset, pindex))
            return false;

    if (!hooks->DisconnectBlock (*this, dbset, pindex))
        return error("DisconnectBlock() : hook failed");

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = 0;
        if (!dbset.tx ().WriteBlockIndex (blockindexPrev))
            return error("DisconnectBlock() : WriteBlockIndex failed");
    }

    return true;
}

bool
CBlock::ConnectBlock (DatabaseSet& dbset, CBlockIndex* pindex)
{
    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(pindex->nHeight))
        return false;

    //// issue here: it doesn't know the version
    unsigned int nTxPos = pindex->nBlockPos + ::GetSerializeSize(*this, SER_DISK|SER_BLOCKHEADERONLY) + GetSizeOfCompactSize(vtx.size());

    CTestPool poolUnused;
    int64 nFees = 0;
    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);
        nTxPos += ::GetSerializeSize(tx, SER_DISK);

        if (!tx.ConnectInputs (dbset, poolUnused, posThisTx, pindex,
                               nFees, true, false))
            return false;
    }

    int64 nFeesBeforeTax = nFees;

    // This call updates the game state and creates vgametx
    if (!hooks->ConnectBlock(*this, dbset, pindex, nFees, nTxPos))
        return error("ConnectBlock() : hook failed");

    // nFees may include taxes from the game, so we check it after creating game transactions
    if (pindex->nHeight && vtx[0].GetValueOut() > GetBlockValue(pindex->nHeight, nFees))
    {
        printf("ConnectBlock() : GetValueOut > GetBlockValue + fees\n");
        printf("  vtx[0].GetValueOut() = %s\n", FormatMoney(vtx[0].GetValueOut()).c_str());
        printf("  GetBlockValue(pindex->nHeight, nFees) = %s\n", FormatMoney(GetBlockValue(pindex->nHeight, nFees)).c_str());
        printf("  nFees = %s\n", FormatMoney(nFees).c_str());
        printf("  nFeesBeforeTax = %s\n", FormatMoney(nFeesBeforeTax).c_str());
        return false;
    }

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();
        if (!dbset.tx ().WriteBlockIndex (blockindexPrev))
            return error("ConnectBlock() : WriteBlockIndex failed");
    }

    // Watch for transactions paying to me
    BOOST_FOREACH(CTransaction& tx, vtx)
        SyncWithWallets(tx, this, true);
    BOOST_FOREACH(CTransaction& tx, vgametx)
        SyncWithWallets(tx, this, true);

    return true;
}

static bool
Reorganize (DatabaseSet& dbset, CBlockIndex* pindexNew)
{
    printf("REORGANIZE\n");

    // Find the fork
    CBlockIndex* pfork = pindexBest;
    CBlockIndex* plonger = pindexNew;
    while (pfork != plonger)
    {
        while (plonger->nHeight > pfork->nHeight)
            if (!(plonger = plonger->pprev))
                return error("Reorganize() : plonger->pprev is null");
        if (pfork == plonger)
            break;
        if (!(pfork = pfork->pprev))
            return error("Reorganize() : pfork->pprev is null");
    }

    // Print some data.
    printf ("Reorganize():\n  Old: %s\n  New: %s\n  Common: %s\n",
            pindexBest->ToString ().c_str (),
            pindexNew->ToString ().c_str (),
            pfork->ToString ().c_str ());

    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;
    for (CBlockIndex* pindex = pindexBest; pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    // List of what to connect
    vector<CBlockIndex*> vConnect;
    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        vConnect.push_back(pindex);
    reverse(vConnect.begin(), vConnect.end());

    vector<CTransaction> vResurrect, vDelete;
    std::set<COutPoint> setSpent;
    CRITICAL_BLOCK(cs_main)    // Lock to prevent concurrent game state reads on the non-main chain
    {
        // Disconnect shorter branch
        BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
        {
            CBlock block;
            if (!block.ReadFromDisk(pindex))
                return error("Reorganize() : ReadFromDisk for disconnect failed");
            if (!block.DisconnectBlock (dbset, pindex))
                return error("Reorganize() : DisconnectBlock failed");

            // Queue memory transactions to resurrect
            BOOST_FOREACH(const CTransaction& tx, block.vtx)
                if (!tx.IsCoinBase())
                    vResurrect.push_back(tx);
        }

        // Connect longer branch
        for (int i = 0; i < vConnect.size(); i++)
        {
            CBlockIndex* pindex = vConnect[i];
            CBlock block;
            if (!block.ReadFromDisk(pindex))
                return error("Reorganize() : ReadFromDisk for connect failed");
            if (!block.ConnectBlock (dbset, pindex))
            {
                // Invalid block
                dbset.TxnAbort ();
                return error("Reorganize() : ConnectBlock failed");
            }

            // Queue memory transactions to delete
            BOOST_FOREACH(const CTransaction& tx, block.vtx)
                vDelete.push_back(tx);
            block.GetSpentOutputs (setSpent);
        }
        if (!dbset.tx ().WriteHashBestChain (pindexNew->GetBlockHash ()))
            return error("Reorganize() : WriteHashBestChain failed");

        // Make sure it's successfully written to disk before changing memory structure
        if (!dbset.TxnCommit ())
            return error("Reorganize() : TxnCommit failed");

        // Disconnect shorter branch
        BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
            if (pindex->pprev)
                pindex->pprev->pnext = NULL;

        // Connect longer branch
        BOOST_FOREACH(CBlockIndex* pindex, vConnect)
            if (pindex->pprev)
                pindex->pprev->pnext = pindex;
    }

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect)
        tx.AcceptToMemoryPool (dbset, false);

    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete)
        tx.RemoveFromMemoryPool();
    ClearDoubleSpendings (setSpent);

    return true;
}


bool
CBlock::SetBestChain (DatabaseSet& dbset, CBlockIndex* pindexNew)
{
    uint256 hash = GetHash();

    dbset.TxnBegin ();
    if (pindexGenesisBlock == NULL && hash == hashGenesisBlock)
    {
        dbset.tx ().WriteHashBestChain (hash);
        if (!dbset.TxnCommit ())
            return error("SetBestChain() : TxnCommit failed");
        pindexGenesisBlock = pindexNew;
    }
    else if (hashPrevBlock == hashBestChain)
    {
        // Adding to current best branch
        if (!ConnectBlock (dbset, pindexNew)
            || !dbset.tx ().WriteHashBestChain (hash))
        {
            dbset.TxnAbort ();
            InvalidChainFound(pindexNew);
            return error("SetBestChain() : ConnectBlock failed");
        }
        if (!dbset.TxnCommit ())
            return error("SetBestChain() : TxnCommit failed");

        // Add to current best branch
        pindexNew->pprev->pnext = pindexNew;

        // Delete redundant memory transactions
        std::set<COutPoint> setSpent;
        GetSpentOutputs (setSpent);
        BOOST_FOREACH(CTransaction& tx, vtx)
            tx.RemoveFromMemoryPool();
        ClearDoubleSpendings (setSpent);
    }
    else
    {
        // New best branch
        if (!Reorganize (dbset, pindexNew))
        {
            dbset.TxnAbort ();
            InvalidChainFound(pindexNew);
            return error("SetBestChain() : Reorganize failed");
        }
    }

    // New best block
    hashBestChain = hash;
    pindexBest = pindexNew;
    nBestHeight = pindexBest->nHeight;
    bnBestChainWork = pindexNew->bnChainWork;
    nTimeBestReceived = GetTime();
    nTransactionsUpdated++;
    printf("SetBestChain: new best=%s  height=%d  work=%s\n", hashBestChain.ToString().substr(0,20).c_str(), nBestHeight, bnBestChainWork.ToString().c_str());

    // Update best block in wallet (so we can detect restored wallets)
    if (!IsInitialBlockDownload())
    {
        const CBlockLocator locator(pindexNew);
        ::SetBestChain(locator);
        EraseBadMoveTransactions ();
    }

    // When everything is done, call hook for new block so that the game
    // state can be finally updated in front-ends and such.
    hooks->NewBlockAdded();

    return true;
}


bool CBlock::AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos)
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AddToBlockIndex() : %s already exists", hash.ToString().substr(0,20).c_str());

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(nFile, nBlockPos, *this);
    if (!pindexNew)
        return error("AddToBlockIndex() : new CBlockIndex failed");
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }
    pindexNew->bnChainWork = (pindexNew->pprev ? pindexNew->pprev->bnChainWork : 0) + pindexNew->GetBlockWork();

    {
      DatabaseSet dbset;
      dbset.TxnBegin ();
      dbset.tx ().WriteBlockIndex (CDiskBlockIndex(pindexNew));
      if (!dbset.TxnCommit ())
        return false;

      // New best
      if (pindexNew->bnChainWork > bnBestChainWork)
        if (!SetBestChain (dbset, pindexNew))
          return false;
    }

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = vtx[0].GetHash();
    }

#ifdef GUI
    uiInterface.NotifyBlocksChanged();
#endif
    return true;
}

int GetAuxPowStartBlock()
{
    // Genesis block has version = 1, other can be merged-mined
    if (fTestNet)
        return 1;
    else
        return 1;
}

int GetOurChainID(int algo)
{
    // Chain IDs for merged mining: SHA-256d, scrypt
    const static int chain_id[NUM_ALGOS] = { 0x0006, 0x0002 };
    return chain_id[algo];
}

bool CBlock::WriteToDisk(unsigned int& nFileRet, unsigned int& nBlockPosRet)
{
    CRITICAL_BLOCK(cs_AppendBlockFile)
    {
        DatabaseSet dbset("r+");

        unsigned nSize;
        unsigned nTotalSize = ::GetSerializeSize (*this, SER_DISK);
        nTotalSize += sizeof (pchMessageStart) + sizeof (nSize);

        // Open history file to append
        CAutoFile fileout = AppendBlockFile (dbset, nFileRet, nTotalSize);
        if (!fileout)
            return error("CBlock::WriteToDisk() : AppendBlockFile failed");
        const unsigned startPos = ftell (fileout);

        // Write index header
        nSize = fileout.GetSerializeSize(*this);
        fileout << FLATDATA(pchMessageStart) << nSize;

        // Write block
        nBlockPosRet = ftell(fileout);
        if (nBlockPosRet == -1)
            return error("CBlock::WriteToDisk() : ftell failed");
        fileout << *this;

        // Check that the total size estimate was correct.
        const unsigned writtenSize = ftell (fileout) - startPos;
        if (writtenSize != nTotalSize)
          return error ("CBlock::WriteToDisk: nTotalSize was wrong: %d / %d",
                        nTotalSize, writtenSize);

        // Flush stdio buffers and commit to disk before returning
        FlushBlockFile(fileout);

        return true;
    }
}

bool CBlock::CheckProofOfWork(int nHeight) const
{
    // Prevent same work from being submitted twice:
    // - this block must have our chain ID
    // - parent block must not have the same chain ID (see CAuxPow::Check)
    // - index of this chain in chain merkle tree must be pre-determined (see CAuxPow::Check)
    int algo = GetAlgo();

    if (nHeight >= GetAuxPowStartBlock())
    {
        if (nHeight != INT_MAX && GetChainID() != GetOurChainID(algo))
            return error("CheckProofOfWork() : block does not have our chain ID");

        if (auxpow.get() != NULL)
        {
            /* Disallow auxpow parent blocks that have an auxpow themselves.
               While this was introduced with a fork, no such tx are present
               in the chain before the fork point.  Thus it can be enforced
               throughout the chain without checking for FORK_CARRYINGCAP.  */
            if (auxpow->parentBlock.nVersion & BLOCK_VERSION_AUXPOW)
              return error ("%s : auxpow parent block has auxpow version",
                            __func__);
            assert (!auxpow->parentBlock.auxpow);

            if (auxpow->algo != algo)
                return error("CheckProofOfWork() : AUX POW uses different algorithm");
            if (!auxpow->Check(GetHash(), GetChainID(), nHeight))
                return error("CheckProofOfWork() : AUX POW is not valid");
            // Check proof of work matches claimed amount
            if (!::CheckProofOfWork(auxpow->GetParentBlockHash(), nBits, algo))
                return error("CheckProofOfWork() : AUX proof of work failed");
        }
        else
        {
            // Check proof of work matches claimed amount
            if (!::CheckProofOfWork(GetPoWHash(algo), nBits, algo))
                return error("CheckProofOfWork() : proof of work failed");
        }
    }
    else
    {
        if (auxpow.get() != NULL)
        {
            return error("CheckProofOfWork() : AUX POW is not allowed at this block");
        }

        // Check if proof of work matches claimed amount
        if (!::CheckProofOfWork(GetPoWHash(algo), nBits, algo))
            return error("CheckProofOfWork() : proof of work failed");
    }
    return true;
}


bool CBlock::CheckBlock(int nHeight) const
{
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    // Size limits
    if (vtx.empty() || vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK) > MAX_BLOCK_SIZE)
        return error("CheckBlock() : size limits failed");

    if (!CheckProofOfWork(nHeight))
        return error("CheckBlock() : proof of work failed");

    // Check timestamp
    if (GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60)
        return error("CheckBlock() : block timestamp too far in the future");

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return error("CheckBlock() : first tx is not coinbase");
    for (int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            return error("CheckBlock() : more than one coinbase");

    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, vtx)
        if (!tx.CheckTransaction())
            return error("CheckBlock() : CheckTransaction failed");

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uniqueTx.insert(tx.GetHash());
    }
    if (uniqueTx.size() != vtx.size())
        return /*DoS(100,*/ error("CheckBlock() : duplicate transaction") /*)*/ ;

    // Check that it's not full of nonstandard transactions
    if (GetSigOpCount() > MAX_BLOCK_SIGOPS)
        return error("CheckBlock() : too many nonstandard transactions");

    // Check merkleroot
    if (hashMerkleRoot != BuildMerkleTree(false))
        return error("CheckBlock() : hashMerkleRoot mismatch");

    return true;
}

bool CBlock::AcceptBlock()
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AcceptBlock() : block already in mapBlockIndex");

    // Get prev block index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashPrevBlock);
    if (mi == mapBlockIndex.end())
        return error("AcceptBlock() : prev block not found");
    CBlockIndex* pindexPrev = (*mi).second;
    int nHeight = pindexPrev->nHeight+1;

    // Check proof of work
    if (nBits != GetNextWorkRequired(pindexPrev, GetAlgo()))
        return error("AcceptBlock() : incorrect proof of work");

    // Check timestamp against prev
    if (GetBlockTime() <= pindexPrev->GetMedianTimePast())
        return error("AcceptBlock() : block's timestamp is too early");

    // limit block in future accepted in chain to only a time window of 30 min
    if (GetBlockTime() > GetAdjustedTime() + 30 * 60)
        return error("CheckBlock() : block timestamp too far in the future");

    // Check timestamp against prev it should not be more then 2 times the window
    if (GetBlockTime() <= pindexPrev->GetBlockTime() - 2 * 30 * 60)
        return error("AcceptBlock() : block's timestamp is too early compare to last block");

    // Check that all transactions are finalized
    BOOST_FOREACH(const CTransaction& tx, vtx)
        if (!tx.IsFinal(nHeight, GetBlockTime()))
            return error("AcceptBlock() : contains a non-final transaction");

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!hooks->Lockin(nHeight, hash))
        return error("AcceptBlock() : rejected by checkpoint lockin at %d", nHeight);

    // Write block to history file
    unsigned int nFile = -1;
    unsigned int nBlockPos = 0;
    if (!WriteToDisk(nFile, nBlockPos))
        return error("AcceptBlock() : WriteToDisk failed");
    if (!AddToBlockIndex(nFile, nBlockPos))
        return error("AcceptBlock() : AddToBlockIndex failed");

    // Relay inventory, but don't relay old inventory during initial block download
    if (hashBestChain == hash)
        CRITICAL_BLOCK(cs_vNodes)
            BOOST_FOREACH(CNode* pnode, vNodes)
                if (nBestHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : hooks->LockinHeight()))
                    pnode->PushInventory(CInv(MSG_BLOCK, hash));

    return true;
}

static void
GetSpentOutputsOfVtx (const std::vector<CTransaction>& vtx,
                      std::set<COutPoint>& outs)
{
  for (std::vector<CTransaction>::const_iterator i = vtx.begin ();
       i != vtx.end (); ++i)
    for (std::vector<CTxIn>::const_iterator j = i->vin.begin ();
         j != i->vin.end (); ++j)
      outs.insert (j->prevout);
}

void
CBlock::GetSpentOutputs (std::set<COutPoint>& outs) const
{
  GetSpentOutputsOfVtx (vtx, outs);
  GetSpentOutputsOfVtx (vgametx, outs);
}

bool ProcessBlock(CNode* pfrom, CBlock* pblock)
{
    // Check for duplicate
    uint256 hash = pblock->GetHash();
    if (mapBlockIndex.count(hash))
        return error("ProcessBlock() : already have block %d %s", mapBlockIndex[hash]->nHeight, hash.ToString().substr(0,20).c_str());
    if (mapOrphanBlocks.count(hash))
        return error("ProcessBlock() : already have block (orphan) %s", hash.ToString().substr(0,20).c_str());

    // Preliminary checks

    // This will be checked again in ConnectBlock with the actual height
    if (!pblock->CheckBlock(INT_MAX))
        return error("ProcessBlock() : CheckBlock FAILED");

    // If don't already have its previous block, shunt it off to holding area until we get it
    if (!mapBlockIndex.count(pblock->hashPrevBlock))
    {
        printf("ProcessBlock: ORPHAN BLOCK, prev=%s\n", pblock->hashPrevBlock.ToString().substr(0,20).c_str());
        CBlock* pblock2 = new CBlock(*pblock);
        mapOrphanBlocks.insert(make_pair(hash, pblock2));
        mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrevBlock, pblock2));

        // Ask this guy to fill in what we're missing
        if (pfrom)
            pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(pblock2));
        return true;
    }

    // Store to disk
    if (!pblock->AcceptBlock())
        return error("ProcessBlock() : AcceptBlock FAILED");

    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);
    for (int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (multimap<uint256, CBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            CBlock* pblockOrphan = (*mi).second;
            if (pblockOrphan->AcceptBlock())
                vWorkQueue.push_back(pblockOrphan->GetHash());
            mapOrphanBlocks.erase(pblockOrphan->GetHash());
            delete pblockOrphan;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    const int algo = pblock->GetAlgo ();
    std::string algoStr;
    switch (algo)
      {
      case ALGO_SHA256D:
        algoStr = "SHA-256";
        break;

      case ALGO_SCRYPT:
        algoStr = "scrypt";
        break;

      default:
        assert (false);
      }
    printf ("ProcessBlock: ACCEPTED @%d %s with %s\n",
            mapBlockIndex[hash]->nHeight,
            hash.ToString ().substr (0, 20).c_str (),
            algoStr.c_str ());

    return true;
}








template<typename Stream>
bool static ScanMessageStart(Stream& s)
{
    // Scan ahead to the next pchMessageStart, which should normally be immediately
    // at the file pointer.  Leaves file pointer at end of pchMessageStart.
    s.clear(0);
    short prevmask = s.exceptions(0);
    const char* p = BEGIN(pchMessageStart);
    try
    {
        loop
        {
            char c;
            s.read(&c, 1);
            if (s.fail())
            {
                s.clear(0);
                s.exceptions(prevmask);
                return false;
            }
            if (*p != c)
                p = BEGIN(pchMessageStart);
            if (*p == c)
            {
                if (++p == END(pchMessageStart))
                {
                    s.clear(0);
                    s.exceptions(prevmask);
                    return true;
                }
            }
        }
    }
    catch (...)
    {
        s.clear(0);
        s.exceptions(prevmask);
        return false;
    }
}

bool
CheckDiskSpace (uint64 nAdditionalBytes)
{
    uint64 nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

    // Check for 15MB because database could create another 10MB log file at any time
    if (nFreeBytesAvailable < (uint64)15000000 + nAdditionalBytes)
    {
        fShutdown = true;
        string strMessage = _("Warning: Disk space is low  ");
        strMiscWarning = strMessage;
        printf("*** %s\n", strMessage.c_str());
#ifdef GUI
        uiInterface.ThreadSafeMessageBox(strMessage, "Huntercoin", wxOK | wxICON_EXCLAMATION);
#else
        ThreadSafeMessageBox(strMessage, "Huntercoin", wxOK | wxICON_EXCLAMATION);
#endif

        CreateThread(Shutdown, NULL);
        return false;
    }
    return true;
}

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode)
{
    if (nFile == -1)
        return NULL;
    FILE* file = fopen(strprintf("%s/blk%04d.dat", GetDataDir().c_str(), nFile).c_str(), pszMode);
    if (!file)
        return NULL;
    if (nBlockPos != 0 && !strchr(pszMode, 'a') && !strchr(pszMode, 'w'))
    {
        if (fseek(file, nBlockPos, SEEK_SET) != 0)
        {
            fclose(file);
            return NULL;
        }
    }
    return file;
}

static unsigned int nCurrentBlockFile = 1;

/* Append to a splitting block history file.  The given size is the number of
   bytes that will be written.  This is used to check for disk space as well
   as extend the file in preallocated chunks to combat fragmentation.  */
FILE*
AppendBlockFile (DatabaseSet& dbset, unsigned int& nFileRet, unsigned size)
{
    CTxDB& txdb = dbset.tx ();

    if (!CheckDiskSpace (size))
      {
        printf ("ERROR: AppendBlockFile: out of disk space");
        return NULL;
      }
    if (fDebug)
      printf ("AppendBlockFile: adding %u bytes\n", size);

    FILE* file = NULL;
    nFileRet = 0;
    loop
    {
        /* We want to open the file in "r+" mode, so that we can write
           not only at the end (so that overwriting reserved bytes is possible).
           Make sure the file exists first.  */
        file = OpenBlockFile (nCurrentBlockFile, 0, "ab");
        fclose (file);
        file = OpenBlockFile (nCurrentBlockFile, 0, "rb+");
        if (!file)
          goto error;

        /* reserved should be an int, since otherwise -reserved below
           fails and the fseek has a wrong effect.  */
        int reserved = txdb.ReadBlockFileReserved (nCurrentBlockFile);
        if (size <= reserved)
          {
            if (fseek (file, -reserved, SEEK_END) != 0)
              goto error;
            reserved -= size;
            if (!txdb.WriteBlockFileReserved (nCurrentBlockFile, reserved))
              goto error;
            break;
          }

        if (fseek (file, 0, SEEK_END) != 0)
          goto error;
        const unsigned fileSize = ftell (file);
        assert (fileSize >= reserved);
        const unsigned addedChunk = std::max (BLOCKFILE_CHUNK_SIZE,
                                              size - reserved);

        if (fileSize + addedChunk <= BLOCKFILE_MAX_SIZE)
          {
            std::vector<char> buf(addedChunk, '\0');
            const unsigned written = fwrite (&buf[0], 1, addedChunk, file);
            if (written != addedChunk)
              {
                printf ("ERROR: AppendBlockFile: write to extend"
                        " by %u bytes failed, only %u written\n",
                        addedChunk, written);
                goto error;
              }
            printf ("Block file extended by %u bytes.\n", written);

            reserved += addedChunk;
            if (fseek (file, -reserved, SEEK_END) != 0)
              goto error;
            reserved -= size;
            if (!txdb.WriteBlockFileReserved (nCurrentBlockFile, reserved))
              goto error;
            break;
          }

        fclose(file);
        nCurrentBlockFile++;
    }

    nFileRet = nCurrentBlockFile;
    return file;

error:
    if (file)
      fclose (file);
    return NULL;
}

void FlushBlockFile(FILE *f)
{
    fflush(f);
    if (!IsInitialBlockDownload() || (nBestHeight + 1) % 500 == 0)
    {
#ifdef __WXMSW__
        _commit(_fileno(f));
#else
        fsync(fileno(f));
#endif
    }
}

bool LoadBlockIndex(bool fAllowNew)
{
    if (fTestNet)
    {
        bnProofOfWorkLimit[ALGO_SHA256D] = CBigNum(~uint256(0) >> 24);
        bnInitialHashTarget[ALGO_SHA256D] = CBigNum(~uint256(0) >> 24);
        bnProofOfWorkLimit[ALGO_SCRYPT] = CBigNum(~uint256(0) >> 12);
        bnInitialHashTarget[ALGO_SCRYPT] = CBigNum(~uint256(0) >> 12);

        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xbf;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xda;
    }

    hooks->MessageStart(pchMessageStart);

    /* Load block index.  Update to the new format (without auxpow)
       if necessary.  */
    {
      int nTxDbVersion = VERSION;
      {
        CTxDB txdb("cr");
        txdb.ReadVersion (nTxDbVersion);
        txdb.SetSerialisationVersion (nTxDbVersion);

        if (!txdb.LoadBlockIndex())
            return false;
        txdb.Close();
      }

      if (nTxDbVersion < 1000400)
        {
          printf ("ERROR: Too old blkindex.dat.\n");
          return false;
        }

      if (nTxDbVersion < 1001300)
        {
          CTxDB wtxdb;
          /* SerialisationVersion is set to VERSION by default.  */

          /* Go through each blkindex object loaded into memory and
             write it again to disk.  */
          printf ("Updating blkindex.dat data format...\n");
          map<uint256, CBlockIndex*>::const_iterator mi;
          for (mi = mapBlockIndex.begin (); mi != mapBlockIndex.end (); ++mi)
            {
              CDiskBlockIndex disk(mi->second);
              wtxdb.WriteBlockIndex (disk);
            }

          /* Rewrite the txindex.  */
          wtxdb.RewriteTxIndex (nTxDbVersion);

          /* Rewrite the database to compact the storage format.  */
          wtxdb.WriteVersion (VERSION);
          wtxdb.Rewrite ();
        }
    }

    //
    // Init with genesis block
    //
    if (mapBlockIndex.empty())
    {
        if (!fAllowNew)
            return false;

        CBlock block;

        if (!hooks->GenesisBlock(block))
            return error("Hook has not created genesis block");

        // Start new block file
        unsigned int nFile;
        unsigned int nBlockPos;
        if (!block.WriteToDisk(nFile, nBlockPos))
            return error("LoadBlockIndex() : writing genesis block to disk failed");
        if (!block.AddToBlockIndex(nFile, nBlockPos))
            return error("LoadBlockIndex() : genesis block not accepted");

        DatabaseSet dbset;
        if (!block.ConnectBlock (dbset, pindexGenesisBlock))
            return error("LoadBlockIndex() : genesis block not accepted");
    }

    return true;
}



void PrintBlockTree()
{
    // precompute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                printf("| ");
            printf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                printf("| ");
            printf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            printf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
        printf("%d (%u,%u) %s  %s  tx %d",
            pindex->nHeight,
            pindex->nFile,
            pindex->nBlockPos,
            block.GetHash().ToString().substr(0,20).c_str(),
            DateTimeStrFormat("%x %H:%M:%S", block.GetBlockTime()).c_str(),
            block.vtx.size());

        PrintWallets(block);

        // put the main timechain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}










//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

map<uint256, CAlert> mapAlerts;
CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;
    if (GetBoolArg("-testsafemode"))
        strRPC = "test";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    // Longer invalid proof-of-work chain
    if (pindexBest && bnBestInvalidWork > bnBestChainWork + pindexBest->GetBlockWork() * 6)
    {
        nPriority = 2000;
        strStatusBar = strRPC = "WARNING: Displayed transactions may not be correct!  You may need to upgrade, or other nodes may need to upgrade.";
    }

    // Alerts
    CRITICAL_BLOCK(cs_mapAlerts)
    {
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(("GetWarnings() : invalid parameter", false));
    return "error";
}

bool CAlert::CheckSignature()
{
    CKey key1, key2;
    if (!key1.SetPubKey(ParseHex(hooks->GetAlertPubkey1())))
        return error("CAlert::CheckSignature() : SetPubKey failed");
    if (!key2.SetPubKey(ParseHex(hooks->GetAlertPubkey2())))
        return error("CAlert::CheckSignature() : SetPubKey failed");
    if (!key1.Verify(Hash(vchMsg.begin(), vchMsg.end()), vchSig) && !key2.Verify(Hash(vchMsg.begin(), vchMsg.end()), vchSig) )
        return error("CAlert::CheckSignature() : verify signature failed");

    // Now unserialize the data
    CDataStream sMsg(vchMsg);
    sMsg >> *(CUnsignedAlert*)this;
    return true;
}

bool CAlert::ProcessAlert()
{
    if (!CheckSignature())
        return false;
    if (!IsInEffect())
        return false;

    CRITICAL_BLOCK(cs_mapAlerts)
    {
        // Cancel previous alerts
        for (map<uint256, CAlert>::iterator mi = mapAlerts.begin(); mi != mapAlerts.end();)
        {
            const CAlert& alert = (*mi).second;
            if (Cancels(alert))
            {
                printf("cancelling alert %d\n", alert.nID);
#ifdef GUI
                uiInterface.NotifyAlertChanged((*mi).first, CT_DELETED);
#endif
                mapAlerts.erase(mi++);
            }
            else if (!alert.IsInEffect())
            {
                printf("expiring alert %d\n", alert.nID);
#ifdef GUI
                uiInterface.NotifyAlertChanged((*mi).first, CT_DELETED);
#endif
                mapAlerts.erase(mi++);
            }
            else
                mi++;
        }

        // Check if this alert has been cancelled
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.Cancels(*this))
            {
                printf("alert already cancelled by %d\n", alert.nID);
                return false;
            }
        }

        // Add to mapAlerts
        mapAlerts.insert(make_pair(GetHash(), *this));
#ifdef GUI
        // Notify UI if it applies to me
        if (AppliesToMe())
            uiInterface.NotifyAlertChanged(GetHash(), CT_NEW);
#endif
    }

    printf("accepted alert %d, AppliesToMe()=%d\n", nID, AppliesToMe());
    return true;
}








//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(CTxDB& txdb, const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_TX:    return mapTransactions.count(inv.hash) || mapOrphanTransactions.count(inv.hash) || txdb.ContainsTx(inv.hash);
    case MSG_BLOCK: return mapBlockIndex.count(inv.hash) || mapOrphanBlocks.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}




// The message start string is designed to be unlikely to occur in normal data.
// The characters are rarely used upper ascii, not valid as UTF-8, and produce
// a large 4-byte int at any alignment.
char pchMessageStart[4] = { 0xf9, 0xbe, 0xb4, 0xd9 };


bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv)
{
    static map<unsigned int, vector<unsigned char> > mapReuseKey;
    RandAddSeedPerfmon();
    if (fDebug)
    {
        printf("%s ", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());
        printf("received: %s (%d bytes)\n", strCommand.c_str(), vRecv.size());
    }

    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        printf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }





    if (strCommand == "version")
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
            return false;

        int64 nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64 nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty())
            vRecv >> pfrom->strSubVer;
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;

        if (pfrom->nVersion == 0)
            return false;

        if (pfrom->nVersion < 1000000)
            return error("The remote node is using old beta version");

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            printf("connected to self at %s, disconnecting\n", pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
            return true;
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        AddTimeData(pfrom->addr.ip, nTime);

        // Change version
        pfrom->PushMessage("verack");
        pfrom->vSend.SetVersion(min(pfrom->nVersion, VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (addrLocalHost.IsRoutable() && !fUseProxy)
            {
                CAddress addr(addrLocalHost);
                addr.nTime = GetAdjustedTime();
                pfrom->PushAddress(addr);
            }

            // Get recent addresses
            if (mapAddresses.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
        }

        // Ask the first connected node for block updates
        static int nAskedForBlocks;
        if (!pfrom->fClient && (nAskedForBlocks < 1 || vNodes.size() <= 1))
        {
            nAskedForBlocks++;
            pfrom->PushGetBlocks(pindexBest, uint256(0));
        }

        // Relay alerts
        CRITICAL_BLOCK(cs_mapAlerts)
            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);

        pfrom->fSuccessfullyConnected = true;

        printf("version message: version %d, blocks=%d, ip=%s\n", pfrom->nVersion, pfrom->nStartingHeight, pfrom->addr.ToString().c_str());

        cPeerBlockCounts.input(pfrom->nStartingHeight);
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->vRecv.SetVersion(min(pfrom->nVersion, VERSION));
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        if (vAddr.size() > 1000)
            return error("message addr size() = %d", vAddr.size());

        // Store the new addresses
        int64 nNow = GetAdjustedTime();
        int64 nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            if (fShutdown)
                return true;
            // ignore IPv6 for now, since it isn't implemented anyway
            if (!addr.IsIPv4())
                continue;
            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            AddAddress(addr, 2 * 60 * 60);
            pfrom->AddAddressKnown(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                CRITICAL_BLOCK(cs_vNodes)
                {
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        RAND_bytes((unsigned char*)&hashSalt, sizeof(hashSalt));
                    uint256 hashRand = hashSalt ^ (((int64)addr.ip)<<32) ^ ((GetTime()+addr.ip)/(24*60*60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = 2;
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
        }
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
    }


    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > 50000)
            return error("message inv size() = %d", vInv.size());

        // find last block in inv vector
        unsigned int nLastBlock = (unsigned int)(-1);
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK) {
                nLastBlock = vInv.size() - 1 - nInv;
                break;
            }
        }
        CTxDB txdb("r");
        for (int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            if (fShutdown)
                return true;
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(txdb, inv);
            if (fDebug)
                printf("  got inventory: %s  %s\n",
                       inv.ToString().c_str(), fAlreadyHave ? "have" : "new");

            if (!fAlreadyHave)
                pfrom->AskFor(inv);
            else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash)) {
                pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(mapOrphanBlocks[inv.hash]));
            } else if (nInv == nLastBlock) {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation and push another getblocks to continue.
                std::vector<CInv> vGetData(1,inv);
                pfrom->PushGetBlocks(mapBlockIndex[inv.hash], uint256(0));
                if (fDebug)
                    printf("force request: %s\n", inv.ToString().c_str());
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > 50000)
            return error("message getdata size() = %d", vInv.size());

        BOOST_FOREACH(const CInv& inv, vInv)
        {
            if (fShutdown)
                return true;
            if (fDebug)
                printf("received getdata for: %s\n", inv.ToString().c_str());

            if (inv.type == MSG_BLOCK)
            {
                // Send block from disk
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    pfrom->PushMessage("block", block);

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, hashBestChain));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                CRITICAL_BLOCK(cs_mapRelay)
                {
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end())
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                }
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = locator.GetBlockIndex();

        // Send the rest of the chain
        if (pindex)
            pindex = pindex->pnext;
        int nLimit = 500 + locator.GetDistanceBack();
        unsigned int nBytes = 0;
        printf("getblocks %d to %s limit %d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString().substr(0,20).c_str(), nLimit);
        for (; pindex; pindex = pindex->pnext)
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                printf("  getblocks stopping at %d %s (%u bytes)\n", pindex->nHeight, pindex->GetBlockHash().ToString().substr(0,20).c_str(), nBytes);
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            CBlock block;
            block.ReadFromDisk(pindex);
            nBytes += block.GetSerializeSize(SER_NETWORK);
            if (--nLimit <= 0 || nBytes >= SendBufferSize()/2)
            {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                printf("  getblocks stopping at limit %d %s (%u bytes)\n", pindex->nHeight, pindex->GetBlockHash().ToString().substr(0,20).c_str(), nBytes);
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = locator.GetBlockIndex();
            if (pindex)
                pindex = pindex->pnext;
        }

        vector<CBlock> vHeaders;
        int nLimit = 2000 + locator.GetDistanceBack();
        printf("getheaders %d to %s limit %d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString().substr(0,20).c_str(), nLimit);
        for (; pindex; pindex = pindex->pnext)
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }

        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx")
    {
        vector<uint256> vWorkQueue;
        CDataStream vMsg(vRecv);
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        // Truncate messages to the size of the tx in them
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK/*, PROTOCOL_VERSION*/);
        unsigned int oldSize = vMsg.size();
        if (nSize < oldSize) {
            vMsg.resize(nSize);
            printf("truncating oversized TX %s (%u -> %u)\n",
                   tx.GetHash().ToString().c_str(),
                   oldSize, nSize);
        }

        bool fMissingInputs = false;
        if (tx.AcceptToMemoryPool(true, &fMissingInputs))
        {
            SyncWithWallets(tx, NULL, true);
            RelayMessage(inv, vMsg);
            mapAlreadyAskedFor.erase(inv);
            vWorkQueue.push_back(inv.hash);

            // Recursively process any orphan transactions that depended on this one
            for (int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (multimap<uint256, CDataStream*>::iterator mi = mapOrphanTransactionsByPrev.lower_bound(hashPrev);
                     mi != mapOrphanTransactionsByPrev.upper_bound(hashPrev);
                     ++mi)
                {
                    const CDataStream& vMsg = *((*mi).second);
                    CTransaction tx;
                    CDataStream(vMsg) >> tx;
                    CInv inv(MSG_TX, tx.GetHash());

                    if (tx.AcceptToMemoryPool(true))
                    {
                        printf("   accepted orphan tx %s\n", inv.hash.ToString().substr(0,10).c_str());
                        SyncWithWallets(tx, NULL, true);
                        RelayMessage(inv, vMsg);
                        mapAlreadyAskedFor.erase(inv);
                        vWorkQueue.push_back(inv.hash);
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vWorkQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            printf("storing orphan tx %s\n", inv.hash.ToString().substr(0,10).c_str());
            AddOrphanTx(vMsg);
        }
    }


    else if (strCommand == "block")
    {
        CBlock block;
        vRecv >> block;

        printf("received block %s\n", block.GetHash().ToString().substr(0,20).c_str());
        // block.print();

        CInv inv(MSG_BLOCK, block.GetHash());
        pfrom->AddInventoryKnown(inv);

        if (ProcessBlock(pfrom, &block))
            mapAlreadyAskedFor.erase(inv);
    }


    else if (strCommand == "getaddr")
    {
        // Nodes rebroadcast an addr every 24 hours
        pfrom->vAddrToSend.clear();
        int64 nSince = GetAdjustedTime() - 3 * 60 * 60; // in the last 3 hours
        CRITICAL_BLOCK(cs_mapAddresses)
        {
            unsigned int nCount = 0;
            BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, CAddress)& item, mapAddresses)
            {
                const CAddress& addr = item.second;
                if (addr.nTime > nSince)
                    nCount++;
            }
            BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, CAddress)& item, mapAddresses)
            {
                const CAddress& addr = item.second;
                if (addr.nTime > nSince && GetRand(nCount) < 2500)
                    pfrom->PushAddress(addr);
            }
        }
    }


    else if (strCommand == "checkorder")
    {
        uint256 hashReply;
        vRecv >> hashReply;

        if (!GetBoolArg("-allowreceivebyip"))
        {
            pfrom->PushMessage("reply", hashReply, (int)2, string(""));
            return true;
        }

        CWalletTx order;
        vRecv >> order;

        /// we have a chance to check the order here

        // Keep giving the same key to the same ip until they use it
        if (!mapReuseKey.count(pfrom->addr.ip))
            mapReuseKey[pfrom->addr.ip] = pwalletMain->GetKeyFromKeyPool();

        // Send back approval of order and pubkey to use
        CScript scriptPubKey;
        scriptPubKey << mapReuseKey[pfrom->addr.ip] << OP_CHECKSIG;
        pfrom->PushMessage("reply", hashReply, (int)0, scriptPubKey);
    }


    else if (strCommand == "reply")
    {
        uint256 hashReply;
        vRecv >> hashReply;

        CRequestTracker tracker;
        CRITICAL_BLOCK(pfrom->cs_mapRequests)
        {
            map<uint256, CRequestTracker>::iterator mi = pfrom->mapRequests.find(hashReply);
            if (mi != pfrom->mapRequests.end())
            {
                tracker = (*mi).second;
                pfrom->mapRequests.erase(mi);
            }
        }
        if (!tracker.IsNull())
            tracker.fn(tracker.param1, vRecv);
    }


    else if (strCommand == "ping")
    {
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        if (alert.ProcessAlert())
        {
            // Relay
            pfrom->setKnown.insert(alert.GetHash());
            CRITICAL_BLOCK(cs_vNodes)
                BOOST_FOREACH(CNode* pnode, vNodes)
                    alert.RelayTo(pnode);
        }
    }


    else
    {
        // Ignore unknown commands for extensibility
    }


    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            AddressCurrentlyConnected(pfrom->addr);


    return true;
}

bool ProcessMessages(CNode* pfrom)
{
    CDataStream& vRecv = pfrom->vRecv;
    if (vRecv.empty())
        return true;
    //if (fDebug)
    //    printf("ProcessMessages(%u bytes)\n", vRecv.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //

    loop
    {
        // Scan for message start
        CDataStream::iterator pstart = search(vRecv.begin(), vRecv.end(), BEGIN(pchMessageStart), END(pchMessageStart));
        int nHeaderSize = vRecv.GetSerializeSize(CMessageHeader());
        if (vRecv.end() - pstart < nHeaderSize)
        {
            if (vRecv.size() > nHeaderSize)
            {
                printf("\n\nPROCESSMESSAGE MESSAGESTART NOT FOUND\n\n");
                vRecv.erase(vRecv.begin(), vRecv.end() - nHeaderSize);
            }
            break;
        }
        if (pstart - vRecv.begin() > 0)
            printf("\n\nPROCESSMESSAGE SKIPPED %d BYTES\n\n", pstart - vRecv.begin());
        vRecv.erase(vRecv.begin(), pstart);

        // Read header
        vector<char> vHeaderSave(vRecv.begin(), vRecv.begin() + nHeaderSize);
        CMessageHeader hdr;
        vRecv >> hdr;
        if (!hdr.IsValid())
        {
            printf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand().c_str());
            continue;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;
        if (nMessageSize > MAX_SIZE)
        {
            printf("ProcessMessage(%s, %u bytes) : nMessageSize > MAX_SIZE\n", strCommand.c_str(), nMessageSize);
            continue;
        }
        if (nMessageSize > vRecv.size())
        {
            // Rewind and wait for rest of message
            vRecv.insert(vRecv.begin(), vHeaderSave.begin(), vHeaderSave.end());
            break;
        }

        // Checksum
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum)
        {
            printf("ProcessMessage(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
                   strCommand.c_str(), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Copy message to its own buffer
        CDataStream vMsg(vRecv.begin(), vRecv.begin() + nMessageSize, vRecv.nType, vRecv.nVersion);
        vRecv.ignore(nMessageSize);

        // Process message
        bool fRet = false;
        try
        {
            CRITICAL_BLOCK(cs_main)
                fRet = ProcessMessage(pfrom, strCommand, vMsg);
            if (fShutdown)
                return true;
        }
        catch (std::ios_base::failure& e)
        {
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from underlength message on vRecv
                printf("ProcessMessage(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from overlong size
                printf("ProcessMessage(%s, %u bytes) : Exception '%s' caught\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessage()");
            }
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessage()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessage()");
        }

        if (!fRet)
            printf("ProcessMessage(%s, %u bytes) FAILED\n", strCommand.c_str(), nMessageSize);
    }

    vRecv.Compact();
    return true;
}


bool SendMessages(CNode* pto, bool fSendTrickle)
{
    CRITICAL_BLOCK(cs_main)
    {
        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        // Keep-alive ping
        if (pto->nLastSend && GetTime() - pto->nLastSend > 30 * 60 && pto->vSend.empty())
            pto->PushMessage("ping");

        // Start block sync
        if (pto->fStartSync) {
            pto->fStartSync = false;
            pto->PushGetBlocks(pindexBest, uint256(0));
        }

        // Resend wallet transactions that haven't gotten in a block yet
        ResendWalletTransactions();

        // Address refresh broadcast
        static int64 nLastRebroadcast;
        if (GetTime() - nLastRebroadcast > 24 * 60 * 60)
        {
            nLastRebroadcast = GetTime();
            CRITICAL_BLOCK(cs_vNodes)
            {
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    // Periodically clear setAddrKnown to allow refresh broadcasts
                    pnode->setAddrKnown.clear();

                    // Rebroadcast our address
                    if (addrLocalHost.IsRoutable() && !fUseProxy)
                    {
                        CAddress addr(addrLocalHost);
                        addr.nTime = GetAdjustedTime();
                        pnode->PushAddress(addr);
                    }
                }
            }
        }

        // Clear out old addresses periodically so it's not too much work at once
        static int64 nLastClear;
        if (nLastClear == 0)
            nLastClear = GetTime();
        if (GetTime() - nLastClear > 10 * 60 && vNodes.size() >= 3)
        {
            nLastClear = GetTime();
            CRITICAL_BLOCK(cs_mapAddresses)
            {
                CAddrDB addrdb;
                int64 nSince = GetAdjustedTime() - 14 * 24 * 60 * 60;
                for (map<vector<unsigned char>, CAddress>::iterator mi = mapAddresses.begin();
                     mi != mapAddresses.end();)
                {
                    const CAddress& addr = (*mi).second;
                    if (addr.nTime < nSince)
                    {
                        if (mapAddresses.size() < 1000 || GetTime() > nLastClear + 20)
                            break;
                        addrdb.EraseAddress(addr);
                        mapAddresses.erase(mi++);
                    }
                    else
                        mi++;
                }
            }
        }


        //
        // Message: addr
        //
        if (fSendTrickle)
        {
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }


        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        CRITICAL_BLOCK(pto->cs_inventory)
        {
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
            {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle)
                {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        RAND_bytes((unsigned char*)&hashSalt, sizeof(hashSalt));
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    // always trickle our own transactions
                    if (!fTrickleWait)
                    {
                        CWalletTx wtx;
                        if (GetTransaction(inv.hash, wtx))
                            if (wtx.fFromMe)
                                fTrickleWait = true;
                    }

                    if (fTrickleWait)
                    {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000)
                    {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);


        //
        // Message: getdata
        //
        vector<CInv> vGetData;
        int64 nNow = GetTime() * 1000000;
        CTxDB txdb("r");
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(txdb, inv))
            {
                if (fDebug)
                    printf("sending getdata: %s\n", inv.ToString().c_str());

                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);

    }
    return true;
}














//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;
    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;
    unsigned int bits = len * 8;
    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;
    return blocks;
}

using CryptoPP::ByteReverse;

static const unsigned int pSHA256InitState[8] =
{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

inline void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    memcpy(pstate, pinit, 32);
    CryptoPP::SHA256::Transform((CryptoPP::word32*)pstate, (CryptoPP::word32*)pinput);
}

//
// ScanHash scans nonces looking for a hash with at least some zero bits.
// It operates on big endian data.  Caller does the byte reversing.
// All input buffers are 16-byte aligned.  nNonce is usually preserved
// between calls, but periodically or if nNonce is 0xffff0000 or above,
// the block is rebuilt and nNonce starts over at zero.
//
unsigned int static ScanHash_CryptoPP(char* pmidstate, char* pdata, char* phash1, char* phash, unsigned int& nHashesDone)
{
    unsigned int& nNonce = *(unsigned int*)(pdata + 12);
    for (;;)
    {
        // Crypto++ SHA-256
        // Hash pdata using pmidstate as the starting state into
        // preformatted buffer phash1, then hash phash1 into phash
        nNonce++;
        SHA256Transform(phash1, pdata, pmidstate);
        SHA256Transform(phash, phash1, pSHA256InitState);

        // Return the nonce if the hash has at least some zero bits,
        // caller will check if it has enough to reach the target
        if (((unsigned short*)phash)[14] == 0)
            return nNonce;

        // If nothing found after trying for a while, return -1
        if ((nNonce & 0xffff) == 0)
        {
            nHashesDone = 0xffff+1;
            return -1;
        }
    }
}


class COrphan
{
public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;

    COrphan(CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = 0;
    }

    void print() const
    {
        printf("COrphan(hash=%s, dPriority=%.1f)\n", ptx->GetHash().ToString().substr(0,10).c_str(), dPriority);
        BOOST_FOREACH(uint256 hash, setDependsOn)
            printf("   setDependsOn %s\n", hash.ToString().substr(0,10).c_str());
    }
};

void CBlock::SetNull()
{
    nVersion = BLOCK_VERSION_DEFAULT | (GetOurChainID(ALGO_SHA256D) * BLOCK_VERSION_CHAIN_START);
    hashPrevBlock = 0;
    hashMerkleRoot = 0;
    hashGameMerkleRoot = 0;
    nTime = 0;
    nBits = 0;
    nNonce = 0;
    vtx.clear();
    vgametx.clear();
    vMerkleTree.clear();
    vGameMerkleTree.clear();
    auxpow.reset();

    nGameTxFile = nGameTxPos = -1;
}

CBlock* CreateNewBlock(CReserveKey& reservekey, int algo)
{
    // Create new block
    auto_ptr<CBlock> pblock(new CBlock());
    if (!pblock.get())
        return NULL;

    pblock->nVersion = BLOCK_VERSION_DEFAULT | (GetOurChainID(algo) * BLOCK_VERSION_CHAIN_START);
    switch (algo)
    {
        case ALGO_SHA256D:
            break;
        case ALGO_SCRYPT:
            pblock->nVersion |= BLOCK_VERSION_SCRYPT;
            break;
        default:
            error("CreateNewBlock: bad algo");
            return NULL;
    }

    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    txNew.vout[0].scriptPubKey.SetBitcoinAddress(reservekey.GetReservedKey());

    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);

    CBlockIndex* pindexPrev;

    // Collect memory pool transactions into the block
    int64 nFees = 0;

    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        DatabaseSet dbset("r");

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;
        multimap<double, CTransaction*> mapPriority;
        for (map<uint256, CTransaction>::iterator mi = mapTransactions.begin(); mi != mapTransactions.end(); ++mi)
        {
            CTransaction& tx = (*mi).second;
            if (tx.IsCoinBase() || !tx.IsFinal())
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                /* Read prev output from UTXO set.  */
                CUtxoEntry txo;
                if (!dbset.utxo ().ReadUtxo (txin.prevout, txo))
                {
                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);

                    assert (!dbset.utxo ().ReadUtxo (txin.prevout, txo));
                    continue;
                }

                /* Calculate priority.  */
                const int64 nValueIn = txo.txo.nValue;
                const int nConf = 1 + nBestHeight - txo.height;
                assert (nConf > 0);
                dPriority += static_cast<double> (nValueIn) * nConf;

                if (fDebug && GetBoolArg("-printpriority"))
                    printf("priority     nValueIn=%.8f nConf=%d dPriority=%.4f\n", static_cast<double> (nValueIn) / COIN, nConf, dPriority);
            }

            // Priority is sum(valuein * age) / txsize
            dPriority /= ::GetSerializeSize(tx, SER_NETWORK);

            if (porphan)
                porphan->dPriority = dPriority;
            else
                mapPriority.insert(make_pair(-dPriority, &(*mi).second));

            if (fDebug && GetBoolArg("-printpriority"))
            {
                printf("priority %.4f %s\n%s", dPriority, tx.GetHash().ToString().substr(0,10).c_str(), tx.ToString().c_str());
                if (porphan)
                    porphan->print();
                printf("\n");
            }
        }

        // If we do not exclude invalid game transactions, the block won't be accepted by ConnectBlock
        // Also we need to compute tax
        pindexPrev = pindexBest;
        GameStepMiner gameStepMiner(dbset, pindexPrev);

        // Collect transactions into block
        CTestPool testPool;
        uint64 nBlockSize = 1000;
        int nBlockSigOps = 100;
        while (!mapPriority.empty())
        {
            // Take highest priority transaction off priority queue
            double dPriority = -(*mapPriority.begin()).first;
            CTransaction& tx = *(*mapPriority.begin()).second;
            mapPriority.erase(mapPriority.begin());

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK);
            if (nBlockSize + nTxSize >= MAX_BLOCK_SIZE_GEN)
                continue;
            int nTxSigOps = tx.GetSigOpCount();
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Transaction fee required depends on block size
            bool fAllowFree = (nBlockSize + nTxSize < 4000 || CTransaction::AllowFree(dPriority));
            int64 nMinFee = tx.GetMinFee(nBlockSize, fAllowFree, true);

            // Connecting shouldn't fail due to dependency on other memory pool transactions
            // because we're already processing them in order of dependency
            CTestPool tmpPool(testPool);
            if (!tx.ConnectInputs (dbset, tmpPool, CDiskTxPos(1,1,1),
                                   pindexPrev, nFees, false, true, nMinFee))
                continue;
            if (!gameStepMiner.AddTx(tx))
                continue;
            testPool.swap (tmpPool);

            // Added
            pblock->vtx.push_back(tx);
            nBlockSize += nTxSize;
            nBlockSigOps += nTxSigOps;

            // Add transactions that depend on this one to the priority queue
            uint256 hash = tx.GetHash();
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                            mapPriority.insert(make_pair(-porphan->dPriority, porphan->ptx));
                    }
                }
            }
        }

        int64 nTax = gameStepMiner.ComputeTax();
        nFees += nTax;
    }
    pblock->vtx[0].vout[0].nValue = GetBlockValue(pindexPrev->nHeight+1, nFees);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    pblock->hashMerkleRoot = pblock->BuildMerkleTree(false);
    pblock->nTime          = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());
    pblock->nBits          = GetNextWorkRequired(pindexPrev, algo);
    pblock->nNonce         = 0;

    return pblock.release();
}


void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce, int64& nPrevTime)
{
    // Update nExtraNonce
    int64 nNow = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());
    if (++nExtraNonce >= 0x7f && nNow > nPrevTime+1)
    {
        nExtraNonce = 1;
        nPrevTime = nNow;
    }
    const unsigned nHeight = pindexPrev->nHeight + 1;
    pblock->vtx[0].vin[0].scriptSig = CScript() << nHeight << CBigNum(nExtraNonce);
    pblock->hashMerkleRoot = pblock->BuildMerkleTree(false);
}

// Create coinbase with auxiliary data, for multichain mining
void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    //
    // Prebuild hash buffers
    //
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;
        unsigned char pchPadding0[64];
        uint256 hash1;
        unsigned char pchPadding1[64];
    }
    tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (int i = 0; i < sizeof(tmp)/4; i++)
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}


bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    int algo = pblock->GetAlgo();
    uint256 hashBlock = pblock->GetHash();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    CAuxPow *auxpow = pblock->auxpow.get();

    if (auxpow != NULL)
    {
        if (auxpow->parentBlock.nVersion & BLOCK_VERSION_AUXPOW)
          return error ("%s : auxpow parent block has auxpow version",
                        __func__);

        if (auxpow->algo != algo)
            return error("CheckWork() : AUX POW uses different algorithm");

        /* FIXME: Remove this hack for nHeight when the fork has passed
           and we no longer need it anyway.  */
        if (!auxpow->Check(hashBlock, pblock->GetChainID(), nBestHeight + 1))
            return error("AUX POW is not valid");

        uint256 hashParent = auxpow->GetParentBlockHash();
        if (hashParent > hashTarget)
            return error("AUX POW parent hash %s is not under target %s", auxpow->GetParentBlockHash().GetHex().c_str(), hashTarget.GetHex().c_str());

        //// debug print
        printf("BitcoinMiner:\n");
        printf("AUX proof-of-work found  \n  block-hash: %s  \n  parent hash: %s  \ntarget: %s\n",
                hashBlock.GetHex().c_str(),
                hashParent.GetHex().c_str(),
                hashTarget.GetHex().c_str());
    }
    else
    {
        uint256 hashPoW = pblock->GetPoWHash(algo);

        if (hashPoW > hashTarget)
            return false;

        //// debug print
        printf("BitcoinMiner:\n");
        printf("proof-of-work found  \n  block-hash: %s  \n  pow-hash: %s  \ntarget: %s\n",
                hashBlock.GetHex().c_str(),
                hashPoW.GetHex().c_str(),
                hashTarget.GetHex().c_str());
    }

    pblock->print();
    printf("%s ", DateTimeStrFormat("%x %H:%M", GetTime()).c_str());
    printf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue).c_str());

    // Found a solution
    CRITICAL_BLOCK(cs_main)
    {
        if (pblock->hashPrevBlock != hashBestChain)
            return error("BitcoinMiner : generated block is stale");

        // Remove key from key pool
        reservekey.KeepKey();

        // Track how many getdata requests this block gets
        CRITICAL_BLOCK(wallet.cs_mapRequestCount)
            wallet.mapRequestCount[pblock->GetHash()] = 0;

        // Process this block the same as if we had received it from another node
        if (!ProcessBlock(NULL, pblock))
            return error("BitcoinMiner : ProcessBlock, block not accepted");
    }

    MilliSleep(2000);
    return true;
}

void static ThreadBitcoinMiner(void* parg);

void static BitcoinMiner(CWallet *pwallet)
{
    printf("BitcoinMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;
    int64 nPrevTime = 0;

    while (fGenerateBitcoins)
    {
        if (AffinityBugWorkaround(ThreadBitcoinMiner, pwallet))
            return;
        if (fShutdown)
            return;

        while (vNodes.empty() || IsInitialBlockDownload())
        {
            MilliSleep(1000);
            if (fShutdown)
                return;
            if (!fGenerateBitcoins)
                return;
        }

        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = nTransactionsUpdated;
        CBlockIndex* pindexPrev = pindexBest;

        auto_ptr<CBlock> pblock(CreateNewBlock(reservekey, ALGO_SHA256D));
        if (!pblock.get())
            return;
        IncrementExtraNonce(pblock.get(), pindexPrev, nExtraNonce, nPrevTime);

        printf("Running BitcoinMiner with %d transactions in block\n", pblock->vtx.size());


        //
        // Prebuild hash buffers
        //
        char pmidstatebuf[32+16]; char* pmidstate = alignup<16>(pmidstatebuf);
        char pdatabuf[128+16];    char* pdata     = alignup<16>(pdatabuf);
        char phash1buf[64+16];    char* phash1    = alignup<16>(phash1buf);

        FormatHashBuffers(pblock.get(), pmidstate, pdata, phash1);

        unsigned int& nBlockTime = *(unsigned int*)(pdata + 64 + 4);
        unsigned int& nBlockNonce = *(unsigned int*)(pdata + 64 + 12);


        //
        // Search
        //
        int64 nStart = GetTime();
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
        uint256 hashbuf[2];
        uint256& hash = *alignup<16>(hashbuf);
        loop
        {
            unsigned int nHashesDone = 0;
            unsigned int nNonceFound;

            // Crypto++ SHA-256
            nNonceFound = ScanHash_CryptoPP(pmidstate, pdata + 64, phash1,
                                            (char*)&hash, nHashesDone);

            // Check if something found
            if (nNonceFound != -1)
            {
                for (int i = 0; i < sizeof(hash)/4; i++)
                    ((unsigned int*)&hash)[i] = ByteReverse(((unsigned int*)&hash)[i]);

                if (hash <= hashTarget)
                {
                    // Found a solution
                    pblock->nNonce = ByteReverse(nNonceFound);
                    assert(hash == pblock->GetHash());

                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    CheckWork(pblock.get(), *pwalletMain, reservekey);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    break;
                }
            }

            // Meter hashes/sec
            static int64 nHashCounter;
            if (nHPSTimerStart == 0)
            {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            }
            else
                nHashCounter += nHashesDone;
            if (GetTimeMillis() - nHPSTimerStart > 4000)
            {
                static CCriticalSection cs;
                CRITICAL_BLOCK(cs)
                {
                    if (GetTimeMillis() - nHPSTimerStart > 4000)
                    {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        string strStatus = strprintf("    %.0f khash/s", dHashesPerSec/1000.0);
                        UIThreadCall(boost::bind(CalledSetStatusBar, strStatus, 0));
                        static int64 nLogTime;
                        if (GetTime() - nLogTime > 30 * 60)
                        {
                            nLogTime = GetTime();
                            printf("%s ", DateTimeStrFormat("%x %H:%M", GetTime()).c_str());
                            printf("hashmeter %3d CPUs %6.0f khash/s\n", vnThreadsRunning[3], dHashesPerSec/1000.0);
                        }
                    }
                }
            }

            // Check for stop or if block needs to be rebuilt
            if (fShutdown)
                return;
            if (!fGenerateBitcoins)
                return;
            if (fLimitProcessors && vnThreadsRunning[3] > nLimitProcessors)
                return;
            if (vNodes.empty())
                break;
            if (nBlockNonce >= 0xffff0000)
                break;
            if (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                break;
            if (pindexPrev != pindexBest)
                break;

            // Update nTime every few seconds
            pblock->nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());
            nBlockTime = ByteReverse(pblock->nTime);
        }
    }
}

void static ScryptMiner(CWallet *pwallet)
{
    printf("ScryptMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;
    int64 nPrevTime = 0;

    while (fGenerateBitcoins)
    {
        if (AffinityBugWorkaround(ThreadBitcoinMiner, pwallet))
            return;
        if (fShutdown)
            return;

        while (vNodes.empty() || IsInitialBlockDownload())
        {
            MilliSleep(1000);
            if (fShutdown)
                return;
            if (!fGenerateBitcoins)
                return;
        }

        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = nTransactionsUpdated;
        CBlockIndex* pindexPrev = pindexBest;

        auto_ptr<CBlock> pblock(CreateNewBlock(reservekey, ALGO_SCRYPT));
        if (!pblock.get())
            return;
        IncrementExtraNonce(pblock.get(), pindexPrev, nExtraNonce, nPrevTime);

        printf("Running ScryptMiner with %d transactions in block\n", pblock->vtx.size());


        //
        // Prebuild hash buffers
        //
        char pmidstatebuf[32+16]; char* pmidstate = alignup<16>(pmidstatebuf);
        char pdatabuf[128+16];    char* pdata     = alignup<16>(pdatabuf);
        char phash1buf[64+16];    char* phash1    = alignup<16>(phash1buf);

        FormatHashBuffers(pblock.get(), pmidstate, pdata, phash1);

        unsigned int& nBlockTime = *(unsigned int*)(pdata + 64 + 4);
        unsigned int& nBlockBits = *(unsigned int*)(pdata + 64 + 8);

        //
        // Search
        //
        int64 nStart = GetTime();
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
        loop
        {
            unsigned int nHashesDone = 0;
            uint256 thash;
            char scratchpad[SCRYPT_SCRATCHPAD_SIZE];
            loop
            {
#if defined(USE_SSE2)
                // Detection would work, but in cases where we KNOW it always has SSE2,
                // it is faster to use directly than to use a function pointer or conditional.
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_AMD64) || (defined(MAC_OSX) && defined(__i386__))
                // Always SSE2: x86_64 or Intel MacOS X
                scrypt_1024_1_1_256_sp_sse2(BEGIN(pblock->nVersion), BEGIN(thash), scratchpad);
#else
                // Detect SSE2: 32bit x86 Linux or Windows
                scrypt_1024_1_1_256_sp(BEGIN(pblock->nVersion), BEGIN(thash), scratchpad);
#endif
#else
                // Generic scrypt
                scrypt_1024_1_1_256_sp_generic(BEGIN(pblock->nVersion), BEGIN(thash), scratchpad);
#endif

                if (thash <= hashTarget)
                {
                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    CheckWork(pblock.get(), *pwallet, reservekey);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    break;
                }
                pblock->nNonce += 1;
                nHashesDone += 1;
                if ((pblock->nNonce & 0xFF) == 0)
                    break;
            }

            // Meter hashes/sec
            static int64 nHashCounter;
            if (nHPSTimerStart == 0)
            {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            }
            else
                nHashCounter += nHashesDone;
            if (GetTimeMillis() - nHPSTimerStart > 4000)
            {
                static CCriticalSection cs;
                CRITICAL_BLOCK(cs)
                {
                    if (GetTimeMillis() - nHPSTimerStart > 4000)
                    {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        string strStatus = strprintf("    %.0f khash/s", dHashesPerSec/1000.0);
                        UIThreadCall(boost::bind(CalledSetStatusBar, strStatus, 0));
                        static int64 nLogTime;
                        if (GetTime() - nLogTime > 30 * 60)
                        {
                            nLogTime = GetTime();
                            printf("%s ", DateTimeStrFormat("%x %H:%M", GetTime()).c_str());
                            printf("hashmeter %3d CPUs %6.0f khash/s\n", vnThreadsRunning[3], dHashesPerSec/1000.0);
                        }
                    }
                }
            }

            // Check for stop or if block needs to be rebuilt
            if (fShutdown)
                return;
            if (!fGenerateBitcoins)
                return;
            if (fLimitProcessors && vnThreadsRunning[3] > nLimitProcessors)
                return;
            if (vNodes.empty())
                break;
            if (pblock->nNonce >= 0xffff0000)
                break;
            if (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                break;
            if (pindexPrev != pindexBest)
                break;

            // Update nTime every few seconds
            pblock->nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());
            nBlockTime = ByteReverse(pblock->nTime);
        }
    }
}


void static ThreadBitcoinMiner(void* parg)
{
    CWallet* pwallet = (CWallet*)parg;
    try
    {
        vnThreadsRunning[3]++;
        if (miningAlgo == ALGO_SHA256D)
            BitcoinMiner(pwallet);
        else if (miningAlgo == ALGO_SCRYPT)
            ScryptMiner(pwallet);
        else
            error("ThreadBitcoinMiner: unknown algo");
        vnThreadsRunning[3]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[3]--;
        PrintException(&e, "ThreadBitcoinMiner()");
    } catch (...) {
        vnThreadsRunning[3]--;
        PrintException(NULL, "ThreadBitcoinMiner()");
    }
    UIThreadCall(boost::bind(CalledSetStatusBar, "", 0));
    nHPSTimerStart = 0;
    if (vnThreadsRunning[3] == 0)
        dHashesPerSec = 0;
    printf("ThreadBitcoinMiner exiting, %d threads remaining\n", vnThreadsRunning[3]);
}


void GenerateBitcoins(bool fGenerate, CWallet* pwallet)
{
    if (fGenerateBitcoins != fGenerate)
    {
        fGenerateBitcoins = fGenerate;
        WriteSetting("fGenerateBitcoins", fGenerateBitcoins);
    }
    if (fGenerateBitcoins)
    {
        int nProcessors = boost::thread::hardware_concurrency();
        printf("%d processors\n", nProcessors);
        if (nProcessors < 1)
            nProcessors = 1;
        if (fLimitProcessors && nProcessors > nLimitProcessors)
            nProcessors = nLimitProcessors;
        int nAddThreads = nProcessors - vnThreadsRunning[3];
        printf("Starting %d BitcoinMiner threads\n", nAddThreads);
        for (int i = 0; i < nAddThreads; i++)
        {
            if (!CreateThread(ThreadBitcoinMiner, pwallet))
                printf("Error: CreateThread(ThreadBitcoinMiner) failed\n");
            MilliSleep(10);
        }
    }
}


// A helper function to compute nonce for the genesis block. The resulting hash
// is printed to debug.log and has to be manually copied from there.
// In the production code calls to this function should be commented out
// and the hash should be hard-coded.
void MineGenesisBlock(CBlock *pblock, bool fUpdateBlockTime /* = true*/)
{
    printf("# Mining genesis block...\n");

    int64 nPrevTime = 0;

    //
    // Prebuild hash buffers
    //
    char pmidstatebuf[32+16]; char* pmidstate = alignup<16>(pmidstatebuf);
    char pdatabuf[128+16];    char* pdata     = alignup<16>(pdatabuf);
    char phash1buf[64+16];    char* phash1    = alignup<16>(phash1buf);

    FormatHashBuffers(pblock, pmidstate, pdata, phash1);

    unsigned int& nBlockTime = *(unsigned int*)(pdata + 64 + 4);
    unsigned int& nBlockNonce = *(unsigned int*)(pdata + 64 + 12);

    if (fUpdateBlockTime)
    {
        pblock->nTime = GetAdjustedTime();
        nBlockTime = ByteReverse(pblock->nTime);
    }

    //
    // Search
    //
    int64 nStart = GetTime();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    uint256 hashbuf[2];
    uint256& hash = *alignup<16>(hashbuf);
    loop
    {
        unsigned int nHashesDone = 0;
        unsigned int nNonceFound;

        // Crypto++ SHA-256
        nNonceFound = ScanHash_CryptoPP(pmidstate, pdata + 64, phash1,
                                        (char*)&hash, nHashesDone);

        // Check if something found
        if (nNonceFound != -1)
        {
            for (int i = 0; i < sizeof(hash)/4; i++)
                ((unsigned int*)&hash)[i] = ByteReverse(((unsigned int*)&hash)[i]);

            if (hash <= hashTarget)
            {
                // Found a solution
                pblock->nNonce = ByteReverse(nNonceFound);
                assert(hash == pblock->GetHash());

                printf("# Genesis block miner: solution found\n");

                break;
            }
        }

        // Meter hashes/sec
        static int64 nHashCounter;
        if (nHPSTimerStart == 0)
        {
            nHPSTimerStart = GetTimeMillis();
            nHashCounter = 0;
        }
        else
            nHashCounter += nHashesDone;
        if (GetTimeMillis() - nHPSTimerStart > 30000)
        {
            static CCriticalSection cs;
            CRITICAL_BLOCK(cs)
            {
                if (GetTimeMillis() - nHPSTimerStart > 30000)
                {
                    dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                    nHPSTimerStart = GetTimeMillis();
                    nHashCounter = 0;
                    string strStatus = strprintf("    %.0f khash/s", dHashesPerSec/1000.0);
                    printf("%s ", DateTimeStrFormat("%x %H:%M", GetTime()).c_str());
                    printf("hashmeter %6.0f khash/s\n", dHashesPerSec/1000.0);
                }
            }
        }

        if (fUpdateBlockTime)
        {
            // Update nTime every few seconds
            pblock->nTime = GetAdjustedTime();
            nBlockTime = ByteReverse(pblock->nTime);
        }
    }
}

int64
CBlockIndex::GetTotalRewards () const
{
  /* Initialise with premine.  */
  int64 total;
  if (fTestNet)
    total = 100 * COIN;
  else
    total = 85000 * COIN;

  /* The genesis block had no ordinary mining reward, compensate for this.  */
  total -= GetBlockValue (0, 0);

  /* Sum up rewards (mining + harvest) over previous times.  */
  for (int h = nHeight; h >= 0; --h)
    total += GetBlockValue (h, 0) * 10;

  return total;
}

std::string CBlockIndex::ToString() const
{
    return strprintf("CBlockIndex(nprev=%08x, pnext=%08x, nFile=%d, nBlockPos=%-6d nHeight=%d, merkle=%s, hashBlock=%s)",
            pprev, pnext, nFile, nBlockPos, nHeight,
            hashMerkleRoot.ToString().substr(0,10).c_str(),
            GetBlockHash().ToString().substr(0,20).c_str());
}

CBigNum CBlockIndex::GetBlockWork() const
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);
    if (bnTarget <= 0)
        return 0;
    CBigNum work = (CBigNum(1)<<256) / (bnTarget+1);

    // Apply scrypt-to-SHA ratio
    // We assume that scrypt is 2^12 times harder to mine (for the same difficulty target)
    // This only affects how a longer chain is selected in case of conflict
    if (GetAlgo() == ALGO_SCRYPT)
        work <<= 12;

    return work;
}
