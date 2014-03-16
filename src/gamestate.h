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

static const int NUM_TEAM_COLORS = 4;
static const int MAX_WAYPOINTS = 100;                      // Maximum number of waypoints per character
static const unsigned char MAX_STAY_IN_SPAWN_AREA = 30;
static const int DESTRUCT_RADIUS = 1;
static const int DESTRUCT_RADIUS_MAIN = 2;                 // Destruction radius for main character
static const int NUM_INITIAL_CHARACTERS = 3;               // Initial number of characters to spawn for new player (includes main character)
static const int MAX_CHARACTERS_PER_PLAYER = 20;           // Maximum number of characters per player at the same time
static const int MAX_CHARACTERS_PER_PLAYER_TOTAL = 1000;   // Maximum number of characters per player in the lifetime
static const int HEART_EVERY_NTH_BLOCK = 10;               // Spawn rate of hearts

// Unique player name
typedef std::string PlayerID;

// Player name + character index
struct CharacterID
{
    PlayerID player;
    int index;
    
    CharacterID() : index(-1) { }
    CharacterID(const PlayerID &player_, int index_)
        : player(player_), index(index_)
    {
        if (index_ < 0)
            throw std::runtime_error("Bad character index");
    }

    std::string ToString() const;

    static CharacterID Parse(const std::string &s)
    {
        size_t pos = s.find('.');
        if (pos == std::string::npos)
            return CharacterID(s, 0);
        return CharacterID(s.substr(0, pos), atoi(s.substr(pos + 1).c_str()));
    }

    bool operator==(const CharacterID &that) const { return player == that.player && index == that.index; }
    bool operator!=(const CharacterID &that) const { return !(*this == that); }
    // Lexicographical comparison
    bool operator<(const CharacterID &that) const { return player < that.player || (player == that.player && index < that.index); }
    bool operator>(const CharacterID &that) const { return that < *this; }
    bool operator<=(const CharacterID &that) const { return !(*this > that); }
    bool operator>=(const CharacterID &that) const { return !(*this < that); }
};

class GameState;
class RandomGenerator;

struct Coord
{
    int x, y;

    Coord() : x(0), y(0) { }
    Coord(int x_, int y_) : x(x_), y(y_) { }

    unsigned int GetSerializeSize(int = 0, int = VERSION) const
    {
        return sizeof(int) * 2;
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

typedef std::vector<Coord> WaypointVector;

struct Move
{
    PlayerID player;

    // Updates to the player state
    boost::optional<std::string> message;
    boost::optional<std::string> address;
    boost::optional<std::string> addressLock;

    unsigned char color;   // For spawn move
    std::map<int, WaypointVector> waypoints;
    std::set<int> destruct;

    Move() : color(0xFF)
    {
    }

    std::string AddressOperationPermission(const GameState &state) const;

    bool IsSpawn() const { return color != 0xFF; }
    bool IsValid(const GameState &state) const;
    void ApplyCommon(GameState &state) const;
    void ApplySpawn(GameState &state, RandomGenerator &rnd) const;
    void ApplyWaypoints(GameState &state) const;
    bool IsAttack(const GameState &state, int character_index) const;
 
    // Move must be empty before Parse and cannot be reused after Parse
    bool Parse(const PlayerID &player, const std::string &json);

    // Returns true if move is initialized (i.e. was parsed successfully)
    operator bool() { return !player.empty(); }
};

// Do not use for user-provided coordinates, as abs can overflow on INT_MIN.
// Use for algorithmically-computed coordinates that guaranteedly lie within the game map.
inline int distLInf(const Coord &c1, const Coord &c2)
{
    return std::max(abs(c1.x - c2.x), abs(c1.y - c2.y));
}

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

struct CharacterState
{
    Coord coord;                        // Current coordinate
    unsigned char dir;                  // Direction of last move (for nice sprite orientation). Encoding: as on numeric keypad.
    Coord from;                         // Straight-line pathfinding for current waypoint
    WaypointVector waypoints;           // Waypoints (stored in reverse so removal of the first waypoint is fast)
    CollectedLootInfo loot;             // Loot collected by player but not banked yet
    unsigned char stay_in_spawn_area;   // Auto-kill players who stay in the spawn area too long
    std::string attack;

    CharacterState() : coord(0, 0), dir(0), from(0, 0), stay_in_spawn_area(0) { }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(coord);
        READWRITE(dir);
        READWRITE(from);
        READWRITE(waypoints);
        READWRITE(loot);
        READWRITE(stay_in_spawn_area);
        READWRITE(attack);
    )

    void Spawn(int color, RandomGenerator &rnd);

    void StopMoving()
    {
        from = coord;
        waypoints.clear();
    }

    void MoveTowardsWaypoint();
    WaypointVector DumpPath(const WaypointVector *alternative_waypoints = NULL) const;

    /**
     * Calculate total length (in the same L-infinity sense that gives the
     * actual movement time) of the outstanding path.
     * @param altWP Optionally provide alternative waypoints (for queued moves).
     * @return Time necessary to finish current path in blocks.
     */
    unsigned TimeToDestination(const WaypointVector *altWP = NULL) const;

    json_spirit::Value ToJsonValue(bool has_crown) const;
};

struct PlayerState
{
    unsigned char color;                        // Color represents player team

    std::map<int, CharacterState> characters;   // Characters owned by the player (0 is the main character)
    int next_character_index;                   // Index of the next spawned character

    std::string message;      // Last message, can be shown as speech bubble
    int message_block;        // Block number. Game visualizer can hide messages that are too old
    std::string address;      // Address for receiving rewards. Empty means receive to the name address
    std::string addressLock;  // "Admin" address for player - reward address field can only be changed, if player is transferred to addressLock

    IMPLEMENT_SERIALIZE
    (
        READWRITE(color);
        READWRITE(characters);
        READWRITE(next_character_index);
        READWRITE(message);
        READWRITE(message_block);
        READWRITE(address);
        READWRITE(addressLock);
    )

    PlayerState() : color(0xFF), next_character_index(0), message_block(0) { }
    void SpawnCharacter(RandomGenerator &rnd);
    bool CanSpawnCharacter()
    {
        return characters.size() < MAX_CHARACTERS_PER_PLAYER && next_character_index < MAX_CHARACTERS_PER_PLAYER_TOTAL;
    }
    json_spirit::Value ToJsonValue(int crown_index, bool dead = false) const;
};

struct GameState
{
    GameState();

    // Memory-only version. Is not read/written. Used by UpgradeGameDB.
    int nVersion;

    // Player states
    std::map<PlayerID, PlayerState> players;

    // Last chat messages of dead players (only in the current block)
    // Minimum info is stored: color, message, message_block.
    // When converting to JSON, this array is concatenated with normal players.
    std::map<PlayerID, PlayerState> dead_players_chat;

    std::map<Coord, LootInfo> loot;
    std::set<Coord> hearts;
    Coord crownPos;
    CharacterID crownHolder;

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
        if (this->nVersion >= 1000500)
            READWRITE(dead_players_chat);
        else if (fRead)
            (const_cast<std::map<PlayerID, PlayerState>&>(dead_players_chat)).clear();
        READWRITE(loot);
        READWRITE(hearts);
        READWRITE(crownPos);
        READWRITE(crownHolder.player);
        if (!crownHolder.player.empty())
            READWRITE(crownHolder.index);
        READWRITE(nHeight);
        READWRITE(hashBlock);
    )

    void UpdateVersion();

    json_spirit::Value ToJsonValue() const;

    // Helper functions
    void AddLoot(Coord coord, int64 nAmount);
    void DivideLootAmongPlayers();
    void CollectHearts(RandomGenerator &rnd);
    void UpdateCrownState(bool &respawn_crown);
    void CollectCrown(RandomGenerator &rnd, bool respawn_crown);
    void CrownBonus(int64 nAmount);
};

struct StepData : boost::noncopyable
{
    int64 nNameCoinAmount, nTreasureAmount;
    uint256 newHash;
    std::vector<Move> vMoves;
};

struct StepResult
{
    StepResult() : nTaxAmount(0) { }

    std::map<CharacterID, CollectedLootInfo> bounties;

    // The following arrays only contain killed players (i.e. the main character)
    std::set<PlayerID> killedPlayers;
    std::multimap<PlayerID, CharacterID> killedBy;

    int64 nTaxAmount;
};

// All moves happen simultaneously, so this function must work identically
// for any ordering of the moves, except non-critical cases (e.g. finding
// an empty cell to spawn new player)
bool PerformStep(const GameState &inState, const StepData &stepData, GameState &outState, StepResult &stepResult);

}

#endif
