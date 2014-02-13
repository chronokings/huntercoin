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
class CScript;

bool PerformStep(CNameDB *pnameDb, const Game::GameState &inState, const CBlock *block, int64 &nTax, Game::GameState &outState, std::vector<CTransaction> &outvgametx);

// Caller of these functions must hold cs_main lock
bool GetGameState(CTxDB &txdb, CBlockIndex *pindex, Game::GameState &outState);
bool AdvanceGameState(CTxDB &txdb, CBlockIndex *pindex, CBlock *block, int64 &nFees);
void RollbackGameState(CTxDB& txdb, CBlockIndex* pindex);
const Game::GameState &GetCurrentGameState();

// Like name_clean; called in ResendWalletTransactions to remove outdated move transactions that are
// no longer valid for the current game state
void EraseBadMoveTransactions();

bool UpgradeGameDB();

#endif // GAMEDB_H
