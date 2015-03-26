#ifndef GAMETX_H
#define GAMETX_H

#include <vector>
#include <string>

#include "json/json_spirit.h"

// This module manages transactions for in-game events (e.g. player rewards):
// - creates transactions for StepResult
// - parses these transactions and creates string representation (to show in the transaction list)

namespace Game
{
    struct GameState;
    struct StepResult;
}

class CTransaction;
class CTxIn;
class CNameDB;
class CScript;

// Create resulting game transactions for the given StepResult
bool CreateGameTransactions (CNameDB& pnameDb, const Game::GameState& gameState,
                             const Game::StepResult& stepResult,
                             std::vector<CTransaction>& outvgametx);

/* See if a given tx input is a player death in a game tx.  If this is the case,
   the killed player's name is returned.  */
bool IsPlayerDeathInput (const CTxIn& in, std::vector<unsigned char>& name);

// Format human-readable description of a single input of game tx.
// Note: the caller must know the structure of game transactions, to correctly match txin and txout
// By providing nameStart/EndTag you can make player names bold in HTML.
// In brief mode details are omitted and fUseColon is ignored
std::string GetGameTxDescription(const CScript &scriptSig, bool fBrief, const char *nameStartTag = "", const char *nameEndTag = "", bool fUseColon = true);

/* Decode a game tx scriptsig into a JSON object.  */
void GameInputToJSON (const CScript& scriptSig, json_spirit::Object& o);

#endif // GAMETX_H
