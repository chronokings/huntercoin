#include "gamedb.h"
#include "gamestate.h"
#include "gametx.h"

#include "headers.h"
#include "huntercoin.h"

using namespace Game;

static const int KEEP_EVERY_NTH_STATE = 2000;

class CGameDB : public CDB
{
protected:
    bool fHaveParent;
public:
    CGameDB(const char* pszMode="r+") : CDB("game.dat", pszMode) {
        fHaveParent = false;
    }

    CGameDB(const char* pszMode, CDB& parent) : CDB("game.dat", pszMode) {
        vTxn.push_back(parent.GetTxn());
        fHaveParent = true;
    }

    ~CGameDB()
    {
        if (fHaveParent)
            vTxn.erase(vTxn.begin());
    }

    bool Read(unsigned int nHeight, GameState &gameState)
    {
        return CDB::Read(nHeight, gameState);
    }

    bool Write(unsigned int nHeight, const GameState &gameState)
    {
        return CDB::Write(nHeight, gameState);
    }

    bool Erase(unsigned int nHeight)
    {
        return CDB::Erase(nHeight);
    }
};

class GameStepValidator
{
    bool fOwnState;
    bool fOwnTxDB;
    
    CTxDB *pTxDB;

    // Detect duplicates (multiple moves per block). Probably already handled by NameDB and not needed.
    std::set<PlayerID> dup;

    // Temporary variables
    std::vector<unsigned char> vchName;
    std::vector<unsigned char> vchValue;

protected:
    const GameState *pstate;

public:
    GameStepValidator(const GameState *pstate_)
        : pstate(pstate_), fOwnState(false), pTxDB(NULL)
    {
    }

    GameStepValidator(CTxDB &txdb, CBlockIndex *pindex)
        : fOwnState(true), pTxDB(&txdb), fOwnTxDB(false)
    {
        GameState *newState = new GameState;
        if (!GetGameState(txdb, pindex, *newState))
        {
            delete newState;
            throw std::runtime_error("GameStepValidator : cannot get previous game state");
        }
        pstate = newState;
    }

    ~GameStepValidator()
    {
        if (fOwnState)
            delete pstate;
        if (pTxDB && fOwnTxDB)
            delete pTxDB;
    }

    // Returns:
    //   false - invalid move tx
    //   true  - non-move tx or valid tx
    bool IsValid(const CTransaction& tx, Move &outMove)
    {
        if (!GetNameOfTx(tx, vchName) || !GetValueOfNameTx(tx, vchValue))
            return true;

        std::string sName = stringFromVch(vchName), sValue = stringFromVch(vchValue);
        if (dup.count(sName))
            return error("GameStepValidator: duplicate player name %s", sName.c_str());
        dup.insert(sName);

        Move m;
        m.Parse(sName, sValue);
        if (!m)
            return error("GameStepValidator: cannot parse move %s for player %s", sValue.c_str(), sName.c_str());
        if (!m.IsValid(*pstate))
            return error("GameStepValidator: invalid move for the game state: move %s for player %s", sValue.c_str(), sName.c_str());
        std::string addressLock = m.AddressOperationPermission(*pstate);
        if (!addressLock.empty())
        {
            // If one of inputs has address equal to addressLock, then that input has been signed by the address owner
            // and thus authorizes the address change operation
            bool found = false;
            if (!pTxDB)
            {
                pTxDB = new CTxDB("r");
                fOwnTxDB = true;
            }
            for (int i = 0; i < tx.vin.size(); i++)
            {
                COutPoint prevout = tx.vin[i].prevout;
                CTransaction txPrev;
                CTxIndex txindex;
                if (!pTxDB->ReadTxIndex(prevout.hash, txindex) || txindex.pos == CDiskTxPos(1,1,1))
                    continue;
                else if (!txPrev.ReadFromDisk(txindex.pos))
                    continue;
                if (prevout.n >= txPrev.vout.size())
                    continue;
                const CTxOut &vout = txPrev.vout[prevout.n];
                std::string address;
                if (ExtractDestination(vout.scriptPubKey, address) && address == addressLock)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                return error("GameStepValidator: address operation permission denied: move %s for player %s", sValue.c_str(), sName.c_str());
        }
        outMove = m;
        return true;
    }

    bool IsValid(const CTransaction& tx)
    {
        Move m;
        if (!IsValid(tx, m))
            return false;
        return true;
    }
};

static void InitStepData(StepData &stepData, const GameState &state)
{
    stepData.nNameCoinAmount = NAME_COIN_AMOUNT;
    int64 nSubsidy = GetBlockValue(state.nHeight + 1, 0);
    // Miner subsidy is 10%, thus game treasure is 9 times the subsidy
    stepData.nTreasureAmount = nSubsidy * 9;
}

class GameStepMinerImpl : public GameStepValidator
{
    StepData stepData;
public:
    GameStepMinerImpl(CTxDB &txdb, CBlockIndex *pindex)
        : GameStepValidator(txdb, pindex)
    {
        InitStepData(stepData, *pstate);
    }

    bool AddTx(const CTransaction& tx)
    {
        Move m;
        if (!IsValid(tx, m))
            return false;
        if (m)
            stepData.vMoves.push_back(m);

        return true;
    }

    int64 ComputeTax()
    {
        StepResult stepResult;
        Game::GameState outState;
        if (!Game::PerformStep(*pstate, stepData, outState, stepResult))
        {
            error("GameStepMinerImpl::ComputeTax failed");
            return 0;
        }

        return stepResult.nTaxAmount;
    }
};

// A simple wrapper (pImpl pattern) to remove dependency on the game-related headers when miner just wants to check transactions
// (declared in hooks.h)
GameStepMiner::GameStepMiner(CTxDB &txdb, CBlockIndex *pindex)
    : pImpl(new GameStepMinerImpl(txdb, pindex))
{
}

GameStepMiner::~GameStepMiner()
{
    delete pImpl;
}

bool GameStepMiner::AddTx(const CTransaction& tx)
{
    return pImpl->AddTx(tx);
}

int64 GameStepMiner::ComputeTax()
{
    return pImpl->ComputeTax();
}

bool PerformStep(CNameDB *pnameDb, const GameState &inState, const CBlock *block, int64 &nTax, GameState &outState, std::vector<CTransaction> &outvgametx)
{
    if (block->hashPrevBlock != inState.hashBlock)
        return error("PerformStep: game state for wrong block");

    StepData stepData;
    InitStepData(stepData, inState);
    stepData.newHash = block->GetHash();

    GameStepValidator gameStepValidator(&inState);
    // Create moves for all move transactions
    BOOST_FOREACH(const CTransaction& tx, block->vtx)
    {
        Move m;
        if (!gameStepValidator.IsValid(tx, m))
            return error("GameStepValidator rejected transaction %s in block %s", tx.GetHash().ToString().substr(0,10).c_str(), block->GetHash().ToString().c_str());
        if (m)
            stepData.vMoves.push_back(m);
    }

    StepResult stepResult;
    if (!Game::PerformStep(inState, stepData, outState, stepResult))
        return error("PerformStep failed for block %s", block->GetHash().ToString().c_str());

    nTax = stepResult.nTaxAmount;

    return CreateGameTransactions(pnameDb, outState, stepResult, outvgametx);
}

static GameState currentState;

// Caller must hold cs_main lock
const GameState &GetCurrentGameState()
{
    if (currentState.nHeight != nBestHeight)
    {
        CTxDB txdb("r");
        GetGameState(txdb, pindexBest, currentState);
    }
    return currentState;
}

// pindex must belong to the main branch, i.e. corresponding blocks must be connected
// Returns a copy of the game state
bool GetGameState(CTxDB &txdb, CBlockIndex *pindex, GameState &outState)
{
    if (!pindex)
    {
        outState = GameState();
        return true;
    }

    if (*pindex->phashBlock == currentState.hashBlock)
    {
        /* In GetCurrentState(), this routine is called with currentState
           as outState.  Avoid a redundant copy operation.  The default
           operator= does this, AFAIK, but make it explicit here
           also for clarity.  */
        if (&outState != &currentState)
            outState = currentState;
        return true;
    }

    // Get the latest saved state
    CGameDB gameDb("cr", txdb);

    if (gameDb.Read(pindex->nHeight, outState))
    {
        if (outState.nHeight != pindex->nHeight)
            return error("GetGameState: wrong height");
        if (outState.hashBlock != *pindex->phashBlock)
            return error("GetGameState: wrong hash");
        return true;
    }

    if (!pindex->IsInMainChain())
        return error("GetGameState called for non-main chain");

    printf("%s ", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());
    printf("GetGameState: need to integrate state for height %d (current %d)\n",
           pindex->nHeight, currentState.nHeight);

    CBlockIndex *plast = pindex;
    GameState lastState;
    for (; plast->pprev; plast = plast->pprev)
    {
        if (gameDb.Read(plast->pprev->nHeight, lastState))
            break;
    }

    printf("%s ", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());
    printf("GetGameState: last saved block has height %d\n", lastState.nHeight);

    // When connecting genesis block, there is no nameindexfull.dat file yet
    std::auto_ptr<CNameDB> nameDb(pindex == pindexGenesisBlock ? NULL : new CNameDB("r", txdb));

    // Integrate steps starting from the last saved state
    loop
    {
        std::vector<CTransaction> vgametx;

        CBlock block;
        block.ReadFromDisk(plast);

        int64 nTax;
        if (!PerformStep(nameDb.get(), lastState, &block, nTax, outState, vgametx))
            return false;
        if (block.vgametx != vgametx)
        {
            printf("Error: GetGameState: computed vgametx is different from the stored one\n");
            printf("  block %s (height = %d) vgametx:\n", block.GetHash().ToString().c_str(), plast->nHeight);
            BOOST_FOREACH (const CTransaction &tx, block.vgametx)
            {
                printf("    ");
                tx.print();
            }
            printf("  computed vgametx (height = %d):\n", outState.nHeight);
            BOOST_FOREACH (const CTransaction &tx, vgametx)
            {
                printf("    ");
                tx.print();
            }
            return false;
        }
        if (outState.nHeight != plast->nHeight)
            return error("GetGameState: wrong height");
        if (outState.hashBlock != *plast->phashBlock)
            return error("GetGameState: wrong hash");
        if (plast == pindex)
            break;
        plast = plast->pnext;
        lastState = outState;
    }
    if (pindex == pindexBest && &outState != &currentState)
        currentState = outState;

    printf("%s ", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());
    printf("GetGameState: done integrating\n");

    if (outState.nHeight % KEEP_EVERY_NTH_STATE == 0)
    {
        CGameDB gameDb("r+", txdb);
        gameDb.Write(outState.nHeight, outState);
    }

    return true;
}

// Called from ConnectBlock
bool AdvanceGameState(CTxDB &txdb, CBlockIndex *pindex, CBlock *block, int64 &nFees)
{
    GameState outState;

    if (currentState.hashBlock != block->hashPrevBlock)
    {
        if (!GetGameState(txdb, pindex->pprev, currentState))
            return error("AdvanceGameState: cannot get current game state");
    }

    if (currentState.nHeight != pindex->nHeight - 1)
        return error("AdvanceGameState: incorrect height encountered");
    if (currentState.hashBlock != block->hashPrevBlock)
        return error("AdvanceGameState: incorrect hash encountered");

    int64 nTax = 0;

    {
        // When connecting genesis block, there is no nameindexfull.dat file yet
        std::auto_ptr<CNameDB> nameDb(pindex == pindexGenesisBlock ? NULL : new CNameDB("r", txdb));

        if (!PerformStep(nameDb.get(), currentState, block, nTax, outState, block->vgametx))
            return false;
    }

    if (outState.nHeight != pindex->nHeight)
        return error("AdvanceGameState: incorrect height stored");
    if (outState.hashBlock != *pindex->phashBlock)
        return error("AdvanceGameState: incorrect hash stored");

    // Here we optimistically assume that the block being connected
    // will be connected successfully and pindexBest will equal pindex
    currentState = outState;

    CGameDB gameDb("cr+", txdb);
    gameDb.Write(pindex->nHeight, outState);
    // Prune old states from DB, keeping every Nth for quick lookup (intermediate states can be obtained by integrating blocks)
    if (pindex->nHeight - 1 <= 0 || (pindex->nHeight - 1) % KEEP_EVERY_NTH_STATE != 0)
        gameDb.Erase(pindex->nHeight - 1);

    nFees += nTax;

    return true;
}

// Called from DisconnectBlock
void RollbackGameState(CTxDB& txdb, CBlockIndex* pindex)
{
    if (!pindex->IsInMainChain())
    {
        error("RollbackGameState called for non-main chain");
        return;
    }

    CGameDB("r+", txdb).Erase(pindex->nHeight);
}

extern CWallet* pwalletMain;

// Erase unconfirmed transactions, that should not be re-broadcasted, because they're not
// valid for the current game state (e.g. attack on some player who's already killed)
void EraseBadMoveTransactions()
{
    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(pwalletMain->cs_mapWallet)
    {
        std::map<uint256, CWalletTx> mapRemove;
        std::vector<unsigned char> vchName;

        GameStepValidator gameStepValidator(&currentState);

        {
            CTxDB txdb("r");
            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
            {
                CWalletTx& wtx = item.second;

                if (wtx.GetDepthInMainChain() < 1 && (IsConflictedTx(txdb, wtx, vchName) || !gameStepValidator.IsValid(wtx)))
                    mapRemove[wtx.GetHash()] = wtx;
            }
        }

        if (mapRemove.empty())
            return;

        int countRemove = mapRemove.size();
        printf("EraseBadMoveTransactions : erasing %d transactions\n", countRemove);

        bool fRepeat = true;
        while (fRepeat)
        {
            fRepeat = false;
            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
            {
                CWalletTx& wtx = item.second;
                BOOST_FOREACH(const CTxIn& txin, wtx.vin)
                {
                    uint256 hash = wtx.GetHash();

                    // If this tx depends on a tx to be removed, remove it too
                    if (mapRemove.count(txin.prevout.hash) && !mapRemove.count(hash))
                    {
                        mapRemove[hash] = wtx;
                        fRepeat = true;
                    }
                }
            }
        }
        if (mapRemove.size() > countRemove)
            printf("EraseBadMoveTransactions : erasing additional %d dependent transactions\n", mapRemove.size() - countRemove);

        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapRemove)
        {
            CWalletTx& wtx = item.second;

            UnspendInputs(wtx);
            wtx.RemoveFromMemoryPool();
            pwalletMain->EraseFromWallet(wtx.GetHash());
            if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName))
            {
                std::string name = stringFromVch(vchName);
                printf("EraseBadMoveTransactions : erase %s from pending of name %s",
                        wtx.GetHash().GetHex().c_str(), name.c_str());
                if (!mapNamePending[vchName].erase(wtx.GetHash()))
                    error("EraseBadMoveTransactions : erase but it was not pending");
            }
            wtx.print();
        }
    }
}

bool UpgradeGameDB()
{
    int nGameDbVersion = VERSION;

    {
        CGameDB gameDb("cr");
        gameDb.ReadVersion(nGameDbVersion);
        gameDb.Close();
    }

    if (nGameDbVersion < 1000500)
    {
        printf("Updating GameDB...\n");

        CGameDB gameDb("r+");

        GameState state;
        state.nVersion = nGameDbVersion;
        for (unsigned int i = 0; i <= nBestHeight; i++)
        {
            if (gameDb.Read(i, state))
            {
                state.UpdateVersion();
                if (!gameDb.Write(i, state))
                    return false;
                state.nVersion = nGameDbVersion;
            }
        }

        if (!gameDb.WriteVersion(VERSION))
            return false;
        printf("GameDB updated\n");
    }

    return true;
}
