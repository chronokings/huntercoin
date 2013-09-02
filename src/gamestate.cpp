#include "gamestate.h"

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
        return color == 0 || color == 1;
    }

    bool IsValid(const GameState &state) const
    {
        return state.players.count(player) == 0;
    }

    void ApplySpawn(GameState &state) const
    {
        PlayerState newPlayer;
        newPlayer.color = color;
        newPlayer.coord = Coord(0, 0);
        bool ok;
        // Find a cell surrounded by empty cells, in the row 0
        do
        {
            ok = true;
            BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, state.players)
            {
                if (distLInf(p.second.coord, newPlayer.coord) <= 1)
                {
                    newPlayer.coord.x++;
                    ok = false;
                }
            }
        } while (!ok);
        state.players[player] = newPlayer;
    }
};

struct StepMove : public Move
{
    int deltaX, deltaY;

    bool IsValid() const
    {
        // Allow move by 1 tile vertically or horizontally. Empty move also allowed.
        return
            deltaX >= -1 && deltaY >= -1          // Prevent abs overflow on INT_MIN
            && deltaX <= 1 && deltaY <= 1         // Prevent addition overflow
            && abs(deltaX) + abs(deltaY) <= 1;
    }

    bool IsValid(const GameState &state) const
    {
        return state.players.count(player) != 0;
    }

    void ApplyStep(GameState &state) const
    {
        state.players[player].coord.x += deltaX;
        state.players[player].coord.y += deltaY;
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
        std::map<PlayerID, PlayerState>::const_iterator mi1 = state.players.find(player);
        std::map<PlayerID, PlayerState>::const_iterator mi2 = state.players.find(victim);
        if (mi1 == state.players.end() || mi2 == state.players.end() || mi1 == mi2)
            return false;

        // The following line allows restricting killings to opposite teams. It can be moved to IsAttack.
        //return mi1->second.color != mi2->second.color;
        return true;
    }

    bool IsAttack(const GameState &state, PlayerID &outVictim) const
    {
        const PlayerState &p1 = state.players.find(player)->second;
        const PlayerState &p2 = state.players.find(victim)->second;
        if (distL1(p1.coord, p2.coord) <= 1)
        {
            outVictim = victim;
            return true;
        }
        return false;
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

    // Create proper move type depending on the remaining (move-specific) fields

    Move *move;

    if (CheckJsonObject(obj, std::map<std::string, json_spirit::Value_type>()))
        move = new EmptyMove;
    else if (CheckJsonObject(obj, boost::assign::map_list_of("color", int_type)))
    {
        SpawnMove *m = new SpawnMove;
        m->color = find_value(obj, "color").get_value<int>();
        move = m;
    }
    else if (CheckJsonObject(obj, boost::assign::map_list_of("deltaX", int_type)("deltaY", int_type)))
    {
        StepMove *m = new StepMove;
        m->deltaX = find_value(obj, "deltaX").get_value<int>();
        m->deltaY = find_value(obj, "deltaY").get_value<int>();
        move = m;
    }
    else if (CheckJsonObject(obj, boost::assign::map_list_of("attack", str_type)))
    {
        AttackMove *m = new AttackMove;
        m->victim = find_value(obj, "attack").get_str();
        move = m;
    }

    // Copy fields that are common to all moves
    *(MoveBase*)move = move_base;

    if (!move->IsValid())
    {
        delete move;
        return NULL;
    }

    return move;
}

PlayerState::PlayerState()
    : color(0),
    coord(0, 0),
    message_block(0)
{
}

json_spirit::Value PlayerState::ToJsonValue() const
{
    using namespace json_spirit;

    Object obj;
    obj.push_back(Pair("color", color));
    obj.push_back(Pair("x", coord.x));
    obj.push_back(Pair("y", coord.y));
    if (!message.empty())
    {
        obj.push_back(Pair("message", message));
        obj.push_back(Pair("message_block", message_block));
    }
    if (!address.empty())
        obj.push_back(Pair("address", address));

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

void GameState::DivideLootAmongPlayers(std::map<PlayerID, BountyInfo> &outBounties)
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
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, players)
    {
        const Coord &coord = p.second.coord;
        std::map<Coord, int>::iterator mi = playersOnLootTile.find(coord);
        if (mi != playersOnLootTile.end())
        {
            const LootInfo &lootInfo = loot[coord];
            int64 nAmount = lootInfo.nAmount / (mi->second--);
            if (nAmount > 0)   // If amount was ~1e-8 and several players moved onto it, then some of them will get nothing
            {
                outBounties[p.first].Add(coord, lootInfo, nAmount);
                AddLoot(coord, -nAmount);
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
        if (attack.IsValid(*this) && attack.IsAttack(*this, victim))
            ret.push_back(victim);
    }
}

StepData::~StepData()
{
    BOOST_FOREACH(const Move *m, vpMoves)
        delete m;
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

    BOOST_FOREACH(const Move *m, stepData.vpMoves)
        m->ApplySpawn(outState);
        
    BOOST_FOREACH(const Move *m, stepData.vpMoves)
        m->ApplyCommon(outState);

    BOOST_FOREACH(const Move *m, stepData.vpMoves)
    {
        PlayerID victim;
        if (m->IsAttack(inState, victim))
        {
            outState.players.erase(victim);
            const PlayerState &victimState = inState.players.find(victim)->second;
            outState.AddLoot(victimState.coord, stepData.nNameCoinAmount);
            stepResult.killedPlayers.insert(victim);
            stepResult.killedBy.insert(std::make_pair(victim, m->player));
        }
    }

    BOOST_FOREACH(const Move *m, stepData.vpMoves)
    {
        if (outState.players.count(m->player) != 0)  // Skip players that have just been killed
            m->ApplyStep(outState);
    }

    // Random reward placed on the map inside NxN square growing with each block, centered at (0, 0).
    // It can coincide with some player, who'll collect it immediately.
    // This is done to test that outBounties do not affect the block hash.
    CBigNum rnd(SerializeHash(outState.hashBlock, SER_GETHASH, 0));
    int n = 2 * outState.nHeight + 1;
    int x = (rnd % n).getint() - (n / 2);
    int y = ((rnd / n) % n).getint() - (n / 2);
    outState.AddLoot(Coord(x, y), stepData.nTreasureAmount);

    outState.DivideLootAmongPlayers(stepResult.bounties);

    return true;
}
