#include "gamestate.h"

#include <cstdarg>
#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"
#include <boost/xpressive/xpressive_dynamic.hpp>

#include "headers.h"
#include "chronokings.h"   // For NAME_COIN_AMOUNT

using namespace Game;

static bool CheckJsonObject(const json_spirit::Object &obj, const char *field_name, ...)
{
    using namespace json_spirit;

    va_list arg_ptr;
    va_start(arg_ptr, field_name);
    Value_type field_type;
    int n = 0;
    while (field_name)
    {
        field_type = (Value_type)va_arg(arg_ptr, int);

        bool fAllowNull = false;
        if (field_name[0] == '?')
        {
            fAllowNull = true;
            field_name++;
        }

        Value v = find_value(obj, field_name);
        if (v.type() == field_type)
            n++;
        else
        {
            if (!(v.type() == null_type && fAllowNull))
                return false;
        }

        field_name = va_arg(arg_ptr, const char *);
    }
    va_end(arg_ptr);
    return obj.size() == n;    // Check that there are no extra fields in obj
}

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

    if (CheckJsonObject(obj, NULL))
        move = new EmptyMove;
    else if (CheckJsonObject(obj, "color", int_type, NULL))
    {
        SpawnMove *m = new SpawnMove;
        m->color = find_value(obj, "color").get_value<int>();
        move = m;
    }
    else if (CheckJsonObject(obj, "deltaX", int_type, "deltaY", int_type, NULL))
    {
        StepMove *m = new StepMove;
        m->deltaX = find_value(obj, "deltaX").get_value<int>();
        m->deltaY = find_value(obj, "deltaY").get_value<int>();
        move = m;
    }
    else if (CheckJsonObject(obj, "attack", str_type, NULL))
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

bool EmptyMove::IsValid(const GameState &state) const
{
    return state.players.count(player) != 0;
}

bool SpawnMove::IsValid(const GameState &state) const
{
    return state.players.count(player) == 0;
}

void SpawnMove::ApplySpawn(GameState &state) const
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

bool StepMove::IsValid(const GameState &state) const
{
    return state.players.count(player) != 0;
}

void StepMove::ApplyStep(GameState &state) const
{
    state.players[player].x += deltaX;
    state.players[player].y += deltaY;
}

bool AttackMove::IsValid() const
{
    return IsValidPlayerName(victim);
}

bool AttackMove::IsValid(const GameState &state) const
{
    return state.players.count(player) != 0 && state.players.count(victim) != 0;
}

bool AttackMove::IsAttack(const GameState &state, PlayerID &outVictim) const
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

GameState::GameState()
{
    nHeight = -1;
    hashBlock = 0;
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

void GameState::DivideLootAmoungPlayers(std::map<PlayerID, int64> &outBounties)
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
                loot.insert(std::make_pair(xy, 1));
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
        }
    }
}

bool PerformStep(const GameState &inState, const std::vector<Move*> &vpMoves, GameState &outState, uint256 newHash, std::map<PlayerID, int64> &outBounties)
{
    BOOST_FOREACH(const Move *m, vpMoves)
        if (!m->IsValid(inState))
            return false;

    outState = inState;
    outBounties.clear();

    BOOST_FOREACH(const Move *m, vpMoves)
        m->ApplySpawn(outState);

    BOOST_FOREACH(const Move *m, vpMoves)
    {
        PlayerID victim;
        if (m->IsAttack(inState, victim))
        {
            outState.players.erase(victim);
            const PlayerState &victimState = inState.players.find(victim)->second;
            outState.AddLoot(victimState.x, victimState.y, NAME_COIN_AMOUNT);
        }
    }

    BOOST_FOREACH(const Move *m, vpMoves)
    {
        if (outState.players.count(m->player) != 0)  // Skip players that have just been killed
            m->ApplyStep(outState);
    }

    outState.DivideLootAmoungPlayers(outBounties);

    outState.hashBlock = newHash;
    outState.nHeight = inState.nHeight + 1;

    return true;
}

