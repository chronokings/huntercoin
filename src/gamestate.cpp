#include "gamestate.h"
#include "gamemap.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>

#include "headers.h"
#include "huntercoin.h"

using namespace Game;

json_spirit::Value ValueFromAmount(int64 amount);

/* Parameters that determine when a poison-disaster will happen.  The
   probability is 1/x at each block between min and max time.  */
static const unsigned PDISASTER_MIN_TIME = 1440;
static const unsigned PDISASTER_MAX_TIME = 12 * 1440;
static const unsigned PDISASTER_PROBABILITY = 10000;

/* Parameters about how long a poisoned player may still live.  */
static const unsigned POISON_MIN_LIFE = 1;
static const unsigned POISON_MAX_LIFE = 50;

/* Parameters for dynamic banks after the life-steal fork.  */
static const unsigned DYNBANKS_NUM_BANKS = 75;
static const unsigned DYNBANKS_MIN_LIFE = 25;
static const unsigned DYNBANKS_MAX_LIFE = 100;

namespace Game
{

inline bool IsOriginalSpawnArea(const Coord &c)
{
    return IsOriginalSpawnArea(c.x, c.y);
}

inline bool IsWalkable(const Coord &c)
{
    return IsWalkable(c.x, c.y);
}

/**
 * Keep a set of walkable tiles.  This is used for random selection of
 * one of them for spawning / dynamic bank purposes.  Note that it is
 * important how they are ordered (according to Coord::operator<) in order
 * to reach consensus on the game state.
 *
 * This is filled in from IsWalkable() whenever it is empty (on startup).  It
 * does not ever change.
 */
static std::vector<Coord> walkableTiles;
// for FORK_TIMESAVE -- 2 more sets of walkable tiles
static std::vector<Coord> walkableTiles_ts_players;
static std::vector<Coord> walkableTiles_ts_banks;

/* Calculate carrying capacity.  This is where it is basically defined.
   It depends on the block height (taking forks changing it into account)
   and possibly properties of the player.  Returns -1 if the capacity
   is unlimited.  */
static int64_t
GetCarryingCapacity (int nHeight, bool isGeneral, bool isCrownHolder)
{
  if (!ForkInEffect (FORK_CARRYINGCAP, nHeight) || isCrownHolder)
    return -1;

  if (ForkInEffect (FORK_LIFESTEAL, nHeight))
    return 100 * COIN;

  if (ForkInEffect (FORK_LESSHEARTS, nHeight))
    return 2000 * COIN;

  return (isGeneral ? 50 : 25) * COIN;
}

/* Return the minimum necessary amount of locked coins.  This replaces the
   old NAME_COIN_AMOUNT constant and makes it more dynamic, so that we can
   change it with hard forks.  */
static int64_t
GetNameCoinAmount (unsigned nHeight)
{
  if (ForkInEffect (FORK_TIMESAVE, nHeight))
    return 100 * COIN;
  if (ForkInEffect (FORK_LESSHEARTS, nHeight))
    return 200 * COIN;
  if (ForkInEffect (FORK_POISON, nHeight))
    return 10 * COIN;
  return COIN;
}

/* Get the destruct radius a hunter has at a certain block height.  This
   may depend on whether or not it is a general.  */
static int
GetDestructRadius (int nHeight, bool isGeneral)
{
  if (ForkInEffect (FORK_LESSHEARTS, nHeight))
    return 1;

  return isGeneral ? 2 : 1;
}

/* Get maximum allowed stay on a bank.  */
static int
MaxStayOnBank (int nHeight)
{
  if (ForkInEffect (FORK_LIFESTEAL, nHeight))
    return 2;

  /* Between those two forks, spawn death was disabled.  */
  if (ForkInEffect (FORK_CARRYINGCAP, nHeight)
        && !ForkInEffect (FORK_LESSHEARTS, nHeight))
    return -1;

  /* Return original value.  */
  return 30;
}

/* Check whether or not a heart should be dropped at the current height.  */
static bool
DropHeart (int nHeight)
{
  if (ForkInEffect (FORK_LIFESTEAL, nHeight))
    return false;

  const int heartEvery = (ForkInEffect (FORK_LESSHEARTS, nHeight) ? 500 : 10);
  return nHeight % heartEvery == 0;
}

/* Ensure that walkableTiles is filled.  */
static void
FillWalkableTiles ()
{
    // for FORK_TIMESAVE -- less possible player and bank spawn tiles
    if (walkableTiles_ts_players.empty ())
    {
        for (int x = 0; x < MAP_WIDTH; ++x)
          for (int y = 0; y < MAP_HEIGHT; ++y)
            if (IsWalkable (x, y))
            {
                if ( ! (SpawnMap[y][x] & SPAWNMAPFLAG_PLAYER) ) // note: player spawn tiles and bank spawn tiles are separated
                  continue;

              walkableTiles_ts_players.push_back (Coord (x, y));
            }

        /* Do not forget to sort in the order defined by operator<!  */
        std::sort (walkableTiles_ts_players.begin (), walkableTiles_ts_players.end ());
        assert (!walkableTiles_ts_players.empty ());
    }
    if (walkableTiles_ts_banks.empty ())
    {
        for (int x = 0; x < MAP_WIDTH; ++x)
          for (int y = 0; y < MAP_HEIGHT; ++y)
            if (IsWalkable (x, y))
            {
                if ( ! (SpawnMap[y][x] & SPAWNMAPFLAG_BANK) )
                  continue;

              walkableTiles_ts_banks.push_back (Coord (x, y));
            }

        /* Do not forget to sort in the order defined by operator<!  */
        std::sort (walkableTiles_ts_banks.begin (), walkableTiles_ts_banks.end ());
        assert (!walkableTiles_ts_banks.empty ());
    }

    if (!walkableTiles.empty ())
      return;

    for (int x = 0; x < MAP_WIDTH; ++x)
      for (int y = 0; y < MAP_HEIGHT; ++y)
        if (IsWalkable (x, y))
        {
          walkableTiles.push_back (Coord (x, y));
        }

    /* Do not forget to sort in the order defined by operator<!  */
    std::sort (walkableTiles.begin (), walkableTiles.end ());

    assert (!walkableTiles.empty ());
}

} // namespace Game


// Random generator seeded with block hash
class Game::RandomGenerator
{
public:
    RandomGenerator(uint256 hashBlock)
        : state0(SerializeHash(hashBlock, SER_GETHASH, 0))
    {
        state = state0;
    }

    int GetIntRnd(int modulo)
    {
        // Advance generator state, if most bits of the current state were used
        if (state < MIN_STATE)
        {
            state0.setuint256(SerializeHash(state0, SER_GETHASH, 0));
            state = state0;
        }
        return state.DivideGetRemainder(modulo).getint();
    }

    /* Get an integer number in [a, b].  */
    int GetIntRnd (int a, int b)
    {
      assert (a <= b);
      const int mod = (b - a + 1);
      const int res = GetIntRnd (mod) + a;
      assert (res >= a && res <= b);
      return res;
    }

private:
    CBigNum state, state0;
    static const CBigNum MIN_STATE;
};

const CBigNum RandomGenerator::MIN_STATE = CBigNum().SetCompact(0x097FFFFFu);

/* ************************************************************************** */
/* KilledByInfo.  */

bool
KilledByInfo::HasDeathTax () const
{
  return reason != KILLED_SPAWN;
}

bool
KilledByInfo::DropCoins (unsigned nHeight, const PlayerState& victim) const
{
  if (!ForkInEffect (FORK_LESSHEARTS, nHeight))
    return true;

  /* If the player is poisoned, no dropping of coins.  Note that we have
     to allow ==0 here (despite what gamestate.h says), since that is the
     case precisely when we are killing the player right now due to poison.  */
  if (victim.remainingLife >= 0)
    return false;

  assert (victim.remainingLife == -1);
  return true;
}

bool
KilledByInfo::CanRefund (unsigned nHeight, const PlayerState& victim) const
{
  if (!ForkInEffect (FORK_LESSHEARTS, nHeight))
    return false;

  switch (reason)
    {
    case KILLED_SPAWN:

      /* Before life-steal fork, poisoned players were not refunded.  */
      if (!ForkInEffect (FORK_LIFESTEAL, nHeight) && victim.remainingLife >= 0)
        return false;

      return true;

    case KILLED_POISON:
      return ForkInEffect (FORK_LIFESTEAL, nHeight);

    default:
      return false;
    }

  assert (false);
}

/* ************************************************************************** */
/* Move.  */

static bool
ExtractField (json_spirit::Object& obj, const std::string field,
              json_spirit::Value& v)
{
    for (std::vector<json_spirit::Pair>::iterator i = obj.begin(); i != obj.end(); ++i)
    {
        if (i->name_ == field)
        {
            v = i->value_;
            obj.erase(i);
            return true;
        }
    }
    return false;
}

bool Move::IsValid(const GameState &state) const
{
  PlayerStateMap::const_iterator mi = state.players.find (player);

  /* Before the life-steal fork, check that the move does not contain
     destruct and waypoints together.  This needs the height for its
     decision, thus it is not done in Parse (as before).  */
  /* FIXME: Remove check once the fork is passed.  */
  if (!ForkInEffect (FORK_LIFESTEAL, state.nHeight + 1))
    for (std::map<int, WaypointVector>::const_iterator i = waypoints.begin ();
         i != waypoints.end (); ++i)
      if (destruct.count (i->first) > 0)
        return error ("%s: destruct and waypoints together", __func__);

  int64_t oldLocked;
  if (mi == state.players.end ())
    {
      if (!IsSpawn ())
        return false;
      oldLocked = 0;
    }
  else
    {
      if (IsSpawn ())
        return false;
      oldLocked = mi->second.lockedCoins;
    }

  assert (oldLocked >= 0 && newLocked >= 0);
  const int64_t gameFee = newLocked - oldLocked;
  const int64_t required = MinimumGameFee (state.nHeight + 1);
  assert (required >= 0);
  if (gameFee < required)
    return error ("%s: too little game fee attached, got %lld, required %lld",
                  __func__, gameFee, required);

  return true;
}

bool ParseWaypoints(json_spirit::Object &obj, std::vector<Coord> &result, bool &bWaypoints)
{
    using namespace json_spirit;

    bWaypoints = false;
    result.clear();
    Value v;
    if (!ExtractField(obj, "wp", v))
        return true;
    if (v.type() != array_type)
        return false;
    Array arr = v.get_array();
    if (arr.size() % 2)
        return false;
    int n = arr.size() / 2;
    if (n > MAX_WAYPOINTS)
        return false;
    result.resize(n);
    for (int i = 0; i < n; i++)
    {
        if (arr[2 * i].type() != int_type || arr[2 * i + 1].type() != int_type)
            return false;
        int x = arr[2 * i].get_int();
        int y = arr[2 * i + 1].get_int();
        if (!IsInsideMap(x, y))
            return false;
        // Waypoints are reversed for easier deletion of current waypoint from the end of the vector
        result[n - 1 - i] = Coord(x, y);
        if (i && result[n - 1 - i] == result[n - i])
            return false; // Forbid duplicates        
    }
    bWaypoints = true;
    return true;
}

bool ParseDestruct(json_spirit::Object &obj, bool &result)
{
    using namespace json_spirit;

    result = false;
    Value v;
    if (!ExtractField(obj, "destruct", v))
        return true;
    if (v.type() != bool_type)
        return false;
    result = v.get_bool();
    return true;
}

bool Move::Parse(const PlayerID &player, const std::string &json)
{
    using namespace json_spirit;

    if (!IsValidPlayerName(player))
        return false;
        
    Value v;
    if (!read_string(json, v) || v.type() != obj_type)
        return false;
    Object obj = v.get_obj();

    if (ExtractField(obj, "msg", v))
    {
        if (v.type() != str_type)
            return false;
        message = v.get_str();
    }
    if (ExtractField(obj, "address", v))
    {
        if (v.type() != str_type)
            return false;
        const std::string &addr = v.get_str();
        if (!addr.empty() && !IsValidBitcoinAddress(addr))
            return false;
        address = addr;
    }
    if (ExtractField(obj, "addressLock", v))
    {
        if (v.type() != str_type)
            return false;
        const std::string &addr = v.get_str();
        if (!addr.empty() && !IsValidBitcoinAddress(addr))
            return false;
        addressLock = addr;
    }

    if (ExtractField(obj, "color", v))
    {
        if (v.type() != int_type)
            return false;
        color = v.get_int();
        if (color >= NUM_TEAM_COLORS)
            return false;
        if (!obj.empty()) // Extra fields are not allowed in JSON string
            return false;
        this->player = player;
        return true;
    }

    std::set<int> character_indices;
    for (std::vector<json_spirit::Pair>::iterator it = obj.begin(); it != obj.end(); ++it)
    {
        int i = atoi(it->name_);
        if (i < 0 || strprintf("%d", i) != it->name_)
            return false;               // Number formatting must be strict
        if (character_indices.count(i))
            return false;               // Cannot contain duplicate character indices
        character_indices.insert(i);
        v = it->value_;
        if (v.type() != obj_type)
            return false;
        Object subobj = v.get_obj();
        bool bWaypoints = false;
        std::vector<Coord> wp;
        if (!ParseWaypoints(subobj, wp, bWaypoints))
            return false;
        bool bDestruct;
        if (!ParseDestruct(subobj, bDestruct))
            return false;

        if (bDestruct)
            destruct.insert(i);
        if (bWaypoints)
            waypoints.insert(std::make_pair(i, wp));

        if (!subobj.empty())      // Extra fields are not allowed in JSON string
            return false;
    }
        
    this->player = player;
    return true;
}

void Move::ApplyCommon(GameState &state) const
{
    std::map<PlayerID, PlayerState>::iterator mi = state.players.find(player);

    if (mi == state.players.end())
    {
        if (message)
        {
            PlayerState &pl = state.dead_players_chat[player];
            pl.message = *message;
            pl.message_block = state.nHeight;
        }
        return;
    }

    PlayerState &pl = mi->second;
    if (message)
    {
        pl.message = *message;
        pl.message_block = state.nHeight;
    }
    if (address)
        pl.address = *address;
    if (addressLock)
        pl.addressLock = *addressLock;
}

std::string Move::AddressOperationPermission(const GameState &state) const
{
    if (!address && !addressLock)
        return std::string();      // No address operation requested - allow

    std::map<PlayerID, PlayerState>::const_iterator mi = state.players.find(player);
    if (mi == state.players.end())
        return std::string();      // Spawn move - allow any address operation

    return mi->second.addressLock;
}

void
Move::ApplySpawn (GameState &state, RandomGenerator &rnd) const
{
  assert (state.players.count (player) == 0);

  PlayerState pl;
  assert (pl.next_character_index == 0);
  pl.color = color;

  /* This is a fresh player and name.  Set its value to the height's
     name coin amount and put the remainder in the game fee.  This prevents
     people from "overpaying" on purpose in order to get beefed-up players.
     This rule, however, is only active after the life-steal fork.  Before
     that, overpaying did, indeed, allow to set the hunter value
     arbitrarily high.  */
  if (ForkInEffect (FORK_LIFESTEAL, state.nHeight))
    {
      const int64_t coinAmount = GetNameCoinAmount (state.nHeight);
      assert (pl.lockedCoins == 0 && pl.value == -1);
      assert (newLocked >= coinAmount);
      pl.value = coinAmount;
      pl.lockedCoins = newLocked;
      state.gameFund += newLocked - coinAmount;
    }
  else
    {
      pl.value = newLocked;
      pl.lockedCoins = newLocked;
    }

  const unsigned limit = state.GetNumInitialCharacters ();
  for (unsigned i = 0; i < limit; i++)
    pl.SpawnCharacter (state.nHeight, rnd);

  state.players.insert (std::make_pair (player, pl));
}

void Move::ApplyWaypoints(GameState &state) const
{
    std::map<PlayerID, PlayerState>::iterator pl;
    pl = state.players.find (player);
    if (pl == state.players.end ())
      return;

    BOOST_FOREACH(const PAIRTYPE(int, std::vector<Coord>) &p, waypoints)
    {
        std::map<int, CharacterState>::iterator mi;
        mi = pl->second.characters.find(p.first);
        if (mi == pl->second.characters.end())
            continue;
        CharacterState &ch = mi->second;
        const std::vector<Coord> &wp = p.second;

        if (ch.waypoints.empty() || wp.empty() || ch.waypoints.back() != wp.back())
            ch.from = ch.coord;
        ch.waypoints = wp;
    }
}

int64_t
Move::MinimumGameFee (unsigned nHeight) const
{
  if (IsSpawn ())
    {
      const int64_t coinAmount = GetNameCoinAmount (nHeight);

      // fee for new hunter is 1 HUC
      if (ForkInEffect (FORK_TIMESAVE, nHeight))
        return coinAmount + COIN;

      if (ForkInEffect (FORK_LIFESTEAL, nHeight))
        return coinAmount + 5 * COIN;

      return coinAmount;
    }

  // destruct fee is 1 HUC
  if (ForkInEffect (FORK_TIMESAVE, nHeight))
    return COIN * destruct.size ();

  if (ForkInEffect (FORK_LIFESTEAL, nHeight))
    return 20 * COIN * destruct.size ();

  return 0;
}

std::string CharacterID::ToString() const
{
    if (!index)
        return player;
    return player + strprintf(".%d", int(index));
}

/* ************************************************************************** */
/* AttackableCharacter and CharactersOnTiles.  */

void
AttackableCharacter::AttackBy (const CharacterID& attackChid,
                               const PlayerState& pl)
{
  /* Do not attack same colour.  */
  if (color == pl.color)
    return;

  assert (attackers.count (attackChid) == 0);
  attackers.insert (attackChid);
}

void
AttackableCharacter::AttackSelf (const GameState& state)
{
  if (!ForkInEffect (FORK_LIFESTEAL, state.nHeight))
    {
      assert (attackers.count (chid) == 0);
      attackers.insert (chid);
    }
}

void
CharactersOnTiles::EnsureIsBuilt (const GameState& state)
{
  if (built)
    return;
  assert (tiles.empty ());

  BOOST_FOREACH (const PAIRTYPE(PlayerID, PlayerState)& p, state.players)
    BOOST_FOREACH (const PAIRTYPE(int, CharacterState)& pc, p.second.characters)
      {
        // newly spawned hunters not attackable
        if (ForkInEffect (FORK_TIMESAVE, state.nHeight))
          if (CharacterIsProtected(pc.second.stay_in_spawn_area))
          {
            // printf("protection: character at x=%d y=%d is protected\n", pc.second.coord.x, pc.second.coord.y);
            continue;
          }

        AttackableCharacter a;
        a.chid = CharacterID (p.first, pc.first);
        a.color = p.second.color;
        a.drawnLife = 0;

        tiles.insert (std::make_pair (pc.second.coord, a));
      }
  built = true;
}

void
CharactersOnTiles::ApplyAttacks (const GameState& state,
                                 const std::vector<Move>& moves)
{
  BOOST_FOREACH(const Move& m, moves)
    {
      if (m.destruct.empty ())
        continue;

      const PlayerStateMap::const_iterator miPl = state.players.find (m.player);
      assert (miPl != state.players.end ());
      const PlayerState& pl = miPl->second;
      BOOST_FOREACH(int i, m.destruct)
        {
          const std::map<int, CharacterState>::const_iterator miCh
            = pl.characters.find (i);
          if (miCh == pl.characters.end ())
            continue;
          const CharacterID chid(m.player, i);
          if (state.crownHolder == chid)
            continue;

          const CharacterState& ch = miCh->second;
          // hunters in spectator mode can't attack
          if ((ForkInEffect (FORK_TIMESAVE, state.nHeight)) &&
              (CharacterInSpectatorMode(ch.stay_in_spawn_area)))
          {
              // printf("protection: character at x=%d y=%d can't attack\n", ch.coord.x, ch.coord.y);
              continue;
          }

          EnsureIsBuilt (state);

          const int radius = GetDestructRadius (state.nHeight, i == 0);

          const Coord& c = ch.coord;
          for (int y = c.y - radius; y <= c.y + radius; y++)
            for (int x = c.x - radius; x <= c.x + radius; x++)
              {
                const std::pair<Map::iterator, Map::iterator> iters
                  = tiles.equal_range (Coord (x, y));
                for (Map::iterator it = iters.first; it != iters.second; ++it)
                  {
                    AttackableCharacter& a = it->second;
                    if (a.chid == chid)
                      a.AttackSelf (state);
                    else
                      a.AttackBy (chid, pl);
                  }
              }
        }
    }
}

void
CharactersOnTiles::DrawLife (GameState& state, StepResult& result)
{
  if (!built)
    return;

  /* Find damage amount if we have life steal in effect.  */
  const bool lifeSteal = ForkInEffect (FORK_LIFESTEAL, state.nHeight);
  const int64_t damage = GetNameCoinAmount (state.nHeight);

  BOOST_FOREACH (PAIRTYPE(const Coord, AttackableCharacter)& tile, tiles)
    {
      AttackableCharacter& a = tile.second;
      if (a.attackers.empty ())
        continue;
      assert (a.drawnLife == 0);

      /* Find the player state of the attacked character.  */
      PlayerStateMap::iterator vit = state.players.find (a.chid.player);
      assert (vit != state.players.end ());
      PlayerState& victim = vit->second;

      /* In case of life steal, actually draw life.  The coins are not yet
         added to the attacker, but instead their total amount is saved
         for future redistribution.  */
      if (lifeSteal)
        {
          assert (a.chid.index == 0);

          int64_t fullDamage = damage * a.attackers.size ();
          if (fullDamage > victim.value)
            fullDamage = victim.value;

          victim.value -= fullDamage;
          a.drawnLife += fullDamage;

          /* If less than the minimum amount remains, als that is drawn
             and later added to the game fund.  */
          assert (victim.value >= 0);
          if (victim.value < damage)
            {
              a.drawnLife += victim.value;
              victim.value = 0;
            }
        }
      assert (victim.value >= 0);
      assert (a.drawnLife >= 0);

      /* If we have life steal and there is remaining health, let
         the player survive.  Note that it must have at least the minimum
         value.  If "split coins" are remaining, we still kill it.  */
      if (lifeSteal && victim.value != 0)
        {
          assert (victim.value >= damage);
          continue;
        }

      if (a.chid.index == 0)
        for (std::set<CharacterID>::const_iterator at = a.attackers.begin ();
             at != a.attackers.end (); ++at)
          {
            const KilledByInfo killer(*at);
            result.KillPlayer (a.chid.player, killer);
          }

      if (victim.characters.count (a.chid.index) > 0)
        {
          assert (a.attackers.begin () != a.attackers.end ());
          const KilledByInfo& info(*a.attackers.begin ());
          state.HandleKilledLoot (a.chid.player, a.chid.index, info, result);
          victim.characters.erase (a.chid.index);
        }
    }
}

void
CharactersOnTiles::DefendMutualAttacks (const GameState& state)
{
  if (!built)
    return;

  /* Build up a set of all (directed) attacks happening.  The pairs
     mean an attack (from, to).  This is then later used to determine
     mutual attacks, and remove them accordingly.

     One can probably do this in a more efficient way, but for now this
     is how it is implemented.  */

  typedef std::pair<CharacterID, CharacterID> Attack;
  std::set<Attack> attacks;
  BOOST_FOREACH (const PAIRTYPE(const Coord, AttackableCharacter)& tile, tiles)
    {
      const AttackableCharacter& a = tile.second;
      for (std::set<CharacterID>::const_iterator mi = a.attackers.begin ();
           mi != a.attackers.end (); ++mi)
        attacks.insert (std::make_pair (*mi, a.chid));
    }

  BOOST_FOREACH (PAIRTYPE(const Coord, AttackableCharacter)& tile, tiles)
    {
      AttackableCharacter& a = tile.second;

      std::set<CharacterID> notDefended;
      for (std::set<CharacterID>::const_iterator mi = a.attackers.begin ();
           mi != a.attackers.end (); ++mi)
        {
          const Attack counterAttack(a.chid, *mi);
          if (attacks.count (counterAttack) == 0)
            notDefended.insert (*mi);
        }

      a.attackers.swap (notDefended);
    }
}

void
CharactersOnTiles::DistributeDrawnLife (RandomGenerator& rnd,
                                        GameState& state) const
{
  if (!built)
    return;

  const int64_t damage = GetNameCoinAmount (state.nHeight);

  /* Life is already drawn.  It remains to distribute the drawn balances
     from each attacked character back to its attackers.  For this,
     we first find the still alive players and assemble them in a map.  */
  std::map<CharacterID, PlayerState*> alivePlayers;
  BOOST_FOREACH (const PAIRTYPE(const Coord, AttackableCharacter)& tile, tiles)
    {
      const AttackableCharacter& a = tile.second;
      assert (alivePlayers.count (a.chid) == 0);

      /* Only non-hearted characters should be around if this is called,
         since this means that life-steal is in effect.  */
      assert (a.chid.index == 0);

      const PlayerStateMap::iterator pit = state.players.find (a.chid.player);
      if (pit != state.players.end ())
        {
          PlayerState& pl = pit->second;
          assert (pl.characters.count (a.chid.index) > 0);
          alivePlayers.insert (std::make_pair (a.chid, &pl));
        }
    }

  /* Now go over all attacks and distribute life to the attackers.  */
  BOOST_FOREACH (const PAIRTYPE(const Coord, AttackableCharacter)& tile, tiles)
    {
      const AttackableCharacter& a = tile.second;
      if (a.attackers.empty () || a.drawnLife == 0)
        continue;

      /* Find attackers that are still alive.  We will randomly distribute
         coins to them later on.  */
      std::vector<CharacterID> alive;
      for (std::set<CharacterID>::const_iterator mi = a.attackers.begin ();
           mi != a.attackers.end (); ++mi)
        if (alivePlayers.count (*mi) > 0)
          alive.push_back (*mi);

      /* Distribute the drawn life randomly until either all is spent
         or all alive attackers have gotten some.  */
      int64_t toSpend = a.drawnLife;
      while (!alive.empty () && toSpend >= damage)
        {
          const unsigned ind = rnd.GetIntRnd (alive.size ());
          const std::map<CharacterID, PlayerState*>::iterator plIt
            = alivePlayers.find (alive[ind]);
          assert (plIt != alivePlayers.end ());

          toSpend -= damage;
          plIt->second->value += damage;

          /* Do not use a silly trick like swapping in the last element.
             We want to keep the array ordered at all times.  The order is
             important with respect to consensus, and this makes the consensus
             protocol "clearer" to describe.  */
          alive.erase (alive.begin () + ind);
        }

      /* Distribute the remaining value to the game fund.  */
      assert (toSpend >= 0);
      state.gameFund += toSpend;
    }
}

/* ************************************************************************** */
/* CharacterState and PlayerState.  */

void
CharacterState::Spawn (unsigned nHeight, int color, RandomGenerator &rnd)
{
  // less possible player spawn tiles
  if (ForkInEffect (FORK_TIMESAVE, nHeight))
  {
      FillWalkableTiles ();

      const int pos = rnd.GetIntRnd (walkableTiles_ts_players.size ());
      coord = walkableTiles_ts_players[pos];

      dir = rnd.GetIntRnd (1, 8);
      if (dir >= 5)
        ++dir;
      assert (dir >= 1 && dir <= 9 && dir != 5);
  }
  /* Pick a random walkable spawn location after the life-steal fork.  */
  else if (ForkInEffect (FORK_LIFESTEAL, nHeight))
    {
      FillWalkableTiles ();

      const int pos = rnd.GetIntRnd (walkableTiles.size ());
      coord = walkableTiles[pos];

      dir = rnd.GetIntRnd (1, 8);
      if (dir >= 5)
        ++dir;
      assert (dir >= 1 && dir <= 9 && dir != 5);
    }

  /* Use old logic with fixed spawns in the corners before the fork.  */
  else
    {
      const int pos = rnd.GetIntRnd(2 * SPAWN_AREA_LENGTH - 1);
      const int x = pos < SPAWN_AREA_LENGTH ? pos : 0;
      const int y = pos < SPAWN_AREA_LENGTH ? 0 : pos - SPAWN_AREA_LENGTH;
      switch (color)
        {
        case 0: // Yellow (top-left)
          coord = Coord(x, y);
          break;
        case 1: // Red (top-right)
          coord = Coord(MAP_WIDTH - 1 - x, y);
          break;
        case 2: // Green (bottom-right)
          coord = Coord(MAP_WIDTH - 1 - x, MAP_HEIGHT - 1 - y);
          break;
        case 3: // Blue (bottom-left)
          coord = Coord(x, MAP_HEIGHT - 1 - y);
          break;
        default:
          throw std::runtime_error("CharacterState::Spawn: incorrect color");
        }

      // Set look-direction for the sprite
      if (coord.x == 0)
        {
          if (coord.y == 0)
            dir = 3;
          else if (coord.y == MAP_HEIGHT - 1)
            dir = 9;
          else
            dir = 6;
        }
      else if (coord.x == MAP_WIDTH - 1)
        {
          if (coord.y == 0)
            dir = 1;
          else if (coord.y == MAP_HEIGHT - 1)
            dir = 7;
          else
            dir = 4;
        }
      else if (coord.y == 0)
        dir = 2;
      else if (coord.y == MAP_HEIGHT - 1)
        dir = 8;
    }

  StopMoving();
}

// Returns direction from c1 to c2 as a number from 1 to 9 (as on the numeric keypad)
static unsigned char
GetDirection (const Coord& c1, const Coord& c2)
{
    int dx = c2.x - c1.x;
    int dy = c2.y - c1.y;
    if (dx < -1)
        dx = -1;
    else if (dx > 1)
        dx = 1;
    if (dy < -1)
        dy = -1;
    else if (dy > 1)
        dy = 1;

    return (1 - dy) * 3 + dx + 2;
}

// Simple straight-line motion
void CharacterState::MoveTowardsWaypoint()
{
    if (waypoints.empty())
    {
        from = coord;
        return;
    }
    if (coord == waypoints.back())
    {
        from = coord;
        do
        {
            waypoints.pop_back();
            if (waypoints.empty())
                return;
        } while (coord == waypoints.back());
    }

    struct Helper
    {
        static int CoordStep(int x, int target)
        {
            if (x < target)
                return x + 1;
            else if (x > target)
                return x - 1;
            else
                return x;
        }

        // Compute new 'v' coordinate using line slope information applied to the 'u' coordinate
        // 'u' is reference coordinate (largest among dx, dy), 'v' is the coordinate to be updated
        static int CoordUpd(int u, int v, int du, int dv, int from_u, int from_v)
        {
            if (dv != 0)
            {
                int tmp = (u - from_u) * dv;
                int res = (abs(tmp) + abs(du) / 2) / du;
                if (tmp < 0)
                    res = -res;
                return res + from_v;
            }
            else
                return v;
        }
    };

    Coord new_c;
    Coord target = waypoints.back();
    
    int dx = target.x - from.x;
    int dy = target.y - from.y;
    
    if (abs(dx) > abs(dy))
    {
        new_c.x = Helper::CoordStep(coord.x, target.x);
        new_c.y = Helper::CoordUpd(new_c.x, coord.y, dx, dy, from.x, from.y);
    }
    else
    {
        new_c.y = Helper::CoordStep(coord.y, target.y);
        new_c.x = Helper::CoordUpd(new_c.y, coord.x, dy, dx, from.y, from.x);
    }

    if (!IsWalkable(new_c))
        StopMoving();
    else
    {
        unsigned char new_dir = GetDirection(coord, new_c);
        // If not moved (new_dir == 5), retain old direction
        if (new_dir != 5)
            dir = new_dir;
        coord = new_c;

        if (coord == target)
        {
            from = coord;
            do
            {
                waypoints.pop_back();
            } while (!waypoints.empty() && coord == waypoints.back());
        }
    }
}

std::vector<Coord> CharacterState::DumpPath(const std::vector<Coord> *alternative_waypoints /* = NULL */) const
{
    std::vector<Coord> ret;
    CharacterState tmp = *this;

    if (alternative_waypoints)
    {
        tmp.StopMoving();
        tmp.waypoints = *alternative_waypoints;
    }

    if (!tmp.waypoints.empty())
    {
        do
        {
            ret.push_back(tmp.coord);
            tmp.MoveTowardsWaypoint();
        } while (!tmp.waypoints.empty());
        if (ret.empty() || ret.back() != tmp.coord)
            ret.push_back(tmp.coord);
    }
    return ret;
}

/**
 * Calculate total length (in the same L-infinity sense that gives the
 * actual movement time) of the outstanding path.
 * @param altWP Optionally provide alternative waypoints (for queued moves).
 * @return Time necessary to finish current path in blocks.
 */
unsigned
CharacterState::TimeToDestination (const WaypointVector* altWP) const
{
  bool reverse = false;
  if (!altWP)
    {
      altWP = &waypoints;
      reverse = true;
    }

  /* In order to handle both reverse and non-reverse correctly, calculate
     first the length of the path alone and only later take the initial
     piece from coord on into account.  */

  if (altWP->empty ())
    return 0;

  unsigned res = 0;
  WaypointVector::const_iterator i = altWP->begin ();
  Coord last = *i;
  for (++i; i != altWP->end (); ++i)
    {
      res += distLInf (last, *i);
      last = *i;
    }

  if (reverse)
    res += distLInf (coord, altWP->back ());
  else
    res += distLInf (coord, altWP->front ());

  return res;
}

int64_t
CharacterState::CollectLoot (LootInfo newLoot, int nHeight, int64_t carryCap)
{
  const int64_t totalBefore = loot.nAmount + newLoot.nAmount;

  int64_t freeCap = carryCap - loot.nAmount;
  if (freeCap < 0)
    {
      /* This means that the character is carrying more than allowed
         (or carryCap == -1, which is handled later anyway).  This
         may happen during transition periods, handle it gracefully.  */
      freeCap = 0;
    }

  int64_t remaining;
  if (carryCap == -1 || newLoot.nAmount <= freeCap)
    remaining = 0;
  else
    remaining = newLoot.nAmount - freeCap;

  if (remaining > 0)
    newLoot.nAmount -= remaining;
  loot.Collect (newLoot, nHeight);

  assert (remaining >= 0 && newLoot.nAmount >= 0);
  assert (totalBefore == loot.nAmount + remaining);
  assert (carryCap == -1 || newLoot.nAmount <= freeCap);
  assert (newLoot.nAmount == 0 || carryCap == -1 || loot.nAmount <= carryCap);

  return remaining;
}

void
PlayerState::SpawnCharacter (unsigned nHeight, RandomGenerator &rnd)
{
  characters[next_character_index++].Spawn (nHeight, color, rnd);
}

json_spirit::Value PlayerState::ToJsonValue(int crown_index, bool dead /* = false*/) const
{
    using namespace json_spirit;

    Object obj;
    obj.push_back(Pair("color", (int)color));
    obj.push_back(Pair("value", ValueFromAmount(value)));

    /* If the character is poisoned, write that out.  Otherwise just
       leave the field off.  */
    if (remainingLife > 0)
      obj.push_back (Pair("poison", remainingLife));
    else
      assert (remainingLife == -1);

    if (!message.empty())
    {
        obj.push_back(Pair("msg", message));
        obj.push_back(Pair("msg_block", message_block));
    }

    if (!dead)
    {
        if (!address.empty())
            obj.push_back(Pair("address", address));
        if (!addressLock.empty())
            obj.push_back(Pair("addressLock", address));
    }
    else
    {
        // Note: not all dead players are listed - only those who sent chat messages in their last move
        assert(characters.empty());
        obj.push_back(Pair("dead", 1));
    }

    BOOST_FOREACH(const PAIRTYPE(int, CharacterState) &pc, characters)
    {
        int i = pc.first;
        const CharacterState &ch = pc.second;
        obj.push_back(Pair(strprintf("%d", i), ch.ToJsonValue(i == crown_index)));
    }

    return obj;
}

json_spirit::Value CharacterState::ToJsonValue(bool has_crown) const
{
    using namespace json_spirit;

    Object obj;
    obj.push_back(Pair("x", coord.x));
    obj.push_back(Pair("y", coord.y));
    if (!waypoints.empty())
    {
        obj.push_back(Pair("fromX", from.x));
        obj.push_back(Pair("fromY", from.y));
        Array arr;
        for (int i = waypoints.size() - 1; i >= 0; i--)
        {
            arr.push_back(Value(waypoints[i].x));
            arr.push_back(Value(waypoints[i].y));
        }
        obj.push_back(Pair("wp", arr));
    }
    obj.push_back(Pair("dir", (int)dir));
    obj.push_back(Pair("stay_in_spawn_area", stay_in_spawn_area));
    obj.push_back(Pair("loot", ValueFromAmount(loot.nAmount)));
    if (has_crown)
        obj.push_back(Pair("has_crown", true));

    return obj;
}

/* ************************************************************************** */
/* GameState.  */

static void
SetOriginalBanks (std::map<Coord, unsigned>& banks)
{
  assert (banks.empty ());
  for (int d = 0; d < SPAWN_AREA_LENGTH; ++d)
    {
      banks.insert (std::make_pair (Coord (0, d), 0));
      banks.insert (std::make_pair (Coord (d, 0), 0));
      banks.insert (std::make_pair (Coord (MAP_WIDTH - 1, d), 0));
      banks.insert (std::make_pair (Coord (d, MAP_HEIGHT - 1), 0));
      banks.insert (std::make_pair (Coord (0, MAP_HEIGHT - d - 1), 0));
      banks.insert (std::make_pair (Coord (MAP_WIDTH - d - 1, 0), 0));
      banks.insert (std::make_pair (Coord (MAP_WIDTH - 1,
                                           MAP_HEIGHT - d - 1), 0));
      banks.insert (std::make_pair (Coord (MAP_WIDTH - d - 1,
                                           MAP_HEIGHT - 1), 0));
    }

  assert (banks.size () == 4 * (2 * SPAWN_AREA_LENGTH - 1));
  BOOST_FOREACH (const PAIRTYPE(Coord, unsigned)& b, banks)
    {
      assert (IsOriginalSpawnArea (b.first));
      assert (b.second == 0);
    }
}

GameState::GameState()
{
    crownPos.x = CROWN_START_X;
    crownPos.y = CROWN_START_Y;
    gameFund = 0;
    nHeight = -1;
    nDisasterHeight = -1;
    hashBlock = 0;
    SetOriginalBanks (banks);
}

void
GameState::UpdateVersion(int oldVersion)
{
  /* Last version change is beyond the last version where the game db
     is fully reconstructed.  */
  assert (oldVersion >= 1001100);

  /* If necessary, initialise the banks array to the original spawn area.
     Make sure that we are not yet at the fork height!  Otherwise this
     is completely wrong.  */
  if (oldVersion < 1030000)
    {
      if (ForkInEffect (FORK_LIFESTEAL, nHeight))
        {
          error ("game DB version upgrade while the life-steal fork is"
                 " already active");
          assert (false);
        }

      SetOriginalBanks (banks);
    }
}

json_spirit::Value GameState::ToJsonValue() const
{
    using namespace json_spirit;

    Object obj;

    Object subobj;
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, players)
    {
        int crown_index = p.first == crownHolder.player ? crownHolder.index : -1;
        subobj.push_back(Pair(p.first, p.second.ToJsonValue(crown_index)));
    }

    // Save chat messages of dead players
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, dead_players_chat)
        subobj.push_back(Pair(p.first, p.second.ToJsonValue(-1, true)));

    obj.push_back(Pair("players", subobj));

    Array arr;
    BOOST_FOREACH(const PAIRTYPE(Coord, LootInfo) &p, loot)
    {
        subobj.clear();
        subobj.push_back(Pair("x", p.first.x));
        subobj.push_back(Pair("y", p.first.y));
        subobj.push_back(Pair("amount", ValueFromAmount(p.second.nAmount)));
        Array blk_rng;
        blk_rng.push_back(p.second.firstBlock);
        blk_rng.push_back(p.second.lastBlock);
        subobj.push_back(Pair("blockRange", blk_rng));
        arr.push_back(subobj);
    }
    obj.push_back(Pair("loot", arr));

    arr.clear ();
    BOOST_FOREACH (const Coord& c, hearts)
      {
        subobj.clear ();
        subobj.push_back (Pair ("x", c.x));
        subobj.push_back (Pair ("y", c.y));
        arr.push_back (subobj);
      }
    obj.push_back (Pair ("hearts", arr));

    arr.clear ();
    BOOST_FOREACH (const PAIRTYPE(Coord, unsigned)& b, banks)
      {
        subobj.clear ();
        subobj.push_back (Pair ("x", b.first.x));
        subobj.push_back (Pair ("y", b.first.y));
        subobj.push_back (Pair ("life", static_cast<int> (b.second)));
        arr.push_back (subobj);
      }
    obj.push_back (Pair ("banks", arr));

    subobj.clear();
    subobj.push_back(Pair("x", crownPos.x));
    subobj.push_back(Pair("y", crownPos.y));
    if (!crownHolder.player.empty())
    {
        subobj.push_back(Pair("holderName", crownHolder.player));
        subobj.push_back(Pair("holderIndex", crownHolder.index));
    }
    obj.push_back(Pair("crown", subobj));

    obj.push_back (Pair("gameFund", ValueFromAmount (gameFund)));
    obj.push_back (Pair("height", nHeight));
    obj.push_back (Pair("disasterHeight", nDisasterHeight));
    obj.push_back (Pair("hashBlock", hashBlock.ToString().c_str()));

    return obj;
}

void GameState::AddLoot(Coord coord, int64_t nAmount)
{
    if (nAmount == 0)
        return;
    std::map<Coord, LootInfo>::iterator mi = loot.find(coord);
    if (mi != loot.end())
    {
        if ((mi->second.nAmount += nAmount) == 0)
            loot.erase(mi);
        else
            mi->second.lastBlock = nHeight;
    }
    else
        loot.insert(std::make_pair(coord, LootInfo(nAmount, nHeight)));
}

/*

We try to split loot equally among players on a loot tile.
If a character hits its carrying capacity, the remaining coins
are split among the others.  To achieve this effect, we sort
the players by increasing (remaining) capacity -- so the ones
with least remaining capacity pick their share first, and if
it fills the capacity, leave extra coins lying around for the
others to pick up.  Since they are then filled up anyway,
it won't matter if others also leave coins, so no "iteration"
is required.

Note that for indivisible amounts the order of players matters.
For equal capacity (which is particularly true before the
hardfork point), we sort by player/character.  This makes
the new logic compatible with the old one.

The class CharacterOnLootTile takes this sorting into account.

*/

class CharacterOnLootTile
{
public:

  PlayerID pid;
  int cid;

  CharacterState* ch;
  int64_t carryCap;

  /* Get remaining carrying capacity.  */
  inline int64_t
  GetRemainingCapacity () const
  {
    if (carryCap == -1)
      return -1;

    /* During periods of change in the carrying capacity, there may be
       players "overloaded".  Take care of them.  */
    if (carryCap < ch->loot.nAmount)
      return 0;

    return carryCap - ch->loot.nAmount;
  }

  friend bool operator< (const CharacterOnLootTile& a,
                         const CharacterOnLootTile& b);

};

bool
operator< (const CharacterOnLootTile& a, const CharacterOnLootTile& b)
{
  const int64_t remA = a.GetRemainingCapacity ();
  const int64_t remB = b.GetRemainingCapacity ();

  if (remA == remB)
    {
      if (a.pid != b.pid)
        return a.pid < b.pid;
      return a.cid < b.cid;
    }

  if (remA == -1)
    {
      assert (remB >= 0);
      return false;
    }
  if (remB == -1)
    {
      assert (remA >= 0);
      return true;
    }

  return remA < remB;
}

void GameState::DivideLootAmongPlayers()
{
    std::map<Coord, int> playersOnLootTile;
    std::vector<CharacterOnLootTile> collectors;
    BOOST_FOREACH (PAIRTYPE(const PlayerID, PlayerState)& p, players)
      BOOST_FOREACH (PAIRTYPE(const int, CharacterState)& pc,
                     p.second.characters)
        {
          CharacterOnLootTile tileChar;

          tileChar.pid = p.first;
          tileChar.cid = pc.first;
          tileChar.ch = &pc.second;

          const bool isCrownHolder = (tileChar.pid == crownHolder.player
                                      && tileChar.cid == crownHolder.index);
          tileChar.carryCap = GetCarryingCapacity (nHeight, tileChar.cid == 0,
                                                   isCrownHolder);

          const Coord& coord = tileChar.ch->coord;

          // ghosting with phasing-in
          if (ForkInEffect (FORK_TIMESAVE, nHeight))
            if ((((coord.x % 2) + (coord.y % 2) > 1) && (nHeight % 500 >= 300)) ||  // for 150 blocks, every 4th coin spawn is ghosted
                (((coord.x % 2) + (coord.y % 2) > 0) && (nHeight % 500 >= 450)) ||  // for 30 blocks, 3 out of 4 coin spawns are ghosted
                (nHeight % 500 >= 480))                                             // for 20 blocks, full ghosting
                     continue;

          if (loot.count (coord) > 0)
            {
              std::map<Coord, int>::iterator mi;
              mi = playersOnLootTile.find (coord);

              if (mi != playersOnLootTile.end ())
                mi->second++;
              else
                playersOnLootTile.insert (std::make_pair (coord, 1));

              collectors.push_back (tileChar);
            }
        }

    std::sort (collectors.begin (), collectors.end ());
    for (std::vector<CharacterOnLootTile>::iterator i = collectors.begin ();
         i != collectors.end (); ++i)
      {
        const Coord& coord = i->ch->coord;
        std::map<Coord, int>::iterator mi = playersOnLootTile.find (coord);
        assert (mi != playersOnLootTile.end ());

        LootInfo lootInfo = loot[coord];
        assert (mi->second > 0);
        lootInfo.nAmount /= (mi->second--);

        /* If amount was ~1e-8 and several players moved onto it, then
           some of them will get nothing.  */
        if (lootInfo.nAmount > 0)
          {
            const int64_t rem = i->ch->CollectLoot (lootInfo, nHeight,
                                                    i->carryCap);
            AddLoot (coord, rem - lootInfo.nAmount);
          }
      }
}

void GameState::UpdateCrownState(bool &respawn_crown)
{
    respawn_crown = false;
    if (crownHolder.player.empty())
        return;

    std::map<PlayerID, PlayerState>::const_iterator mi = players.find(crownHolder.player);
    if (mi == players.end())
    {
        // Player is dead, drop the crown
        crownHolder = CharacterID();
        return;
    }

    const PlayerState &pl = mi->second;
    std::map<int, CharacterState>::const_iterator mi2 = pl.characters.find(crownHolder.index);
    if (mi2 == pl.characters.end())
    {
        // Character is dead, drop the crown
        crownHolder = CharacterID();
        return;
    }

    if (IsBank (mi2->second.coord))
    {
        // Character entered spawn area, drop the crown
        crownHolder = CharacterID();
        respawn_crown = true;
    }
    else
    {
        // Update crown position to character position
        crownPos = mi2->second.coord;
    }
}

void
GameState::CrownBonus (int64_t nAmount)
{
  if (!crownHolder.player.empty ())
    {
      PlayerState& p = players[crownHolder.player];
      CharacterState& ch = p.characters[crownHolder.index];

      const LootInfo loot(nAmount, nHeight);
      const int64_t cap = GetCarryingCapacity (nHeight, crownHolder.index == 0,
                                               true);
      const int64_t rem = ch.CollectLoot (loot, nHeight, cap);

      /* We keep to the logic of "crown on the floor -> game fund" and
         don't distribute coins that can not be hold by the crown holder
         due to carrying capacity to the map.  */
      gameFund += rem;
    }
  else
    gameFund += nAmount;
}

unsigned
GameState::GetNumInitialCharacters () const
{
  return (ForkInEffect (FORK_POISON, nHeight) ? 1 : 3);
}

bool
GameState::IsBank (const Coord& c) const
{
  assert (!banks.empty ());
  return banks.count (c) > 0;
}

int64_t
GameState::GetCoinsOnMap () const
{
  int64_t onMap = 0;
  BOOST_FOREACH(const PAIRTYPE(Coord, LootInfo)& l, loot)
    onMap += l.second.nAmount;
  BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState)& p, players)
    {
      onMap += p.second.value;
      BOOST_FOREACH(const PAIRTYPE(int, CharacterState)& pc,
                    p.second.characters)
        onMap += pc.second.loot.nAmount;
    }

  return onMap;
}

void GameState::CollectHearts(RandomGenerator &rnd)
{
    std::map<Coord, std::vector<PlayerState*> > playersOnHeartTile;
    for (std::map<PlayerID, PlayerState>::iterator mi = players.begin(); mi != players.end(); mi++)
    {
        PlayerState *pl = &mi->second;
        if (!pl->CanSpawnCharacter())
            continue;
        BOOST_FOREACH(PAIRTYPE(const int, CharacterState) &pc, pl->characters)
        {
            const CharacterState &ch = pc.second;

            if (hearts.count(ch.coord))
                playersOnHeartTile[ch.coord].push_back(pl);
        }
    }
    for (std::map<Coord, std::vector<PlayerState*> >::iterator mi = playersOnHeartTile.begin(); mi != playersOnHeartTile.end(); mi++)
    {
        const Coord &c = mi->first;
        std::vector<PlayerState*> &v = mi->second;
        int n = v.size();
        int i;
        for (;;)
        {
            if (!n)
            {
                i = -1;
                break;
            }
            i = n == 1 ? 0 : rnd.GetIntRnd(n);
            if (v[i]->CanSpawnCharacter())
                break;
            v.erase(v.begin() + i);
            n--;
        }
        if (i >= 0)
        {
            v[i]->SpawnCharacter(nHeight, rnd);
            hearts.erase(c);
        }
    }
}

void GameState::CollectCrown(RandomGenerator &rnd, bool respawn_crown)
{
    if (!crownHolder.player.empty())
    {
        assert(!respawn_crown);
        return;
    }

    if (respawn_crown)
    {   
        int a = rnd.GetIntRnd(NUM_CROWN_LOCATIONS);
        crownPos.x = CrownSpawn[2 * a];
        crownPos.y = CrownSpawn[2 * a + 1];
    }

    std::vector<CharacterID> charactersOnCrownTile;
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &pl, players)
    {
        BOOST_FOREACH(const PAIRTYPE(int, CharacterState) &pc, pl.second.characters)
        {
            if (pc.second.coord == crownPos)
                charactersOnCrownTile.push_back(CharacterID(pl.first, pc.first));
        }
    }
    int n = charactersOnCrownTile.size();
    if (!n)
        return;
    int i = n == 1 ? 0 : rnd.GetIntRnd(n);
    crownHolder = charactersOnCrownTile[i];
}

// Loot is pushed out from the spawn area to avoid some ambiguities with banking rules (as spawn areas are also banks)
// Note: the map must be constructed in such a way that there are no obstacles near spawn areas
static Coord
PushCoordOutOfSpawnArea(const Coord &c)
{
    if (!IsOriginalSpawnArea(c))
        return c;
    if (c.x == 0)
    {
        if (c.y == 0)
            return Coord(c.x + 1, c.y + 1);
        else if (c.y == MAP_HEIGHT - 1)
            return Coord(c.x + 1, c.y - 1);
        else
            return Coord(c.x + 1, c.y);
    }
    else if (c.x == MAP_WIDTH - 1)
    {
        if (c.y == 0)
            return Coord(c.x - 1, c.y + 1);
        else if (c.y == MAP_HEIGHT - 1)
            return Coord(c.x - 1, c.y - 1);
        else
            return Coord(c.x - 1, c.y);
    }
    else if (c.y == 0)
        return Coord(c.x, c.y + 1);
    else if (c.y == MAP_HEIGHT - 1)
        return Coord(c.x, c.y - 1);
    else
        return c;     // Should not happen
}

void
GameState::HandleKilledLoot (const PlayerID& pId, int chInd,
                             const KilledByInfo& info, StepResult& step)
{
  const PlayerStateMap::const_iterator mip = players.find (pId);
  assert (mip != players.end ());
  const PlayerState& pc = mip->second;
  assert (pc.value >= 0);
  const std::map<int, CharacterState>::const_iterator mic
    = pc.characters.find (chInd);
  assert (mic != pc.characters.end ());
  const CharacterState& ch = mic->second;

  /* If refunding is possible, do this for the locked amount right now.
     Later on, exclude the amount from further considerations.  */
  bool refunded = false;
  if (chInd == 0 && info.CanRefund (nHeight, pc))
    {
      CollectedLootInfo loot;
      loot.SetRefund (pc.value, nHeight);
      CollectedBounty b(pId, chInd, loot, pc.address);
      step.bounties.push_back (b);
      refunded = true;
    }

  /* Calculate loot.  If we kill a general, take the locked coin amount
     into account, as well.  When life-steal is in effect, the value
     should already be drawn to zero (unless we have a cause of death
     that refunds).  */
  int64_t nAmount = ch.loot.nAmount;
  if (chInd == 0 && !refunded)
    {
      assert (!ForkInEffect (FORK_LIFESTEAL, nHeight) || pc.value == 0);
      nAmount += pc.value;
    }

  /* Apply the miner tax: 4%.  */
  if (info.HasDeathTax ())
    {
      const int64_t nTax = nAmount / 25;
      step.nTaxAmount += nTax;
      nAmount -= nTax;
    }

  /* If requested (and the corresponding fork is in effect), add the coins
     to the game fund instead of dropping them.  */
  if (!info.DropCoins (nHeight, pc))
    {
      gameFund += nAmount;
      return;
    }

  /* Just drop the loot.  Push the coordinate out of spawn if applicable.
     After the life-steal fork with dynamic banks, we no longer push.  */
  Coord lootPos = ch.coord;
  if (!ForkInEffect (FORK_LIFESTEAL, nHeight))
    lootPos = PushCoordOutOfSpawnArea (lootPos);
  AddLoot (lootPos, nAmount);
}

void
GameState::FinaliseKills (StepResult& step)
{
  const PlayerSet& killedPlayers = step.GetKilledPlayers ();
  const KilledByMap& killedBy = step.GetKilledBy ();

  /* Kill depending characters.  */
  BOOST_FOREACH(const PlayerID& victim, killedPlayers)
    {
      const PlayerState& victimState = players.find (victim)->second;

      /* Take a look at the killed info to determine flags for handling
         the player loot.  */
      const KilledByMap::const_iterator iter = killedBy.find (victim);
      assert (iter != killedBy.end ());
      const KilledByInfo& info = iter->second;

      /* Kill all alive characters of the player.  */
      BOOST_FOREACH(const PAIRTYPE(int, CharacterState)& pc,
                    victimState.characters)
        HandleKilledLoot (victim, pc.first, info, step);
    }

  /* Erase killed players from the state.  */
  BOOST_FOREACH(const PlayerID& victim, killedPlayers)
    players.erase (victim);
}

bool
GameState::CheckForDisaster (RandomGenerator& rng) const
{
  /* Before the hardfork, nothing should happen.  */
  if (!ForkInEffect (FORK_POISON, nHeight))
    return false;

  /* Enforce max/min times.  */
  const int dist = nHeight - nDisasterHeight;
  assert (dist > 0);
  if (dist < PDISASTER_MIN_TIME)
    return false;
  if (dist >= PDISASTER_MAX_TIME)
    return true;

  /* Check random chance.  */
  return (rng.GetIntRnd (PDISASTER_PROBABILITY) == 0);
}

void
GameState::KillSpawnArea (StepResult& step)
{
  /* Even if spawn death is disabled after the corresponding softfork,
     we still want to do the loop (but not actually kill players)
     because it keeps stay_in_spawn_area up-to-date.  */

  BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, players)
    {
      std::set<int> toErase;
      BOOST_FOREACH(PAIRTYPE(const int, CharacterState) &pc,
                    p.second.characters)
        {
          const int i = pc.first;
          CharacterState &ch = pc.second;

          // process logout timer
          if (ForkInEffect (FORK_TIMESAVE, nHeight))
          {
              if (IsBank (ch.coord))
              {
                  ch.stay_in_spawn_area = CHARACTER_MODE_LOGOUT; // hunters will never be on bank tile while in spectator mode
              }
              else if (SpawnMap[ch.coord.y][ch.coord.x] & SPAWNMAPFLAG_PLAYER)
              {
                  if (CharacterSpawnProtectionAlmostFinished(ch.stay_in_spawn_area))
                  {
                      // enter spectator mode if standing still
                      // notes : - movement will put the hunter in normal mode (when movement is processed)
                      //         - right now (in KillSpawnArea) waypoint updates are not yet applied for current block,
                      //           i.e. (ch.waypoints.empty()) is always true
                      ch.stay_in_spawn_area = CHARACTER_MODE_SPECTATOR_BEGIN;
                  }
                  else
                  {
                      // give new hunters 10 blocks more thinking time before ghosting ends
                      if ((nHeight % 500 < 490) || (ch.stay_in_spawn_area > 0))
                          ch.stay_in_spawn_area++;
                  }
              }
              else if (CharacterIsProtected(ch.stay_in_spawn_area)) // catch all (for hunters who spawned pre-fork)
              {
                  ch.stay_in_spawn_area++;
              }

              if (CharacterNoLogout(ch.stay_in_spawn_area))
                  continue;
          }
          else
          {
              if (!IsBank (ch.coord))
              {
                ch.stay_in_spawn_area = 0;
                continue;
              }

              /* Make sure to increment the counter in every case.  */
              assert (IsBank (ch.coord));
              const int maxStay = MaxStayOnBank (nHeight);
              if (ch.stay_in_spawn_area++ < maxStay || maxStay == -1)
                continue;
          }

          /* Handle the character's loot and kill the player.  */
          const KilledByInfo killer(KilledByInfo::KILLED_SPAWN);
          HandleKilledLoot (p.first, i, killer, step);
          if (i == 0)
            step.KillPlayer (p.first, killer);

          /* Cannot erase right now, because it will invalidate the
             iterator 'pc'.  */
          toErase.insert(i);
        }
      BOOST_FOREACH(int i, toErase)
        p.second.characters.erase(i);
    }
}

void
GameState::ApplyDisaster (RandomGenerator& rng)
{
  /* Set random life expectations for every player on the map.  */
  BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState)& p, players)
    {
      /* Disasters should be so far apart, that all currently alive players
         are not yet poisoned.  Check this.  In case we introduce a general
         expiry, this can be changed accordingly -- but make sure that
         poisoning doesn't actually *increase* the life expectation.  */
      assert (p.second.remainingLife == -1);

      p.second.remainingLife = rng.GetIntRnd (POISON_MIN_LIFE, POISON_MAX_LIFE);
    }

  /* Remove all hearts from the map.  */
  if (ForkInEffect (FORK_LESSHEARTS, nHeight))
    hearts.clear ();

  /* Reset disaster counter.  */
  nDisasterHeight = nHeight;
}

void
GameState::DecrementLife (StepResult& step)
{
  BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState)& p, players)
    {
      if (p.second.remainingLife == -1)
        continue;

      assert (p.second.remainingLife > 0);
      --p.second.remainingLife;

      if (p.second.remainingLife == 0)
        {
          const KilledByInfo killer(KilledByInfo::KILLED_POISON);
          step.KillPlayer (p.first, killer);
        }
    }
}

void
GameState::RemoveHeartedCharacters (StepResult& step)
{
  assert (IsForkHeight (FORK_LIFESTEAL, nHeight));

  /* Get rid of all hearts on the map.  */
  hearts.clear ();

  /* Immediately kill all hearted characters.  */
  BOOST_FOREACH (PAIRTYPE(const PlayerID, PlayerState)& p, players)
    {
      std::set<int> toErase;
      BOOST_FOREACH (PAIRTYPE(const int, CharacterState)& pc,
                     p.second.characters)
        {
          const int i = pc.first;
          if (i == 0)
            continue;

          const KilledByInfo info(KilledByInfo::KILLED_POISON);
          HandleKilledLoot (p.first, i, info, step);

          /* Cannot erase right now, because it will invalidate the
             iterator 'pc'.  */
          toErase.insert (i);
        }
      BOOST_FOREACH (int i, toErase)
        p.second.characters.erase (i);
    }
}

void
GameState::UpdateBanks (RandomGenerator& rng)
{
  if (!ForkInEffect (FORK_LIFESTEAL, nHeight))
    return;

  std::map<Coord, unsigned> newBanks;

  /* Create initial set of banks at the fork itself.  */
  if (IsForkHeight (FORK_LIFESTEAL, nHeight))
    assert (newBanks.empty ());

  /* Decrement life of existing banks and remove the ones that
     have run out.  */
  else
    {
      assert (banks.size () == DYNBANKS_NUM_BANKS);
      assert (newBanks.empty ());

      BOOST_FOREACH (const PAIRTYPE(Coord, unsigned)& b, banks)
      {
        assert (b.second >= 1);

        // reset all banks as to not break things,
        // e.g. "assert (optionsSet.count (b.first) == 1)"
        if (IsForkHeight (FORK_TIMESAVE, nHeight))
          continue;

        /* Banks with life=1 run out now.  Since banking is done before
           updating the banks in PerformStep, this means that banks that have
           life=1 and are reached in the next turn are still available.  */
        if (b.second > 1)
          newBanks.insert (std::make_pair (b.first, b.second - 1));
      }
    }

  /* Re-create banks that are missing now.  */

  assert (newBanks.size () <= DYNBANKS_NUM_BANKS);

  // less possible bank spawn tiles
  if (ForkInEffect (FORK_TIMESAVE, nHeight))
  {
  FillWalkableTiles ();
  std::set<Coord> optionsSet(walkableTiles_ts_banks.begin (), walkableTiles_ts_banks.end ());
  BOOST_FOREACH (const PAIRTYPE(Coord, unsigned)& b, newBanks)
    {
      assert (optionsSet.count (b.first) == 1);
      optionsSet.erase (b.first);
    }
  assert (optionsSet.size () + newBanks.size () == walkableTiles_ts_banks.size ());

  std::vector<Coord> options(optionsSet.begin (), optionsSet.end ());
  for (unsigned cnt = newBanks.size (); cnt < DYNBANKS_NUM_BANKS; ++cnt)
    {
      const int ind = rng.GetIntRnd (options.size ());
      const int life = rng.GetIntRnd (DYNBANKS_MIN_LIFE, DYNBANKS_MAX_LIFE);
      const Coord& c = options[ind];

      assert (newBanks.count (c) == 0);
      newBanks.insert (std::make_pair (c, life));

      /* Do not use a silly trick like swapping in the last element.
         We want to keep the array ordered at all times.  The order is
         important with respect to consensus, and this makes the consensus
         protocol "clearer" to describe.  */
      options.erase (options.begin () + ind);
    }
  }
  else // pre-fork
  {
    FillWalkableTiles ();
    std::set<Coord> optionsSet(walkableTiles.begin (), walkableTiles.end ());
    BOOST_FOREACH (const PAIRTYPE(Coord, unsigned)& b, newBanks)
    {
      assert (optionsSet.count (b.first) == 1);
      optionsSet.erase (b.first);
    }
    assert (optionsSet.size () + newBanks.size () == walkableTiles.size ());

    std::vector<Coord> options(optionsSet.begin (), optionsSet.end ());
    for (unsigned cnt = newBanks.size (); cnt < DYNBANKS_NUM_BANKS; ++cnt)
    {
      const int ind = rng.GetIntRnd (options.size ());
      const int life = rng.GetIntRnd (DYNBANKS_MIN_LIFE, DYNBANKS_MAX_LIFE);
      const Coord& c = options[ind];

      assert (newBanks.count (c) == 0);
      newBanks.insert (std::make_pair (c, life));

      /* Do not use a silly trick like swapping in the last element.
         We want to keep the array ordered at all times.  The order is
         important with respect to consensus, and this makes the consensus
         protocol "clearer" to describe.  */
      options.erase (options.begin () + ind);
    }
  }

  banks.swap (newBanks);
  assert (banks.size () == DYNBANKS_NUM_BANKS);
}

/* ************************************************************************** */

void
CollectedBounty::UpdateAddress (const GameState& state)
{
  const PlayerID& p = character.player;
  const PlayerStateMap::const_iterator i = state.players.find (p);
  if (i == state.players.end ())
    return;

  address = i->second.address;
}

bool Game::PerformStep(const GameState &inState, const StepData &stepData, GameState &outState, StepResult &stepResult)
{
    BOOST_FOREACH(const Move &m, stepData.vMoves)
        if (!m.IsValid(inState))
            return false;

    outState = inState;

    /* Initialise basic stuff.  The disaster height is set to the old
       block's for now, but it may be reset later when we decide that
       a disaster happens at this block.  */
    outState.nHeight = inState.nHeight + 1;
    outState.nDisasterHeight = inState.nDisasterHeight;
    outState.hashBlock = stepData.newHash;
    outState.dead_players_chat.clear();

    stepResult = StepResult();

    /* Pay out game fees (except for spawns) to the game fund.  This also
       keeps track of the total fees paid into the game world by moves.  */
    int64_t moneyIn = 0;
    BOOST_FOREACH(const Move& m, stepData.vMoves)
      if (!m.IsSpawn ())
        {
          const PlayerStateMap::iterator mi = outState.players.find (m.player);
          assert (mi != outState.players.end ());
          assert (m.newLocked >= mi->second.lockedCoins);
          const int64_t newFee = m.newLocked - mi->second.lockedCoins;
          outState.gameFund += newFee;
          moneyIn += newFee;
          mi->second.lockedCoins = m.newLocked;
        }
      else
        moneyIn += m.newLocked;

    // Apply attacks
    CharactersOnTiles attackedTiles;
    attackedTiles.ApplyAttacks (outState, stepData.vMoves);
    if (ForkInEffect (FORK_LIFESTEAL, outState.nHeight))
      attackedTiles.DefendMutualAttacks (outState);
    attackedTiles.DrawLife (outState, stepResult);

    // Kill players who stay too long in the spawn area
    outState.KillSpawnArea (stepResult);

    /* Decrement poison life expectation and kill players when it
       has dropped to zero.  */
    outState.DecrementLife (stepResult);

    /* Finalise the kills.  */
    outState.FinaliseKills (stepResult);

    /* Special rule for the life-steal fork:  When it takes effect,
       remove all hearted characters from the map.  Also heart creation
       is disabled, so no hearted characters will ever be present
       afterwards.  */
    if (IsForkHeight (FORK_LIFESTEAL, outState.nHeight))
      outState.RemoveHeartedCharacters (stepResult);

    /* Apply updates to target coordinate.  This ignores already
       killed players.  */
    BOOST_FOREACH(const Move &m, stepData.vMoves)
        if (!m.IsSpawn())
            m.ApplyWaypoints(outState);

    // For all alive players perform path-finding
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, outState.players)
        BOOST_FOREACH(PAIRTYPE(const int, CharacterState) &pc, p.second.characters)
    {
        // can't move in spectator mode, moving will lose spawn protection
        if ((ForkInEffect (FORK_TIMESAVE, outState.nHeight)) &&
            ( ! (pc.second.waypoints.empty()) ))
        {
            if (CharacterInSpectatorMode(pc.second.stay_in_spawn_area))
                pc.second.StopMoving();
            else
                pc.second.stay_in_spawn_area = CHARACTER_MODE_NORMAL;
        }
        pc.second.MoveTowardsWaypoint();
    }

    bool respawn_crown = false;
    outState.UpdateCrownState(respawn_crown);

    // Caution: banking must not depend on the randomized events, because they depend on the hash -
    // miners won't be able to compute tax amount if it depends on the hash.

    // Banking
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, outState.players)
        BOOST_FOREACH(PAIRTYPE(const int, CharacterState) &pc, p.second.characters)
        {
            int i = pc.first;
            CharacterState &ch = pc.second;

            // player spawn tiles work like banks (for the purpose of banking)
            if (((ch.loot.nAmount > 0) && (outState.IsBank (ch.coord))) ||
                ((ForkInEffect (FORK_TIMESAVE, outState.nHeight)) && (ch.loot.nAmount > 0) && (IsInsideMap(ch.coord.x, ch.coord.y)) && (SpawnMap[ch.coord.y][ch.coord.x] & SPAWNMAPFLAG_PLAYER)))
            {
                // Tax from banking: 10%
                int64_t nTax = ch.loot.nAmount / 10;
                stepResult.nTaxAmount += nTax;
                ch.loot.nAmount -= nTax;

                CollectedBounty b(p.first, i, ch.loot, p.second.address);
                stepResult.bounties.push_back (b);
                ch.loot = CollectedLootInfo();
            }
        }

    // Miners set hashBlock to 0 in order to compute tax and include it into the coinbase.
    // At this point the tax is fully computed, so we can return.
    if (outState.hashBlock == 0)
        return true;

    RandomGenerator rnd(outState.hashBlock);

    /* Decide about whether or not this will be a disaster.  It should be
       the first action done with the RNG, so that it is possible to
       verify whether or not a block hash leads to a disaster
       relatively easily.  */
    const bool isDisaster = outState.CheckForDisaster (rnd);
    if (isDisaster)
      {
        printf ("DISASTER @%d!\n", outState.nHeight);
        outState.ApplyDisaster (rnd);
        assert (outState.nHeight == outState.nDisasterHeight);
      }

    /* Transfer life from attacks.  This is done randomly, but the decision
       about who dies is non-random and already set above.  */
    if (ForkInEffect (FORK_LIFESTEAL, outState.nHeight))
      attackedTiles.DistributeDrawnLife (rnd, outState);

    // Spawn new players
    BOOST_FOREACH(const Move &m, stepData.vMoves)
        if (m.IsSpawn())
            m.ApplySpawn(outState, rnd);

    // Apply address & message updates
    BOOST_FOREACH(const Move &m, stepData.vMoves)
        m.ApplyCommon(outState);

    /* In the (rare) case that a player collected a bounty, is still alive
       and changed the reward address at the same time, make sure that the
       bounty is paid to the new address to match the old network behaviour.  */
    BOOST_FOREACH(CollectedBounty& bounty, stepResult.bounties)
      bounty.UpdateAddress (outState);

    // Set colors for dead players, so their messages can be shown in the chat window
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, outState.dead_players_chat)
    {
        std::map<PlayerID, PlayerState>::const_iterator mi = inState.players.find(p.first);
        assert(mi != inState.players.end());
        const PlayerState &pl = mi->second;
        p.second.color = pl.color;
    }

    // Drop a random rewards onto the harvest areas
    const int64_t nCrownBonus
      = CROWN_BONUS * stepData.nTreasureAmount / TOTAL_HARVEST;
    int64_t nTotalTreasure = 0;
    for (int i = 0; i < NUM_HARVEST_AREAS; i++)
    {
        int a = rnd.GetIntRnd(HarvestAreaSizes[i]);
        Coord harvest(HarvestAreas[i][2 * a], HarvestAreas[i][2 * a + 1]);
        const int64_t nTreasure
          = HarvestPortions[i] * stepData.nTreasureAmount / TOTAL_HARVEST;
        outState.AddLoot(harvest, nTreasure);
        nTotalTreasure += nTreasure;
    }
    assert(nTotalTreasure + nCrownBonus == stepData.nTreasureAmount);

    // Players collect loot
    outState.DivideLootAmongPlayers();
    outState.CrownBonus(nCrownBonus);

    /* Update the banks.  */
    outState.UpdateBanks (rnd);

    /* Drop heart onto the map.  They are not dropped onto the original
       spawn area for historical reasons.  After the life-steal fork,
       we simply remove this check (there are no hearts anyway).  */
    if (DropHeart (outState.nHeight))
    {
        assert (!ForkInEffect (FORK_LIFESTEAL, outState.nHeight));

        Coord heart;
        do
        {
            heart.x = rnd.GetIntRnd(MAP_WIDTH);
            heart.y = rnd.GetIntRnd(MAP_HEIGHT);
        } while (!IsWalkable(heart) || IsOriginalSpawnArea (heart));
        outState.hearts.insert(heart);
    }

    outState.CollectHearts(rnd);
    outState.CollectCrown(rnd, respawn_crown);

    /* Compute total money out of the game world via bounties paid.  */
    int64_t moneyOut = stepResult.nTaxAmount;
    BOOST_FOREACH(const CollectedBounty& b, stepResult.bounties)
      moneyOut += b.loot.nAmount;

    /* Compare total money before and after the step.  If there is a mismatch,
       we have a bug in the logic.  Better not accept the new game state.  */
    const int64_t moneyBefore = inState.GetCoinsOnMap () + inState.gameFund;
    const int64_t moneyAfter = outState.GetCoinsOnMap () + outState.gameFund;
    if (moneyBefore + stepData.nTreasureAmount + moneyIn
          != moneyAfter + moneyOut)
      {
        printf ("Old game state: %lld (@%d)\n", moneyBefore, inState.nHeight);
        printf ("New game state: %lld\n", moneyAfter);
        printf ("Money in:  %lld\n", moneyIn);
        printf ("Money out: %lld\n", moneyOut);
        printf ("Treasure placed: %lld\n", stepData.nTreasureAmount);
        return error ("total amount before and after step mismatch");
      }

    return true;
}
