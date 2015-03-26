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
    // If N = 0, player was killed for staying too long in spawn area.
    GAMEOP_KILLED_BY = 1,

    // Syntax (scriptSig):
    //     player GAMEOP_COLLECTED_BOUNTY characterIndex firstBlock lastBlock collectedFirstBlock collectedLastBlock
    // vin.size() == vout.size(), they correspond to each other, i.e. a dummy input is used
    // to hold info about the corresponding output in its scriptSig
    // (alternatively we could add vout index to the scriptSig, to allow more complex transactions
    // with arbitrary input assignments, or store it in scriptPubKey of the tx-out instead)
    GAMEOP_COLLECTED_BOUNTY = 2,

    // Syntax (scriptSig):
    //     victim GAMEOP_KILLED_POISON
    // Player was killed due to poisoning
    GAMEOP_KILLED_POISON = 3,

    // Syntax (scriptSig):
    //     player GAMEOP_REFUND characterIndex height
    // This is a tx to refund a player's coins after staying long
    // in the spawn area.  characterIndex is usually 0, but keep it
    // here for future extensibility.
    GAMEOP_REFUND = 4,

};

bool
CreateGameTransactions (CNameDB& nameDb, const GameState& gameState,
                        const StepResult& stepResult,
                        std::vector<CTransaction>& outvgametx)
{
  if (fDebug)
    printf ("Constructing game tx @%d...\n", gameState.nHeight);

  // Create resulting game transactions
  // Transaction hashes must be unique
  outvgametx.clear ();

  CTransaction txNew;
  txNew.SetGameTx ();

  // Destroy name-coins of killed players
  const PlayerSet& killedPlayers = stepResult.GetKilledPlayers ();
  const KilledByMap& killedBy = stepResult.GetKilledBy ();
  txNew.vin.reserve (killedPlayers.size ());
  BOOST_FOREACH(const PlayerID &victim, killedPlayers)
    {
      const vchType vchName = vchFromString (victim);
      CTransaction tx;
      if (!GetTxOfNameAtHeight (nameDb, vchName, gameState.nHeight, tx))
        return error ("Game engine killed a non-existing player %s",
                      victim.c_str ());

      if (fDebug)
        printf ("  killed: %s\n", victim.c_str ());

      CTxIn txin(tx.GetHash (), IndexOfNameOutput (tx));

      /* List all killers, if player was simultaneously killed by several
         other players.  If the reason was not KILLED_DESTRUCT, handle
         it also.  If multiple reasons apply, the game tx is constructed
         for the first reason according to the ordering inside of KilledByMap.
         (Which in turn is determined by the enum values for KILLED_*.)  */

      typedef KilledByMap::const_iterator Iter;
      const std::pair<Iter, Iter> iters = killedBy.equal_range (victim);
      if (iters.first == iters.second)
        return error ("No reason for killed player %s", victim.c_str ());
      const KilledByInfo::Reason reason = iters.first->second.reason;

      /* Unless we have destruct, there should be exactly one entry with
         the "first" reason.  There may be multiple entries for different
         reasons, for instance, killed by poison and staying in spawn
         area at the same time.  */
      {
        Iter it = iters.first;
        ++it;
        if (reason != KilledByInfo::KILLED_DESTRUCT && it != iters.second
            && reason == it->second.reason)
          return error ("Multiple same-reason, non-destruct killed-by"
                        " entries for %s", victim.c_str ());
      }

      switch (reason)
        {
        case KilledByInfo::KILLED_DESTRUCT:
          txin.scriptSig << vchName << GAMEOP_KILLED_BY;
          for (Iter it = iters.first; it != iters.second; ++it)
            {
              if (it->second.reason != KilledByInfo::KILLED_DESTRUCT)
                {
                  assert (it != iters.first);
                  break;
                }
              txin.scriptSig << vchFromString (it->second.killer.ToString ());
            }
          break;

        case KilledByInfo::KILLED_SPAWN:
          txin.scriptSig << vchName << GAMEOP_KILLED_BY;
          break;

        case KilledByInfo::KILLED_POISON:
          txin.scriptSig << vchName << GAMEOP_KILLED_POISON;
          break;

        default:
          assert (false);
        }

      txNew.vin.push_back (txin);
    }
  if (!txNew.IsNull ())
    {
      outvgametx.push_back (txNew);
      if (fDebug)
        printf ("Game tx for killed players: %s\n", txNew.GetHashForLog ());
    }

  /* Pay bounties to the players who collected them.  The transaction
     inputs are just "dummy" containing informational messages.  */
  txNew.SetNull ();
  txNew.SetGameTx ();
  txNew.vin.reserve (stepResult.bounties.size ());
  txNew.vout.reserve (stepResult.bounties.size ());

  BOOST_FOREACH(const CollectedBounty& bounty, stepResult.bounties)
    {
      const vchType vchName = vchFromString (bounty.character.player);
      CTransaction tx;
      if (!GetTxOfNameAtHeight (nameDb, vchName, gameState.nHeight, tx))
        return error ("Game engine created bounty for non-existing player");

      CTxOut txout;
      txout.nValue = bounty.loot.nAmount;

      if (!bounty.address.empty ())
        {
          /* Player-provided addresses are validated before accepting them,
             so failing here is ok.  */
          if (!txout.scriptPubKey.SetBitcoinAddress (bounty.address))
            return error ("Failed to set player-provided address for bounty");
        }
      else
        {
          // TODO: Maybe pay to the script of the name-tx without extracting the address first
          // (see source of GetNameAddress - it obtains the script by calling RemoveNameScriptPrefix)
          uint160 addr;
          if (!GetNameAddress (tx, addr))
            return error("Cannot get name address for bounty");
          txout.scriptPubKey.SetBitcoinAddress (addr);
        }

      txNew.vout.push_back (txout);

      CTxIn txin;
      if (bounty.loot.IsRefund ())
        txin.scriptSig
          << vchName << GAMEOP_REFUND
          << bounty.character.index << bounty.loot.GetRefundHeight ();
      else
        txin.scriptSig
          << vchName << GAMEOP_COLLECTED_BOUNTY
          << bounty.character.index
          << bounty.loot.firstBlock
          << bounty.loot.lastBlock
          << bounty.loot.collectedFirstBlock
          << bounty.loot.collectedLastBlock;
      txNew.vin.push_back (txin);
    }
  if (!txNew.IsNull ())
    {
      outvgametx.push_back (txNew);
      if (fDebug)
        printf ("Game tx for bounties: %s\n", txNew.GetHashForLog ());
    }

  return true;
}

bool
IsPlayerDeathInput (const CTxIn& in, vchType& name)
{
  opcodetype opcode;
  CScript::const_iterator pc = in.scriptSig.begin ();

  if (!in.scriptSig.GetOp (pc, opcode, name))
    return error ("could not extract name in game tx input");

  if (!in.scriptSig.GetOp (pc, opcode))
    return error ("could not extract game tx opcode");

  switch (opcode - OP_1 + 1)
    {
    case GAMEOP_KILLED_BY:
    case GAMEOP_KILLED_POISON:
      return true;

    default:
      return false;
    }
}

/* Decode an integer (not could be encoded as OP_x or a bignum)
   from the script.  Returns -1 in case of error.  */
static int
GetScriptUint (const CScript& script, CScript::const_iterator& pc)
{
  opcodetype opcode;
  vchType vch;
  if (!script.GetOp (pc, opcode, vch))
    return -1;

  if (opcode >= OP_1 && opcode <= OP_16)
    return opcode - OP_1 + 1;

  CBigNum bn;
  bn.setvch (vch);

  return bn.getuint ();
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

        case GAMEOP_KILLED_POISON:
            if (fBrief)
                return strRet + " " + _("is killed");
            strRet += " ";
            strRet += _("died from poison");
            break;

        case GAMEOP_COLLECTED_BOUNTY:
          {
            const int index = GetScriptUint (scriptSig, pc);
            if (index > 0)
                strRet += strprintf(".%d", index);
            strRet += " ";
            strRet += _("collected bounty");
            break;
          }

        case GAMEOP_REFUND:
          {
            const int index = GetScriptUint (scriptSig, pc);
            if (index > 0)
                strRet += strprintf(".%d", index);
            strRet += " ";
            strRet += _("refunded on spawn death");
            break;
          }

        default:
            strRet += " ";
            strRet += _("(unknown tx type)");
            break;
    }
    return strRet;
}

void
GameInputToJSON (const CScript& scriptSig, json_spirit::Object& o)
{
  CScript::const_iterator pc = scriptSig.begin ();
  opcodetype opcode;
  vchType vch;
  if (!scriptSig.GetOp (pc, opcode, vch))
    goto error;
  o.push_back (json_spirit::Pair ("player", stringFromVch (vch)));

  if (!scriptSig.GetOp (pc, opcode))
    goto error;

  switch (opcode - OP_1 + 1)
    {
    case GAMEOP_KILLED_BY:
      {
        json_spirit::Array killers;
        while (scriptSig.GetOp (pc, opcode, vch))
          killers.push_back (stringFromVch (vch));

        if (killers.empty ())
          o.push_back (json_spirit::Pair ("op", "spawn_death"));
        else
          {
            o.push_back (json_spirit::Pair ("op", "killed_by"));
            o.push_back (json_spirit::Pair ("killers", killers));
          }

        break;
      }

    case GAMEOP_KILLED_POISON:
      o.push_back (json_spirit::Pair ("op", "poison_death"));
      break;

    case GAMEOP_COLLECTED_BOUNTY:
      o.push_back (json_spirit::Pair ("op", "banking"));
      o.push_back (json_spirit::Pair ("index", GetScriptUint (scriptSig, pc)));
      o.push_back (json_spirit::Pair ("first_block",
                                      GetScriptUint (scriptSig, pc)));
      o.push_back (json_spirit::Pair ("last_block",
                                      GetScriptUint (scriptSig, pc)));
      o.push_back (json_spirit::Pair ("first_collected",
                                      GetScriptUint (scriptSig, pc)));
      o.push_back (json_spirit::Pair ("last_collected",
                                      GetScriptUint (scriptSig, pc)));
      break;

    case GAMEOP_REFUND:
      o.push_back (json_spirit::Pair ("op", "refund"));
      o.push_back (json_spirit::Pair ("index", GetScriptUint (scriptSig, pc)));
      o.push_back (json_spirit::Pair ("height", GetScriptUint (scriptSig, pc)));
      break;

    default:
      goto error;
    }

  return;

error:
  o.push_back (json_spirit::Pair ("error", "could not decode game tx"));
}
