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

namespace Game
{

inline bool IsInSpawnArea(const Coord &c)
{
    return IsInSpawnArea(c.x, c.y);
}

inline bool IsWalkable(const Coord &c)
{
    return IsWalkable(c.x, c.y);
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

bool ExtractField(json_spirit::Object &obj, const std::string field, json_spirit::Value &v)
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
    if (IsSpawn())
        return state.players.count(player) == 0;
    else
        return state.players.count(player) != 0;
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
        {
            if (bWaypoints)
                return false;     // Cannot combine destruct and waypoints
            destruct.insert(i);
        }
        else if (bWaypoints)
            waypoints[i] = wp;

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
  PlayerState &pl = state.players[player];
  if (pl.next_character_index == 0)
  {
    pl.color = color;
    assert (pl.coinAmount == -1 && coinAmount >= 0);
    pl.coinAmount = coinAmount;

    const unsigned limit = state.GetNumInitialCharacters ();
    for (unsigned i = 0; i < limit; i++)
      pl.SpawnCharacter (rnd);
  }
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

// Returns direction from c1 to c2 as a number from 1 to 9 (as on the numeric keypad)
unsigned char GetDirection(const Coord &c1, const Coord &c2)
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

std::string CharacterID::ToString() const
{
    if (!index)
        return player;
    return player + strprintf(".%d", int(index));
}

void CharacterState::Spawn(int color, RandomGenerator &rnd)
{
    int pos = rnd.GetIntRnd(2 * SPAWN_AREA_LENGTH - 1);
    int x = pos < SPAWN_AREA_LENGTH ? pos : 0;
    int y = pos < SPAWN_AREA_LENGTH ? 0 : pos - SPAWN_AREA_LENGTH;
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

    StopMoving();
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

void PlayerState::SpawnCharacter(RandomGenerator &rnd)
{
    characters[next_character_index++].Spawn(color, rnd);
}

json_spirit::Value PlayerState::ToJsonValue(int crown_index, bool dead /* = false*/) const
{
    using namespace json_spirit;

    Object obj;
    obj.push_back(Pair("color", (int)color));
    obj.push_back(Pair("coinAmount", ValueFromAmount(coinAmount)));

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

GameState::GameState()
{
    crownPos.x = CROWN_START_X;
    crownPos.y = CROWN_START_Y;
    lostCoins = 0;
    nHeight = -1;
    nDisasterHeight = -1;
    hashBlock = 0;
}

void
GameState::UpdateVersion(int oldVersion)
{
  /* Last version change is beyond the last version where the game db
     is fully reconstructed.  */
  assert (oldVersion >= 1001100);

  /* No upgrades to game state are necessary since this change.  */
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
    arr.resize(0);
    BOOST_FOREACH(const Coord &c, hearts)
    {
        Object subobj;
        subobj.push_back(Pair("x", c.x));
        subobj.push_back(Pair("y", c.y));
        arr.push_back(subobj);
    }
    obj.push_back(Pair("hearts", arr));

    subobj.clear();
    subobj.push_back(Pair("x", crownPos.x));
    subobj.push_back(Pair("y", crownPos.y));
    if (!crownHolder.player.empty())
    {
        subobj.push_back(Pair("holderName", crownHolder.player));
        subobj.push_back(Pair("holderIndex", crownHolder.index));
    }
    obj.push_back(Pair("crown", subobj));

    obj.push_back (Pair("lostCoins", ValueFromAmount (lostCoins)));
    obj.push_back (Pair("height", nHeight));
    obj.push_back (Pair("disasterHeight", nDisasterHeight));
    obj.push_back (Pair("hashBlock", hashBlock.ToString().c_str()));

    return obj;
}

void GameState::AddLoot(Coord coord, int64 nAmount)
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

void GameState::DivideLootAmongPlayers()
{
    std::map<Coord, int> playersOnLootTile;
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, players)
    {
        BOOST_FOREACH(const PAIRTYPE(int, CharacterState) &pc, p.second.characters)
        {
            int i = pc.first;
            const CharacterState &ch = pc.second;

            const Coord &coord = ch.coord;
            if (loot.count(coord) != 0)
            {
                std::map<Coord, int>::iterator mi = playersOnLootTile.find(coord);
                if (mi != playersOnLootTile.end())
                    mi->second++;
                else
                    playersOnLootTile.insert(std::make_pair(coord, 1));
            }
        }
    }
    // Split equally, if multiple players on loot cell
    // If not divisible, the amounts are dependent on the order of players
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, players)
    {
        BOOST_FOREACH(PAIRTYPE(const int, CharacterState) &pc, p.second.characters)
        {
            int i = pc.first;
            CharacterState &ch = pc.second;

            const Coord &coord = ch.coord;
            std::map<Coord, int>::iterator mi = playersOnLootTile.find(coord);
            if (mi != playersOnLootTile.end())
            {
                LootInfo lootInfo = loot[coord];
                lootInfo.nAmount /= (mi->second--);

                // If amount was ~1e-8 and several players moved onto it, then some of them will get nothing
                if (lootInfo.nAmount > 0)
                {
                    ch.loot.Collect(lootInfo, nHeight);
                    AddLoot(coord, -lootInfo.nAmount);
                }

                assert((mi->second == 0) == (loot.count(coord) == 0));   // If no more players on this tile, then all loot must be collected
            }
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

    if (IsInSpawnArea(mi2->second.coord))
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
GameState::CrownBonus (int64 nAmount)
{
  if (!crownHolder.player.empty ())
    {
      PlayerState& p = players[crownHolder.player];
      CharacterState& ch = p.characters[crownHolder.index];
      ch.loot.Collect (LootInfo(nAmount, nHeight), nHeight);
    }
  else
    lostCoins += nAmount;
}

unsigned
GameState::GetNumInitialCharacters () const
{
  return (nHeight < FORK_HEIGHT_POISON ? 3 : 1);
}

int64
GameState::GetCoinsOnMap () const
{
  int64 onMap = 0;
  BOOST_FOREACH(const PAIRTYPE(Coord, LootInfo)& l, loot)
    onMap += l.second.nAmount;
  BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState)& p, players)
    BOOST_FOREACH(const PAIRTYPE(int, CharacterState)& pc, p.second.characters)
      onMap += pc.second.loot.nAmount;

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
            v[i]->SpawnCharacter(rnd);
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
    if (!IsInSpawnArea(c))
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
GameState::FinaliseKills (StepResult& step)
{
  const PlayerSet& killedPlayers = step.GetKilledPlayers ();
  const KilledByMap& killedBy = step.GetKilledBy ();

  /* Kill depending characters.  */
  BOOST_FOREACH(const PlayerID& victim, killedPlayers)
    {
      const PlayerState& victimState = players.find (victim)->second;

      /* If killed by the game for staying in the spawn area, then no tax.  */
      const KilledByMap::const_iterator iter = killedBy.find (victim);
      assert (iter != killedBy.end ());
      const bool apply_tax = iter->second.HasDeathTax ();

      /* Kill all alive characters of the player.  */
      BOOST_FOREACH(const PAIRTYPE(int, CharacterState)& pc,
                    victimState.characters)
        {
          const int i = pc.first;
          const CharacterState& ch = pc.second;

          int64 nAmount = ch.loot.nAmount;
          if (i == 0)
            nAmount += victimState.coinAmount;
          if (nAmount > 0)
            {
              if (apply_tax)
                {
                  // Tax from killing: 4%
                  const int64 nTax = nAmount / 25;
                  step.nTaxAmount += nTax;
                  nAmount -= nTax;
                }

              AddLoot (PushCoordOutOfSpawnArea (ch.coord), nAmount);
            }
        }
    }

  /* Erase killed players from the state.  */
  BOOST_FOREACH(const PlayerID& victim, killedPlayers)
    players.erase (victim);
}

bool
GameState::CheckForDisaster (RandomGenerator& rng) const
{
  /* Before the hardfork, nothing should happen.  */
  if (nHeight < FORK_HEIGHT_POISON)
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
GameState::ApplyPoison (RandomGenerator& rng)
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
CollectedBounty::UpdateAddress (const GameState& state)
{
  const PlayerID& p = character.player;
  const PlayerStateMap::const_iterator i = state.players.find (p);
  if (i == state.players.end ())
    return;

  address = i->second.address;
}

struct AttackableCharacter
{
    const std::string *name;
    int index;
    unsigned char color;
};

std::multimap<Coord, AttackableCharacter> *MapCharactersToTiles(const std::map<PlayerID, PlayerState> &players)
{
    std::multimap<Coord, AttackableCharacter> *m = new std::multimap<Coord, AttackableCharacter>();
    for (std::map<PlayerID, PlayerState>::const_iterator p = players.begin(); p != players.end(); p++)
        BOOST_FOREACH(const PAIRTYPE(int, CharacterState) &pc, p->second.characters)
        {
            int i = pc.first;
            const CharacterState &ch = pc.second;

            AttackableCharacter a;
            a.name = &p->first;
            a.index = i;
            a.color = p->second.color;
            m->insert(std::pair<Coord, AttackableCharacter>(ch.coord, a));
        }
    return m;
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

    // Apply attacks
    std::multimap<Coord, AttackableCharacter> *charactersOnTile = NULL;   // Delayed creation - only if at least one attack happened
    BOOST_FOREACH(const Move &m, stepData.vMoves)
    {
        if (m.destruct.empty())
            continue;
        const PlayerState &pl = inState.players.find(m.player)->second;
        BOOST_FOREACH(int i, m.destruct)
        {
            if (!pl.characters.count(i))
                continue;
            CharacterID chid(m.player, i);
            if (inState.crownHolder == chid)
                continue;
            if (!charactersOnTile)
                charactersOnTile = MapCharactersToTiles(inState.players);
            int radius = i == 0 ? DESTRUCT_RADIUS_MAIN : DESTRUCT_RADIUS;
            Coord c = pl.characters.find(i)->second.coord;
            for (int y = c.y - radius; y <= c.y + radius; y++)
                for (int x = c.x - radius; x <= c.x + radius; x++)
                {
                    std::pair<std::multimap<Coord, AttackableCharacter>::const_iterator, std::multimap<Coord, AttackableCharacter>::const_iterator> its =
                                    charactersOnTile->equal_range(Coord(x, y));
                    for (std::multimap<Coord, AttackableCharacter>::const_iterator it = its.first; it != its.second; it++)
                    {
                        const AttackableCharacter &a = it->second;
                        PlayerState& victim = outState.players[*a.name];

                        if (a.color == pl.color)
                            continue;  // Do not kill same color
                        if (a.index == 0)
                        {
                            const KilledByInfo killer(chid);
                            stepResult.KillPlayer (*a.name, killer);
                        }
                        if (victim.characters.count(a.index))
                        {
                            const CharacterState& ch = victim.characters[a.index];
                            // Drop loot
                            int64 nAmount = ch.loot.nAmount;
                            if (a.index == 0)
                              {
                                assert (victim.coinAmount >= 0);
                                nAmount += victim.coinAmount;
                              }
                            if (nAmount > 0)
                              {
                                // Tax from killing: 4%
                                int64 nTax = nAmount / 25;
                                stepResult.nTaxAmount += nTax;
                                nAmount -= nTax;
                                outState.AddLoot(PushCoordOutOfSpawnArea(ch.coord), nAmount);
                              }
                            victim.characters.erase(a.index);
                        }
                    }
                }
            if (outState.players[m.player].characters.count(i))
            {
                CharacterState &ch = outState.players[m.player].characters[i];
                // Drop loot
                int64 nAmount = pl.characters.find(i)->second.loot.nAmount;
                if (i == 0)
                  {
                    assert (pl.coinAmount >= 0);
                    nAmount += pl.coinAmount;
                  }
                if (nAmount > 0)
                  {
                    // Tax from killing: 4%
                    int64 nTax = nAmount / 25;
                    stepResult.nTaxAmount += nTax;
                    nAmount -= nTax;
                    outState.AddLoot(PushCoordOutOfSpawnArea(pl.characters.find(i)->second.coord), nAmount);
                  }
                outState.players[m.player].characters.erase(i);
            }
            if (i == 0)
            {
                const KilledByInfo killer(CharacterID(m.player, i));
                stepResult.KillPlayer (m.player, killer);
            }
        }
    }

    delete charactersOnTile;

    // Kill players who stay too long in the spawn area
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, outState.players)
    {
        std::set<int> toErase;
        BOOST_FOREACH(PAIRTYPE(const int, CharacterState) &pc, p.second.characters)
        {
            int i = pc.first;
            CharacterState &ch = pc.second;

            if (IsInSpawnArea(ch.coord))
            {
                if (ch.stay_in_spawn_area++ >= MAX_STAY_IN_SPAWN_AREA)
                {
                    int64 nAmount = ch.loot.nAmount;
                    if (i == 0)
                    {
                        assert (p.second.coinAmount >= 0);
                        nAmount += p.second.coinAmount;

                        const KilledByInfo killer(KilledByInfo::KILLED_SPAWN);
                        stepResult.KillPlayer (p.first, killer);
                    }
                    if (nAmount > 0)
                        outState.AddLoot(PushCoordOutOfSpawnArea(ch.coord), nAmount);
                    toErase.insert(i);   // Cannot erase right now, because it will invalidate the iterator 'pc'
                }
            }
            else
                ch.stay_in_spawn_area = 0;
        }
        BOOST_FOREACH(int i, toErase)
            p.second.characters.erase(i);
    }

    /* Decrement poison life expectation and kill players when it
       has dropped to zero.  */
    outState.DecrementLife (stepResult);

    /* Finalise the kills.  */
    outState.FinaliseKills (stepResult);

    /* Apply updates to target coordinate.  This ignores already
       killed players.  */
    BOOST_FOREACH(const Move &m, stepData.vMoves)
        if (!m.IsSpawn())
            m.ApplyWaypoints(outState);

    // For all alive players perform path-finding
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, outState.players)
        BOOST_FOREACH(PAIRTYPE(const int, CharacterState) &pc, p.second.characters)
            pc.second.MoveTowardsWaypoint();

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

            if (ch.loot.nAmount > 0 && IsInSpawnArea(ch.coord))
            {
                // Tax from banking: 10%
                int64 nTax = ch.loot.nAmount / 10;
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
        printf ("POISON DISASTER @%d!\n", outState.nHeight);
        outState.ApplyPoison (rnd);
        assert (outState.nHeight == outState.nDisasterHeight);
      }

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

    int64 nCrownBonus = CROWN_BONUS * stepData.nTreasureAmount / TOTAL_HARVEST;

    // Drop a random rewards onto the harvest areas
    int64 nTotalTreasure = 0;
    for (int i = 0; i < NUM_HARVEST_AREAS; i++)
    {
        int a = rnd.GetIntRnd(HarvestAreaSizes[i]);
        Coord harvest(HarvestAreas[i][2 * a], HarvestAreas[i][2 * a + 1]);
        int64 nTreasure = HarvestPortions[i] * stepData.nTreasureAmount / TOTAL_HARVEST;
        outState.AddLoot(harvest, nTreasure);
        nTotalTreasure += nTreasure;
    }
    assert(nTotalTreasure + nCrownBonus == stepData.nTreasureAmount);

    // Players collect loot
    outState.DivideLootAmongPlayers();
    outState.CrownBonus(nCrownBonus);

    // Drop heart onto the map (1 heart per 5 blocks)
    if (outState.nHeight % HEART_EVERY_NTH_BLOCK == 0)
    {
        Coord heart;
        do
        {
            heart.x = rnd.GetIntRnd(MAP_WIDTH);
            heart.y = rnd.GetIntRnd(MAP_HEIGHT);
        } while (!IsWalkable(heart) || IsInSpawnArea(heart));
        outState.hearts.insert(heart);
    }

    outState.CollectHearts(rnd);
    outState.CollectCrown(rnd, respawn_crown);

    return true;
}
