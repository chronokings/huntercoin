#ifndef GAME_H
#define GAME_H

#include <string>
#include <boost/noncopyable.hpp>
#include "json/json_spirit_value.h"
#include "serialize.h"
#include "uint256.h"

namespace Game
{

// Unique player name
typedef std::string PlayerID;

class GameState;

struct MoveBase
{
    PlayerID player;

    // Updates to the player state (can be included in any move)
    std::string message;
    bool has_message;     // This move updates player's message

    std::string address;
    bool has_address;     // This move updates player's address

    void ApplyCommon(GameState &state) const;

    MoveBase();
    MoveBase(const PlayerID &player_);
};

struct Move : public MoveBase
{
    virtual bool IsValid() const = 0;
    virtual bool IsValid(const GameState &state) const = 0;

    // Virtual functions for specific types of moves
    // For a more general case, visitor pattern would be better
    virtual void ApplySpawn(GameState &state) const { }
    virtual void ApplyStep(GameState &state) const { }
    virtual bool IsAttack(const GameState &state, PlayerID &outVictim) const { return false; }

    static Move *Parse(const PlayerID &player, const std::string &json);
};

struct PlayerState
{
    int color;
    int x, y;

    std::string message;   // Last message, can be shown as speech bubble
    int message_block;     // Block number. Game visualizer can hide messages that are too old
    std::string address;   // Address for receiving rewards. Empty means receive to the name address

    IMPLEMENT_SERIALIZE
    (
        READWRITE(color);
        READWRITE(x);
        READWRITE(y);
        READWRITE(message);
        READWRITE(message_block);
        READWRITE(address);
    )

    PlayerState();
    json_spirit::Value ToJsonValue() const;
};

struct GameState
{
    GameState();

    // Player states
    std::map<PlayerID, PlayerState> players;

    // Rewards placed on the map
    std::map<std::pair<int, int>, int64> loot;

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
    void AddLoot(int x, int y, int64 nAmount);
    void DivideLootAmongPlayers(std::map<PlayerID, int64> &outBounties);
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
    std::map<PlayerID, int64> bounties;
    std::set<PlayerID> killedPlayers;
};

// All moves happen simultaneously, so this function must work identically
// for any ordering of the moves, except non-critical cases (e.g. finding
// an empty cell to spawn new player)
bool PerformStep(const GameState &inState, const StepData &stepData, GameState &outState, StepResult &stepResult);

}

#endif
