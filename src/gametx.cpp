#include "gametx.h"
#include "gamestate.h"

#include "headers.h"
#include "huntercoin.h"
#include "script.h"

using namespace Game;

// Opcodes for scriptSig that acts as coinbase for game-generated transactions.
// They serve merely for information purposes, so the client can know why it got this transaction.
// In the future, for some really complex transactions, this data can be encoded in scriptPubKey
// followed by OP_DROPs.
enum
{
    // Syntax (scriptSig):
    //     victim GAMEOP_KILLED_BY killer1 killer2 ... killerN
    // Player can be killed simultaneously by multiple other players.
    // If N = 0, player was killed by the game (for staying for too long in the spawn area)
    GAMEOP_KILLED_BY = 1,

    // Syntax (scriptSig):
    //     player GAMEOP_COLLECTED_BOUNTY characterIndex firstBlock lastBlock collectedFirstBlock collectedLastBlock
    // vin.size() == vout.size(), they correspond to each other, i.e. a dummy input is used
    // to hold info about the corresponding output in its scriptSig
    // (alternatively we could add vout index to the scriptSig, to allow more complex transactions
    // with arbitrary input assignments, or store it in scriptPubKey of the tx-out instead)
    GAMEOP_COLLECTED_BOUNTY = 2,
};

bool CreateGameTransactions(CNameDB *pnameDb, const GameState &gameState, const StepResult &stepResult, std::vector<CTransaction> &outvgametx)
{
    // Create resulting game transactions
    // Transaction hashes must be unique
    outvgametx.clear();

    CTransaction txNew;
    txNew.SetGameTx();

    // Destroy name-coins of killed players
    txNew.vin.reserve(stepResult.killedPlayers.size());
    BOOST_FOREACH(const PlayerID &victim, stepResult.killedPlayers)
    {
        std::vector<unsigned char> vchName = vchFromString(victim);
        CTransaction tx;
        if (!pnameDb || !GetTxOfNameAtHeight(*pnameDb, vchName, gameState.nHeight, tx))
            return error("Game engine killed a non-existing player %s", victim.c_str());

        CTxIn txin(tx.GetHash(), IndexOfNameOutput(tx));

        txin.scriptSig << vchName << GAMEOP_KILLED_BY;

        // List all killers, if player was simultaneously killed by several other players
        typedef std::multimap<PlayerID, CharacterID>::const_iterator Iter;
        std::pair<Iter, Iter> iters = stepResult.killedBy.equal_range(victim);
        for (Iter it = iters.first; it != iters.second; ++it)
            txin.scriptSig << vchFromString(it->second.ToString());
        txNew.vin.push_back(txin);
    }
    if (!txNew.IsNull())
        outvgametx.push_back(txNew);

    // Pay bounties to the players who collected them
    txNew.SetNull();
    txNew.SetGameTx();
    txNew.vin.reserve(stepResult.bounties.size());      // Dummy inputs that contain informational messages only (one per each output)
    txNew.vout.reserve(stepResult.bounties.size());

    BOOST_FOREACH(const PAIRTYPE(CharacterID, CollectedLootInfo) &bounty, stepResult.bounties)
    {
        std::vector<unsigned char> vchName = vchFromString(bounty.first.player);
        CTransaction tx;
        if (!pnameDb || !GetTxOfNameAtHeight(*pnameDb, vchName, gameState.nHeight, tx))
            return error("Game engine created bounty for non-existing player");

        CTxOut txout;
        txout.nValue = bounty.second.nAmount;

        // Note: we only use the resulting game state to pay rewards. If we need to pay them to just-killed players,
        // this function should be modified to accept two game states, or the game state must be augmented with deadPlayers array.

        std::map<PlayerID, PlayerState>::const_iterator mi = gameState.players.find(bounty.first.player);
        if (mi == gameState.players.end())
            return error("Game engine created bounty for non-existing (dead?) player");

        if (!mi->second.address.empty())
        {
            // Player-provided addresses are validated before accepting them, so failing
            // here is ok
            if (!txout.scriptPubKey.SetBitcoinAddress(mi->second.address))
                return error("Failed to set player-provided address for bounty");
        }
        else
        {
            // TODO: Maybe pay to the script of the name-tx without extracting the address first
            // (see source of GetNameAddress - it obtains the script by calling RemoveNameScriptPrefix)
            uint160 addr;
            if (!GetNameAddress(tx, addr))
                return error("Cannot get name address for bounty");
            txout.scriptPubKey.SetBitcoinAddress(addr);
        }

        txNew.vout.push_back(txout);

        CTxIn txin;
        txin.scriptSig << vchName << GAMEOP_COLLECTED_BOUNTY
                << bounty.first.index
                << bounty.second.firstBlock
                << bounty.second.lastBlock
                << bounty.second.collectedFirstBlock
                << bounty.second.collectedLastBlock
            ;
        txNew.vin.push_back(txin);
    }
    if (!txNew.IsNull())
        outvgametx.push_back(txNew);

    return true;
}

std::string GetGameTxDescription(const CScript &scriptSig, bool fBrief,
                                 const char *nameStartTag /* = ""*/, const char *nameEndTag /* = ""*/,
                                 bool fUseColon /* = true*/)
{
    if (fBrief)
        fUseColon = false;

    std::string strRet;
    CScript::const_iterator pc = scriptSig.begin();

    opcodetype opcode;

    std::vector<unsigned char> vch;
    if (!scriptSig.GetOp(pc, opcode, vch))
        return strRet;

    strRet.reserve(50);

    strRet += _("Player");
    strRet += " ";
    strRet += nameStartTag;
    std::string strPlayerName = stringFromVch(vch);
    strRet += strPlayerName;
    strRet += nameEndTag;
    
    // Colon is needed to separate text from player name, since player name can contain whitespaces.
    // When HTML is used, player name can be made bold and colon won't be needed, hence the switch for it.
    if (fUseColon)
        strRet += ":";

    if (!scriptSig.GetOp(pc, opcode))
        return strRet;

    bool fFirst = true; // For writing comma-separated values
    unsigned char characterIndex;

    switch (opcode - OP_1 + 1)
    {
        case GAMEOP_KILLED_BY:
            if (fBrief)
                return strRet + " " + _("is killed");
            strRet += " ";
            while (scriptSig.GetOp(pc, opcode, vch))
            {
                if (!fFirst)
                    strRet += ", ";
                else
                {
                    fFirst = false;
                    strRet += _("killed by");
                    strRet += " ";
                }
                std::string s = stringFromVch(vch);
                if (s == strPlayerName)
                    strRet += "self-destruction";
                else
                {
                    strRet += nameStartTag;
                    strRet += s;
                    strRet += nameEndTag;
                }
            }
            if (fFirst)
                strRet += _("killed for staying too long in the spawn area");
            break;
        case GAMEOP_COLLECTED_BOUNTY:
            scriptSig.GetOp(pc, opcode, vch);
            if (opcode >= OP_1)
                strRet += strprintf(".%d", int(opcode - OP_1 + 1));
            strRet += " ";
            strRet += _("collected bounty");
            break;
        default:
            strRet += " ";
            strRet += _("(unknown tx type)");
            break;
    }
    return strRet;
}
