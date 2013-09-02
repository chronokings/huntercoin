#ifndef GAMEDB_H
#define GAMEDB_H

#include <vector>

namespace Game
{
    struct GameState;
}

class CBlock;
class CTransaction;
class CBlockIndex;
class CTxDB;
class CNameDB;

// Opcodes for CScript() that acts as coinbase for game-generated transactions.
// They serve merely for information purposes, so the client can know why he got this transaction.
enum
{
    // Syntax (scriptSig):
    // victim GAMEOP_KILLED_BY killer1 killer2 ... killerN
    // (player can be killed simultaneously by multiple other players)
    GAMEOP_KILLED_BY = 1,

    // Syntax (scriptSig):
    // player GAMEOP_COLLECTED_BOUNTY x y firstBlock lastBlock
    // vin.size() == vout.size(), they correspond to each other
    // (alternatively we could add vout index to the scriptSig, to allow more complex transactions
    // with arbitrary input assignments)
    GAMEOP_COLLECTED_BOUNTY = 2,
};

bool PerformStep(CNameDB *pnameDb, const Game::GameState &inState, const CBlock *block, Game::GameState &outState, std::vector<CTransaction> &outvgametx);

// These functions must hold cs_main lock
bool GetGameState(CTxDB &txdb, CBlockIndex *pindex, Game::GameState &outState);
bool AdvanceGameState(CTxDB &txdb, CBlockIndex *pindex, CBlock *block);
void RollbackGameState(CTxDB& txdb, CBlockIndex* pindex);

// Must hold cs_main lock
const Game::GameState &GetCurrentGameState();

// Like name_clean; called in ResendWalletTransactions to remove outdated move transactions that are
// no longer valid for the current game state
void EraseBadMoveTransactions();

#endif
