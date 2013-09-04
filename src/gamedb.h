#ifndef GAMEDB_H
#define GAMEDB_H

#include <vector>

// This module acts as a connection between the game engine (gamestate.cpp) and the block chain hook (chronokings.cpp)

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

bool PerformStep(CNameDB *pnameDb, const Game::GameState &inState, const CBlock *block, Game::GameState &outState, std::vector<CTransaction> &outvgametx);

// Caller of these functions must hold cs_main lock
bool GetGameState(CTxDB &txdb, CBlockIndex *pindex, Game::GameState &outState);
bool AdvanceGameState(CTxDB &txdb, CBlockIndex *pindex, CBlock *block);
void RollbackGameState(CTxDB& txdb, CBlockIndex* pindex);
const Game::GameState &GetCurrentGameState();

// Like name_clean; called in ResendWalletTransactions to remove outdated move transactions that are
// no longer valid for the current game state
void EraseBadMoveTransactions();

#endif // GAMEDB_H
