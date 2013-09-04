#include "gamedb.h"
#include "gamestate.h"
#include "gametx.h"

#include "headers.h"
#include "chronokings.h"

using namespace Game;

static const int KEEP_EVERY_NTH_STATE = 2016;

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
    const GameState *pstate;
    bool fOwnState;

    // Detect duplicates (multiple moves per block). Probably already handled by NameDB and not needed.
    std::set<PlayerID> dup;
    
    // Temporary variables
    std::vector<unsigned char> vchName;
    std::vector<unsigned char> vchValue;

public:
    GameStepValidator(const GameState *pstate_)
        : pstate(pstate_), fOwnState(false)
    {
    }

    GameStepValidator(CTxDB &txdb, CBlockIndex *pindex)
        : fOwnState(true)
    {
        GameState *newState = new GameState;
        if (!GetGameState(txdb, pindex, *newState))
            throw std::runtime_error("GameStepValidator : cannot get previous game state");
        pstate = newState;
    }

    ~GameStepValidator()
    {
        if (fOwnState)
            delete pstate;
    }

    // Returns:
    //   false - invalid move tx (m becomes undefined)
    //   true  - non-move tx (m set to NULL) or valid tx (move stored to m, caller must delete it)
    bool IsValid(const CTransaction& tx, const Move *&m)
    {
        if (!GetNameOfTx(tx, vchName) || !GetValueOfNameTx(tx, vchValue))
        {
            m = NULL;
            return true;
        }

        std::string sName = stringFromVch(vchName), sValue = stringFromVch(vchValue);
        if (dup.count(sName))
            return error("GameStepValidator: duplicate player name %s", sName.c_str());
        dup.insert(sName);
        m = Move::Parse(sName, sValue);
        if (!m)
            return error("GameStepValidator: cannot parse move %s for player %s", sValue.c_str(), sName.c_str());
        if (!m->IsValid(*pstate))
        {
            delete m;
            return error("Invalid move for the game state: move %s for player %s", sValue.c_str(), sName.c_str());
        }
        return true;
    }

    bool IsValid(const CTransaction& tx)
    {
        const Move *m;
        if (!IsValid(tx, m))
            return false;
        delete m;
        return true;
    }
};

// A simple wrapper (pImpl patter) to remove dependency on the game-related headers when miner just wants to check transactions
// (declared in hooks.h)
GameStepValidatorMiner::GameStepValidatorMiner(CTxDB &txdb, CBlockIndex *pindex)
    : pImpl(new GameStepValidator(txdb, pindex))
{
}

GameStepValidatorMiner::~GameStepValidatorMiner()
{
    delete pImpl;
}

bool GameStepValidatorMiner::IsValid(const CTransaction& tx)
{
    return pImpl->IsValid(tx);
}

bool PerformStep(CNameDB *pnameDb, const GameState &inState, const CBlock *block, GameState &outState, std::vector<CTransaction> &outvgametx)
{
    if (block->hashPrevBlock != inState.hashBlock)
        return error("PerformStep: game state for wrong block");

    StepData stepData;
    stepData.nNameCoinAmount = NAME_COIN_AMOUNT;
    stepData.nTreasureAmount = GetBlockValue(inState.nHeight + 1, 0);
    stepData.newHash = block->GetHash();

    GameStepValidator gameStepValidator(&inState);
    // Create moves for all move transactions
    BOOST_FOREACH(const CTransaction& tx, block->vtx)
    {
        const Move *m;
        if (!gameStepValidator.IsValid(tx, m))
            return error("GameStepValidator rejected transaction %s in block %s", tx.GetHash().ToString().substr(0,10).c_str(), block->GetHash().ToString().c_str());
        if (m)
            stepData.vpMoves.push_back(m);
    }

    StepResult stepResult;
    if (!Game::PerformStep(inState, stepData, outState, stepResult))
        return error("PerformStep failed for block %s", block->GetHash().ToString().c_str());

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
bool GetGameState(CTxDB &txdb, CBlockIndex *pindex, GameState &outState)
{
    if (!pindex)
    {
        outState = GameState();
        return true;
    }

    if (*pindex->phashBlock == currentState.hashBlock)
    {
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

    CBlockIndex *plast = pindex;
    GameState lastState;
    for (; plast->pprev; plast = plast->pprev)
    {
        if (gameDb.Read(plast->pprev->nHeight, lastState))
            break;
    }

    // When connecting genesis block, there is no nameindexfull.dat file yet
    std::auto_ptr<CNameDB> nameDb(pindex == pindexGenesisBlock ? NULL : new CNameDB("r", txdb));

    // Integrate steps starting from the last saved state
    loop
    {
        std::vector<CTransaction> vgametx;

        CBlock block;
        block.ReadFromDisk(plast);
        
        if (!PerformStep(nameDb.get(), lastState, &block, outState, vgametx))
            return false;
        if (block.vgametx != vgametx)
        {
            printf("Error: GetGameState: computed vgametx is different from the stored one\n");
            printf("  block vgametx:\n");
            BOOST_FOREACH (const CTransaction &tx, block.vgametx)
            {
                printf("    ");
                tx.print();
            }
            printf("  computed vgametx:\n");
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
    if (pindex == pindexBest)
        currentState = outState;

    if (outState.nHeight % KEEP_EVERY_NTH_STATE == 0)
    {
        CGameDB gameDb("r+", txdb);
        gameDb.Write(outState.nHeight, outState);
    }

    return true;
}

// Called from ConnectBlock
bool AdvanceGameState(CTxDB &txdb, CBlockIndex *pindex, CBlock *block)
{
    GameState inState, outState;
    if (!GetGameState(txdb, pindex->pprev, inState))
        return false;

    if (inState.nHeight != pindex->nHeight - 1)
        return error("AdvanceGameState: incorrect height encountered");
    if (inState.hashBlock != block->hashPrevBlock)
        return error("AdvanceGameState: incorrect hash encountered");

    {
        // When connecting genesis block, there is no nameindexfull.dat file yet
        std::auto_ptr<CNameDB> nameDb(pindex == pindexGenesisBlock ? NULL : new CNameDB("r", txdb));

        if (!PerformStep(nameDb.get(), inState, block, outState, block->vgametx))
            return false;
    }

    if (outState.nHeight != pindex->nHeight)
        return error("AdvanceGameState: incorrect height stored");
    if (outState.hashBlock != *pindex->phashBlock)
        return error("AdvanceGameState: incorrect hash stored");

    currentState = outState;

    CGameDB gameDb("cr+", txdb);
    gameDb.Write(pindex->nHeight, outState);
    // Prune old states from DB, keeping every Nth for quick lookup (intermediate states can be obtained by integrating blocks)
    if (pindex->nHeight - 1 <= 0 || (pindex->nHeight - 1) % KEEP_EVERY_NTH_STATE != 0)
        gameDb.Erase(pindex->nHeight - 1);

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
