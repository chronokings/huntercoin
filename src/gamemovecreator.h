#ifndef GAMEMOVECREATOR_H
#define GAMEMOVECREATOR_H

// Advanced client-side path finding that computes shortest path

#include "gamestate.h"

#include <vector>

std::vector<Game::Coord> FindPath(const Game::Coord &start, const Game::Coord &goal);

struct QueuedMove
{
    Game::WaypointVector waypoints;
    bool destruct;

    QueuedMove() : destruct(false) { }
};

typedef std::map<int, QueuedMove> QueuedPlayerMoves;
typedef std::map<Game::PlayerID, QueuedPlayerMoves> QueuedMoves;
std::vector<Game::Coord> *UpdateQueuedPath(const Game::CharacterState &ch, QueuedMoves &queuedMoves, const Game::CharacterID &chid);
std::vector<Game::Coord> PathToCharacterWaypoints(const std::vector<Game::Coord> &path);

#endif // GAMEMOVECREATOR_H
