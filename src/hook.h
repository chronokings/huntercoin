// Copyright (c) 2010-2011 Vincent Durham
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HOOKS_H
#define BITCOIN_HOOKS_H

class CHooks
{
public:
    virtual bool IsStandard(const CScript& scriptPubKey) = 0;
    virtual void AddToWallet(CWalletTx& tx) = 0;
    virtual bool CheckTransaction(const CTransaction& tx) = 0;
    virtual bool ConnectInputs(CTxDB& txdb,
            std::map<uint256, CTxIndex>& mapTestPool,
            const CTransaction& tx,
            std::vector<CTransaction>& vTxPrev,
            std::vector<CTxIndex>& vTxindex,
            CBlockIndex* pindexBlock,
            CDiskTxPos& txPos,
            bool fBlock,
            bool fMiner) = 0;
    virtual bool DisconnectInputs(CTxDB& txdb,
            const CTransaction& tx,
            CBlockIndex* pindexBlock) = 0;
    virtual bool ConnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex, int64 &nFees, unsigned int nPosAfterTx) = 0;
    virtual bool DisconnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex) = 0;
    virtual bool ExtractAddress(const CScript& script, std::string& address) = 0;
    virtual bool GenesisBlock(CBlock& block) = 0;
    virtual bool Lockin(int nHeight, uint256 hash) = 0;
    virtual int LockinHeight() = 0;
    virtual std::string IrcPrefix() = 0;
    virtual void MessageStart(char* pchMessageStart) = 0;
    virtual void AcceptToMemoryPool(CTxDB& txdb, const CTransaction& tx) = 0;
    virtual void RemoveFromMemoryPool(const CTransaction& tx) = 0;

    /* These are for display and wallet management purposes.  Not for use to decide
     * whether to spend a coin. */
    virtual bool IsMine(const CTransaction& tx) = 0;
    virtual bool IsMine(const CTransaction& tx, const CTxOut& txout, bool ignore_name_new = false) = 0;

    // Allows overriding of the default fee for certain transaction types
    virtual void GetMinFee(int64 &nMinFee, int64 &nBaseFee, const CTransaction &tx,
        unsigned int nBlockSize, bool fAllowFree, bool fForRelay,
        unsigned int nBytes, unsigned int nNewBlockSize) = 0;

    virtual std::string GetAlertPubkey1() = 0;
    virtual std::string GetAlertPubkey2() { return GetAlertPubkey1(); }
};

// A simple wrapper (pImpl pattern) to remove dependency on the game-related headers when miner just wants to check transactions
class GameStepMiner
{
    class GameStepMinerImpl *pImpl;
public:
    GameStepMiner(CTxDB &txdb, CBlockIndex *pindex);
    ~GameStepMiner();
    bool AddTx(const CTransaction& tx);
    int64 ComputeTax();
};

extern CHooks* InitHook();
extern std::string GetDefaultDataDirSuffix();

#endif
