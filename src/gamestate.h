#ifndef GAMESTATE_H
#define GAMESTATE_H

#include <string>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include "json/json_spirit_value.h"
#include "uint256.h"
#include "serialize.h"

namespace Game
{

// Unique player name
typedef std::string PlayerID;

static const int NUM_TEAM_COLORS = 4;

class GameState;

// MoveBase contains common data for each type of move.
// Do not instantiate directly, use Move::Parse instead
struct MoveBase
{
    PlayerID player;

    // Updates to the player state
    boost::optional<std::string> message;
    boost::optional<std::string> address;
    boost::optional<std::string> addressLock;

    void ApplyCommon(GameState &state) const;

    MoveBase() { }
    MoveBase(const PlayerID &player_) : player(player_) { }

    std::string AddressOperationPermission(const GameState &state) const;
};

struct Move : public MoveBase
{
    virtual bool IsValid() const = 0;
    virtual bool IsValid(const GameState &state) const = 0;

    // Virtual functions for specific types of moves
    // For a more general case, visitor pattern would be better
    virtual void ApplySpawn(GameState &state, class RandomGenerator &rnd) const { }
    virtual void ApplyStep(GameState &state) const { }
    virtual bool IsAttack(const GameState &state, PlayerID &outVictim) const { return false; }

    static Move *Parse(const PlayerID &player, const std::string &json);
};

struct Coord
{
    int x, y;

    Coord() : x(0), y(0) { }
    Coord(int x_, int y_) : x(x_), y(y_) { }

    unsigned int GetSerializeSize(int = 0, int = VERSION) const
    {
        if (x >= 0 && x < 255 && y >= 0 && y < 255)
            return sizeof(unsigned char) * 2;
        else
            return sizeof(unsigned char) + sizeof(int) * 2;
    }

    template<typename Stream>
    void Serialize(Stream& s, int = 0, int = VERSION) const
    {
        WRITEDATA(s, x);
        WRITEDATA(s, y);
    }

    template<typename Stream>
    void Unserialize(Stream& s, int = 0, int = VERSION)
    {
        READDATA(s, x);
        READDATA(s, y);
    }

    bool operator==(const Coord &that) const { return x == that.x && y == that.y; }
    bool operator!=(const Coord &that) const { return !(*this == that); }
    // Lexicographical comparison
    bool operator<(const Coord &that) const { return y < that.y || (y == that.y && x < that.x); }
    bool operator>(const Coord &that) const { return that < *this; }
    bool operator<=(const Coord &that) const { return !(*this > that); }
    bool operator>=(const Coord &that) const { return !(*this < that); }
};

// Do not use for user-provided coordinates, as abs can overflow on INT_MIN.
// Use for algorithmically-computed coordinates that guaranteedly lie within the game map.
inline int distLInf(const Coord &c1, const Coord &c2)
{
    return std::max(abs(c1.x - c2.x), abs(c1.y - c2.y));
}

static const unsigned char MAX_STAY_IN_SPAWN_AREA = 30;

struct LootInfo
{
    int64 nAmount;
    // Time span over the which this loot accumulated
    // This is merely for informative purposes, plus to make
    // hash of the loot tx unique
    int firstBlock, lastBlock;

    LootInfo() : nAmount(0), firstBlock(-1), lastBlock(-1) { }
    LootInfo(int64 nAmount_, int nHeight) : nAmount(nAmount_), firstBlock(nHeight), lastBlock(nHeight) { }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(nAmount);
        READWRITE(firstBlock);
        READWRITE(lastBlock);
    )
};

struct CollectedLootInfo : public LootInfo
{
    // Time span over which the loot was collected
    int collectedFirstBlock, collectedLastBlock;
    
    CollectedLootInfo() : LootInfo(), collectedFirstBlock(-1), collectedLastBlock(-1) { }

    void Collect(const LootInfo &loot, int nHeight)
    {
        if (loot.nAmount <= 0)
            return;

        nAmount += loot.nAmount;

        if (firstBlock < 0 || loot.firstBlock < firstBlock)
            firstBlock = loot.firstBlock;
        if (loot.lastBlock > lastBlock)
            lastBlock = loot.lastBlock;

        if (collectedFirstBlock < 0)
            collectedFirstBlock = nHeight;
        collectedLastBlock = nHeight;
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(*(LootInfo*)this);
        READWRITE(collectedFirstBlock);
        READWRITE(collectedLastBlock);
    )
};

struct PlayerState
{
    unsigned char color;                // Color represents player team
    Coord coord;                        // Current coordinate
    unsigned char dir;                  // Direction of last move (for nice sprite orientation). Encoding: as on numeric keypad.
    Coord from, target;                 // Straight-line pathfinding (waypoint)
    CollectedLootInfo loot;             // Loot collected by player but not banked yet
    unsigned char stay_in_spawn_area;   // Auto-kill players who stay in the spawn area too long

    std::string message;      // Last message, can be shown as speech bubble
    int message_block;        // Block number. Game visualizer can hide messages that are too old
    std::string address;      // Address for receiving rewards. Empty means receive to the name address
    std::string addressLock;  // "Admin" address for player - reward address field can only be changed, if player is transferred to addressLock

    IMPLEMENT_SERIALIZE
    (
        READWRITE(color);
        READWRITE(coord);
        READWRITE(dir);
        READWRITE(from);
        READWRITE(target);
        READWRITE(loot);
        READWRITE(stay_in_spawn_area);
        READWRITE(message);
        READWRITE(message_block);
        READWRITE(address);
        READWRITE(addressLock);
    )

    PlayerState();
    void StopMoving() { from = target = coord; }
    void MoveTowardsWaypoint();
    std::vector<Coord> DumpPath() const;
    json_spirit::Value ToJsonValue() const;
};

struct GameState
{
    GameState();

    // Player states
    std::map<PlayerID, PlayerState> players;

    std::map<Coord, LootInfo> loot;

    // Number of steps since the game start.
    // State with nHeight==i includes moves from i-th block
    // -1 = initial game state (before genesis block)
    // 0  = game state immediately after the genesis block
    int nHeight;

    // Hash of the last block, moves from which were included
    // into this game state. This is meta-information (i.e. used
    // mainly for managing game states rather than as part of game
    // state, though it can be used as a random seed)
    uint256 hashBlock;
    
    IMPLEMENT_SERIALIZE
    (
        READWRITE(players);
        READWRITE(loot);
        READWRITE(nHeight);
        READWRITE(hashBlock);
    )

    json_spirit::Value ToJsonValue() const;

    // Helper functions
    void AddLoot(Coord coord, int64 nAmount);
    void DivideLootAmongPlayers();
    std::vector<PlayerID> ListPossibleAttacks(const PlayerID &player) const;
};

struct StepData : boost::noncopyable
{
    int64 nNameCoinAmount, nTreasureAmount;
    uint256 newHash;
    std::vector<const Move*> vpMoves;

    ~StepData();
};

struct StepResult
{
    StepResult() : nTaxAmount(0) { }

    std::map<PlayerID, CollectedLootInfo> bounties;
    std::set<PlayerID> killedPlayers;
    std::multimap<PlayerID, PlayerID> killedBy;
    int64 nTaxAmount;
};

// All moves happen simultaneously, so this function must work identically
// for any ordering of the moves, except non-critical cases (e.g. finding
// an empty cell to spawn new player)
bool PerformStep(const GameState &inState, const StepData &stepData, GameState &outState, StepResult &stepResult);

}

#endif
