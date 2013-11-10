#include "gamestate.h"
#include "gamemap.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>

#include "util.h"
#include "bignum.h"
#include "base58.h"

using namespace Game;

static bool CheckJsonObject(const json_spirit::Object &obj, const std::map<std::string, json_spirit::Value_type> &expectedFields)
{
    using namespace json_spirit;

    int n = 0;
    BOOST_FOREACH(const PAIRTYPE(std::string, Value_type)& f, expectedFields)
    {
        const char *fieldName = f.first.c_str();
        Value_type fieldType = f.second;
        bool fAllowNull = false;
        if (fieldName[0] == '?')
        {
            fAllowNull = true;
            fieldName++;
        }

        Value v = find_value(obj, fieldName);
        if (v.type() == fieldType)
            n++;
        else
        {
            if (!(v.type() == null_type && fAllowNull))
                return false;
        }
    }
    return obj.size() == n;    // Check that there are no extra fields in obj
}

json_spirit::Value ValueFromAmount(int64 amount);

bool IsValidPlayerName(const PlayerID &player)
{
    // Check player name validity
    // Can contain letters, digits, underscore, hyphen and whitespace
    // Cannot contain double whitespaces or start/end with whitespace
    using namespace boost::xpressive;
    static sregex regex = sregex::compile("^([a-zA-Z0-9_-]+ )*[a-zA-Z0-9_-]+$");
    smatch match;
    return regex_search(player, match, regex);
}

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

private:
    CBigNum state, state0;
    static const CBigNum MIN_STATE;
};

const CBigNum RandomGenerator::MIN_STATE = CBigNum().SetCompact(0x097FFFFFu);



// Various types of moves

struct EmptyMove : public Move
{
    bool IsValid() const
    {
        return true;
    }

    bool IsValid(const GameState &state) const
    {
        return state.players.count(player) != 0;
    }
};

struct SpawnMove : public Move
{
    int color;

    bool IsValid() const
    {
        return color >= 0 || color <= NUM_TEAM_COLORS;
    }

    bool IsValid(const GameState &state) const
    {
        return state.players.count(player) == 0;
    }

    void ApplySpawn(GameState &state, RandomGenerator &rnd) const
    {
        PlayerState newPlayer;
        newPlayer.color = color;
        int pos = rnd.GetIntRnd(2 * SPAWN_AREA_LENGTH - 1);
        int x = pos < SPAWN_AREA_LENGTH ? pos : 0;
        int y = pos < SPAWN_AREA_LENGTH ? 0 : pos - SPAWN_AREA_LENGTH;
        switch (color)
        {
            case 0: // Yellow (top-left)
                newPlayer.coord = Coord(x, y);
                break;
            case 1: // Red (top-right)
                newPlayer.coord = Coord(MAP_WIDTH - 1 - x, y);
                break;
            case 2: // Green (bottom-right)
                newPlayer.coord = Coord(MAP_WIDTH - 1 - x, MAP_HEIGHT - 1 - y);
                break;
            case 3: // Blue (bottom-left)
                newPlayer.coord = Coord(x, MAP_HEIGHT - 1 - y);
                break;
            default:
                throw std::runtime_error("ApplySpawn: incorrect color");
        }

        // Set look-direction for the sprite
        if (newPlayer.coord.x == 0)
        {
            if (newPlayer.coord.y == 0)
                newPlayer.dir = 3;
            else if (newPlayer.coord.y == MAP_HEIGHT - 1)
                newPlayer.dir = 9;
            else
                newPlayer.dir = 6;
        }
        else if (newPlayer.coord.x == MAP_WIDTH - 1)
        {
            if (newPlayer.coord.y == 0)
                newPlayer.dir = 1;
            else if (newPlayer.coord.y == MAP_HEIGHT - 1)
                newPlayer.dir = 7;
            else
                newPlayer.dir = 4;
        }
        else if (newPlayer.coord.y == 0)
            newPlayer.dir = 2;
        else if (newPlayer.coord.y == MAP_HEIGHT - 1)
            newPlayer.dir = 8;

        // Set from and target to coord
        newPlayer.StopMoving();
        state.players[player] = newPlayer;
    }
};

// Sets new waypoint
struct StepMove : public Move
{
    int targetX, targetY;

    bool IsValid() const
    {
        return IsInsideMap(targetX, targetY);
    }

    bool IsValid(const GameState &state) const
    {
        return state.players.count(player) != 0;
    }

    void ApplyStep(GameState &state) const
    {
        PlayerState &pl = state.players[player];
        pl.from = pl.coord;
        pl.target = Coord(targetX, targetY);
    }
};

struct AttackMove : public Move
{
    PlayerID victim;

    bool IsValid() const
    {
        return IsValidPlayerName(victim);
    }

    bool IsValid(const GameState &state) const
    {
        return state.players.count(player) != 0;
    }

    bool IsAttack(const GameState &state, PlayerID &outVictim) const
    {
        std::map<PlayerID, PlayerState>::const_iterator mi = state.players.find(victim);
        if (mi == state.players.end())
            return false;

        const PlayerState &p1 = state.players.find(player)->second;
        const PlayerState &p2 = mi->second;
        if (p1.color == p2.color)
            return false;
        if (distLInf(p1.coord, p2.coord) > 1)
            return false;

        // Victim is safe in the spawn area
        // TODO: should we restrict this rule to spawn area of the same color only?
        if (IsInSpawnArea(p2.coord))
            return false;

        outVictim = victim;
        return true;
    }
};



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

void MoveBase::ApplyCommon(GameState &state) const
{
    if (message)
    {
        state.players[player].message = *message;
        state.players[player].message_block = state.nHeight;
    }
    if (address)
        state.players[player].address = *address;
    if (addressLock)
        state.players[player].addressLock = *addressLock;
}

std::string MoveBase::AddressOperationPermission(const GameState &state) const
{
    if (!address && !addressLock)
        return std::string();      // No address operation requested - allow

    std::map<PlayerID, PlayerState>::const_iterator mi = state.players.find(player);
    if (mi == state.players.end())
        return std::string();      // Spawn move - allow any address operation

    return mi->second.addressLock;
}

/*static*/ Move *Move::Parse(const PlayerID &player, const std::string &json)
{
    if (!IsValidPlayerName(player))
        return NULL;

    using namespace json_spirit;
    Value v;
    if (!read_string(json, v) || v.type() != obj_type)
        return NULL;
    Object obj = v.get_obj();

    // Initialize common fields
    MoveBase move_base(player);
    if (ExtractField(obj, "message", v))
    {
        if (v.type() != str_type)
            return NULL;
        move_base.message = v.get_str();
    }
    if (ExtractField(obj, "address", v))
    {
        if (v.type() != str_type)
            return NULL;
        const std::string &addr = v.get_str();
        if (!addr.empty() && !IsValidBitcoinAddress(addr))
            return NULL;
        move_base.address = addr;
    }
    if (ExtractField(obj, "addressLock", v))
    {
        if (v.type() != str_type)
            return NULL;
        const std::string &addr = v.get_str();
        if (!addr.empty() && !IsValidBitcoinAddress(addr))
            return NULL;
        move_base.addressLock = addr;
    }

    // Create proper move type depending on the remaining (move-specific) fields

    Move *move;

    if (CheckJsonObject(obj, std::map<std::string, json_spirit::Value_type>()))
        move = new EmptyMove;
    else if (CheckJsonObject(obj, boost::assign::map_list_of("color", int_type)))
    {
        SpawnMove *m = new SpawnMove;
        m->color = find_value(obj, "color").get_int();
        move = m;
    }
    else if (CheckJsonObject(obj, boost::assign::map_list_of("x", int_type)("y", int_type)))
    {
        StepMove *m = new StepMove;
        m->targetX = find_value(obj, "x").get_int();
        m->targetY = find_value(obj, "y").get_int();
        move = m;
    }
    else if (CheckJsonObject(obj, boost::assign::map_list_of("attack", str_type)))
    {
        AttackMove *m = new AttackMove;
        m->victim = find_value(obj, "attack").get_str();
        move = m;
    }
    else
        return NULL;

    // Copy fields that are common to all moves
    *(MoveBase*)move = move_base;

    if (!move->IsValid())
    {
        delete move;
        return NULL;
    }

    return move;
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

PlayerState::PlayerState()
    : color(0),
    coord(0, 0), dir(0),
    from(0, 0), target(0, 0),
    stay_in_spawn_area(0),
    message_block(0)
{
}

// Simple straight-line motion
void PlayerState::MoveTowardsWaypoint()
{
    if (target == coord)
        return;

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
    }
}

std::vector<Coord> PlayerState::DumpPath() const
{
    std::vector<Coord> ret;
    PlayerState tmp = *this;
    while (tmp.target != tmp.coord)
    {
        ret.push_back(tmp.coord);
        tmp.MoveTowardsWaypoint();
    }
    if (!ret.empty())
        ret.push_back(tmp.target);
    return ret;
}

json_spirit::Value PlayerState::ToJsonValue() const
{
    using namespace json_spirit;

    Object obj;
    obj.push_back(Pair("color", (int)color));
    obj.push_back(Pair("x", coord.x));
    obj.push_back(Pair("y", coord.y));
    if (target != coord)
    {
        // Waypoint info
        obj.push_back(Pair("fromX", from.x));
        obj.push_back(Pair("fromY", from.y));
        obj.push_back(Pair("targetX", target.x));
        obj.push_back(Pair("targetY", target.y));
    }
    obj.push_back(Pair("dir", (int)dir));
    if (!message.empty())
    {
        obj.push_back(Pair("message", message));
        obj.push_back(Pair("message_block", message_block));
    }
    if (!address.empty())
        obj.push_back(Pair("address", address));
    if (!addressLock.empty())
        obj.push_back(Pair("addressLock", address));

    return obj;
}

GameState::GameState()
{
    nHeight = -1;
    hashBlock = 0;
}

json_spirit::Value GameState::ToJsonValue() const
{
    using namespace json_spirit;

    Object obj;

    Object subobj;
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, players)
        subobj.push_back(Pair(p.first, p.second.ToJsonValue()));
    obj.push_back(Pair("players", subobj));

    Array arr;
    BOOST_FOREACH(const PAIRTYPE(Coord, LootInfo) &p, loot)
    {
        Object subobj;
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

    obj.push_back(Pair("height", nHeight));
    obj.push_back(Pair("hashBlock", hashBlock.ToString().c_str()));

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
        const Coord &coord = p.second.coord;
        if (loot.count(coord) != 0)
        {
            std::map<Coord, int>::iterator mi = playersOnLootTile.find(coord);
            if (mi != playersOnLootTile.end())
                mi->second++;
            else
                playersOnLootTile.insert(std::make_pair(coord, 1));
        }
    }
    // Split equally, if multiple players on loot cell
    // If not divisible, the amounts are dependent on the order of players
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, players)
    {
        const Coord &coord = p.second.coord;
        std::map<Coord, int>::iterator mi = playersOnLootTile.find(coord);
        if (mi != playersOnLootTile.end())
        {
            LootInfo lootInfo = loot[coord];
            lootInfo.nAmount /= (mi->second--);

            // If amount was ~1e-8 and several players moved onto it, then some of them will get nothing
            if (lootInfo.nAmount > 0)
            {
                p.second.loot.Collect(lootInfo, nHeight);
                AddLoot(coord, -lootInfo.nAmount);
            }

            assert((mi->second == 0) == (loot.count(coord) == 0));   // If no more players on this tile, then all loot must be collected
        }
    }
}

std::vector<PlayerID> GameState::ListPossibleAttacks(const PlayerID &player) const
{
    std::vector<PlayerID> ret;

    AttackMove attack;
    attack.player = player;

    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, players)
    {
        attack.victim = p.first;
        PlayerID victim;
        if (attack.IsValid() && attack.IsValid(*this) && attack.IsAttack(*this, victim))
            ret.push_back(victim);
    }
    return ret;
}

StepData::~StepData()
{
    BOOST_FOREACH(const Move *m, vpMoves)
        delete m;
}

// Loot is pushed out from the spawn area to avoid some ambiguities with banking rules (as spawn areas are also banks)
// Note: the map must be constructed in such a way that there are no obstacles near spawn areas
Coord PushCoordOutOfSpawnArea(const Coord &c)
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

bool Game::PerformStep(const GameState &inState, const StepData &stepData, GameState &outState, StepResult &stepResult)
{
    BOOST_FOREACH(const Move *m, stepData.vpMoves)
        if (!m->IsValid(inState))
            return false;

    outState = inState;

    outState.nHeight = inState.nHeight + 1;
    outState.hashBlock = stepData.newHash;

    stepResult = StepResult();

    // Apply attacks
    BOOST_FOREACH(const Move *m, stepData.vpMoves)
    {
        PlayerID victim;
        if (m->IsAttack(inState, victim))
        {
            const PlayerState &victimState = inState.players.find(victim)->second;
            stepResult.killedPlayers.insert(victim);
            stepResult.killedBy.insert(std::make_pair(victim, m->player));

            outState.players[m->player].StopMoving();
        }
    }

    // Kill players who stay too long in the spawn area
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, outState.players)
        if (IsInSpawnArea(p.second.coord))
        {
            if (p.second.stay_in_spawn_area++ >= MAX_STAY_IN_SPAWN_AREA)
                stepResult.killedPlayers.insert(p.first);
        }
        else
            p.second.stay_in_spawn_area = 0;

    BOOST_FOREACH(const PlayerID &victim, stepResult.killedPlayers)
    {
        const PlayerState &victimState = inState.players.find(victim)->second;
        int64 nAmount = stepData.nNameCoinAmount + victimState.loot.nAmount;

        // If killed by the game for staying in the spawn area, then no tax
        if (stepResult.killedBy.count(victim) != 0)
        {
            // Tax from killing: 4%
            int64 nTax = nAmount / 25;
            stepResult.nTaxAmount += nTax;
            nAmount -= nTax;
        }
        outState.AddLoot(PushCoordOutOfSpawnArea(victimState.coord), nAmount);
    }

    // Apply updates to target coordinate
    BOOST_FOREACH(const Move *m, stepData.vpMoves)
        m->ApplyStep(outState);

    // Erase killed players
    BOOST_FOREACH(const PlayerID &victim, stepResult.killedPlayers)
        outState.players.erase(victim);

    // For all alive players perform path-finding
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, outState.players)
        p.second.MoveTowardsWaypoint();
        
    // Caution: banking must not depend on the randomized events, because they depend on the hash -
    // miners won't be able to compute tax amount if it depends on the hash.

    // Banking
    BOOST_FOREACH(PAIRTYPE(const PlayerID, PlayerState) &p, outState.players)
    {
        PlayerState &pl = p.second;
        if (pl.loot.nAmount > 0 && IsInSpawnArea(pl.coord))
        {
            // Tax from banking: 10%
            int64 nTax = pl.loot.nAmount / 10;
            stepResult.nTaxAmount += nTax;
            pl.loot.nAmount -= nTax;

            stepResult.bounties[p.first] = pl.loot;
            pl.loot = CollectedLootInfo();
        }
    }

    // Miners set hashBlock to 0 in order to compute tax and include it into the coinbase.
    // At this point the tax is fully computed, so we can return.
    if (outState.hashBlock == 0)
        return true;

    RandomGenerator rnd(outState.hashBlock);

    // Spawn new players
    BOOST_FOREACH(const Move *m, stepData.vpMoves)
        m->ApplySpawn(outState, rnd);

    // Apply address & message updates
    BOOST_FOREACH(const Move *m, stepData.vpMoves)
        m->ApplyCommon(outState);

    // Drop a random rewards onto the harvest areas
    int64 nTotalTreasure = 0;
    for (int i = 0; i < NUM_HARVEST_AREAS; i++)
    {
        Coord harvest;

        do
        {
            harvest.x = HarvestAreas[i].x + rnd.GetIntRnd(HarvestAreas[i].w);
            harvest.y = HarvestAreas[i].y + rnd.GetIntRnd(HarvestAreas[i].h);
        } while (!IsWalkable(harvest));
        int64 nTreasure = HarvestAreas[i].fraction * stepData.nTreasureAmount / TOTAL_HARVEST;
        outState.AddLoot(harvest, nTreasure);
        nTotalTreasure += nTreasure;
    }
    assert(nTotalTreasure == stepData.nTreasureAmount);

    // Players collect loot
    outState.DivideLootAmongPlayers();

    return true;
}
