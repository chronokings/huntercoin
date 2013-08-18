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

bool PerformStep(CNameDB &dbName, const Game::GameState &inState, const CBlock *block, Game::GameState &outState, std::vector<CTransaction> &outvgametx);

bool GetGameState(CTxDB &txdb, CBlockIndex *pindex, Game::GameState &outState);
bool AdvanceGameState(CTxDB &txdb, CBlockIndex *pindex, CBlock *block);
void RollbackGameState(CTxDB& txdb, CBlockIndex* pindex);

#endif
