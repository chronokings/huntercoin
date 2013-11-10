#ifndef GAMEPATHFINDER_H
#define GAMEPATHFINDER_H

// Advanced client-side path finding that computes shortest path

#include "../gamestate.h"
#include <deque>

class GamePathfinder
{
public:
    bool FindPath(const Game::Coord &start, const Game::Coord &goal);
    Game::Coord GetCurWaypoint() const { return waypoints[0]; }
    bool GetNextWaypoint(Game::Coord &c);
    std::vector<Game::Coord> DumpPath() const;
    
private:
    // waypoints[0] is previous (visited) waypoint
    // waypoints[1] is the next waypoint (current target)
    std::deque<Game::Coord> waypoints;

    bool CheckLinearPath(const Game::Coord &start, const Game::Coord &target);
};

typedef std::map<Game::PlayerID, GamePathfinder> GamePathfinders;

#endif // GAMEPATHFINDER_H
