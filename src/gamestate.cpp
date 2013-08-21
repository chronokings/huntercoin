#include "gamestate.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>

#include "util.h"
#include "bignum.h"

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
        newPlayer.x = 0;
        newPlayer.y = 0;
        bool ok;
        // Find a cell surrounded by empty cells, in the row 0
        do
        {
            ok = true;
            BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, state.players)
            {
                if (abs(p.second.x - newPlayer.x) <= 1 && abs(p.second.y - newPlayer.y) <= 1)
                {
                    newPlayer.x++;
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
        return abs(deltaX) + abs(deltaY) <= 1;
    }

    bool IsValid(const GameState &state) const
    {
        return state.players.count(player) != 0;
    }

    void ApplyStep(GameState &state) const
    {
        state.players[player].x += deltaX;
        state.players[player].y += deltaY;
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
        return mi1->second.color != mi2->second.color;
    }

    bool IsAttack(const GameState &state, PlayerID &outVictim) const
    {
        const PlayerState &p1 = state.players.find(player)->second;
        const PlayerState &p2 = state.players.find(victim)->second;
        if (abs(p1.x - p2.x) + abs(p1.y - p2.y) <= 1)
        {
            outVictim = victim;
            return true;
        }
        return false;
    }
};



/*static*/ Move *Move::Parse(const PlayerID &player, const std::string &json)
{
    if (!IsValidPlayerName(player))
        return NULL;

    using namespace json_spirit;
    Value v;
    if (!read_string(json, v) || v.type() != obj_type)
        return NULL;
    Object obj = v.get_obj();

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

    move->player = player;

    if (!move->IsValid())
    {
        delete move;
        return NULL;
    }

    return move;
}

json_spirit::Value PlayerState::ToJsonValue() const
{
    using namespace json_spirit;

    Object obj;
    obj.push_back(Pair("color", color));
    obj.push_back(Pair("x", x));
    obj.push_back(Pair("y", y));

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
    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int, int), uint64) &p, loot)
    {
        Object subobj;
        subobj.push_back(Pair("x", p.first.first));
        subobj.push_back(Pair("y", p.first.second));
        subobj.push_back(Pair("amount", ValueFromAmount(p.second)));
        arr.push_back(subobj);
    }
    obj.push_back(Pair("loot", arr));

    obj.push_back(Pair("height", nHeight));
    obj.push_back(Pair("hashBlock", hashBlock.ToString().c_str()));

    return obj;
}

void GameState::AddLoot(int x, int y, int64 nAmount)
{
    if (nAmount == 0)
        return;
    std::pair<int, int> xy(x, y);
    std::map<std::pair<int, int>, int64>::iterator mi = loot.find(xy);
    if (mi != loot.end())
    {
        if ((mi->second += nAmount) == 0)
            loot.erase(mi);
    }
    else
        loot.insert(std::make_pair(xy, nAmount));
}

void GameState::DivideLootAmongPlayers(std::map<PlayerID, int64> &outBounties)
{
    std::map<std::pair<int, int>, int> playersOnLootTile;
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, players)
    {
        std::pair<int, int> xy(p.second.x, p.second.y);
        if (loot.count(xy) != 0)
        {
            std::map<std::pair<int, int>, int>::iterator mi = playersOnLootTile.find(xy);
            if (mi != playersOnLootTile.end())
                mi->second++;
            else
                playersOnLootTile.insert(std::make_pair(xy, 1));
        }
    }
    // Split equally, if multiple players on loot cell
    // If not divisible, the amounts are dependent on the order of players
    BOOST_FOREACH(const PAIRTYPE(PlayerID, PlayerState) &p, players)
    {
        std::pair<int, int> xy(p.second.x, p.second.y);
        std::map<std::pair<int, int>, int>::iterator mi = playersOnLootTile.find(xy);
        if (mi != playersOnLootTile.end())
        {
            int64 nAmount = loot[xy] / (mi->second--);
            outBounties[p.first] += nAmount;
            AddLoot(xy.first, xy.second, -nAmount);
            if (mi->second == 0)
                assert(loot.count(xy) == 0);
        }
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
    stepResult = StepResult();

    BOOST_FOREACH(const Move *m, stepData.vpMoves)
        m->ApplySpawn(outState);

    BOOST_FOREACH(const Move *m, stepData.vpMoves)
    {
        PlayerID victim;
        if (m->IsAttack(inState, victim))
        {
            outState.players.erase(victim);
            const PlayerState &victimState = inState.players.find(victim)->second;
            outState.AddLoot(victimState.x, victimState.y, stepData.nNameCoinAmount);
            stepResult.killedPlayers.insert(victim);
        }
    }

    BOOST_FOREACH(const Move *m, stepData.vpMoves)
    {
        if (outState.players.count(m->player) != 0)  // Skip players that have just been killed
            m->ApplyStep(outState);
    }

    outState.nHeight = inState.nHeight + 1;
    outState.hashBlock = stepData.newHash;

    // Random reward placed on the map inside NxN square growing with each block, centered at (0, 0).
    // It can coincide with some player, who'll collect it immediately.
    // This is done to test that outBounties do not affect the block hash.
    CBigNum rnd(SerializeHash(outState.hashBlock, SER_GETHASH, 0));
    int n = 2 * outState.nHeight + 1;
    outState.AddLoot(
            (rnd % n).getint() - (n / 2),
            ((rnd / n) % n).getint() - (n / 2),
            stepData.nTreasureAmount);

    // TODO: it could be good to attach description to each bounty
    // and then copy it to e.g. strFromAccount to show up in the wallet,
    // or when serializing vgametx

    outState.DivideLootAmongPlayers(stepResult.bounties);

    return true;
}
