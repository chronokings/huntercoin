#ifndef GAMEDB_H
#define GAMEDB_H

#include "uint256.h"

#include <vector>

// This module acts as a connection between the game engine (gamestate.cpp) and the block chain hook (huntercoin.cpp)

namespace Game
{
    struct GameState;
}

class CBlock;
class CTransaction;
class CBlockIndex;
class CTxDB;
class CNameDB;
class DatabaseSet;
class CScript;

/* Check a move tx for validity at the given game state.  */
bool IsMoveValid (const Game::GameState& state, const CTransaction& tx);

bool PerformStep (CNameDB& pnameDb, const Game::GameState& inState,
                  const CBlock* block, int64& nTax, Game::GameState& outState,
                  std::vector<CTransaction>* outvgametx = NULL);

// Caller of these functions must hold cs_main lock
bool GetGameState (DatabaseSet& dbset, CBlockIndex* pindex,
                   Game::GameState& outState);
bool AdvanceGameState (DatabaseSet& dbset, CBlockIndex* pindex,
                       CBlock* block, int64& nFees);
void RollbackGameState(CTxDB& txdb, CBlockIndex* pindex);
const Game::GameState &GetCurrentGameState();

// Like name_clean; called in ResendWalletTransactions to remove outdated move transactions that are
// no longer valid for the current game state
void EraseBadMoveTransactions();

/* Prune the game db by removing all states older than the given
   number of blocks.  Actually, we keep the newest state that is older
   than the treshold, so that we can integrate forward
   in time from there and (more or less) efficiently reconstruct
   every state after the treshold.  */
void PruneGameDB (unsigned nHeight);

bool UpgradeGameDB();

#endif // GAMEDB_H
