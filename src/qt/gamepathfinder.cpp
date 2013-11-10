#include "gamepathfinder.h"
#include "../gamemap.h"

#include <boost/graph/astar_search.hpp>
#include <boost/graph/grid_graph.hpp>
#include <boost/unordered_map.hpp>

using namespace Game;

struct neighbor_iterator;

// Model of:
//  * Graph
//  * IncidenceGraph
struct Maze
{
    // Graph concept requirements
    typedef Coord                             vertex_descriptor;
    typedef std::pair<Coord, Coord>           edge_descriptor;
    typedef boost::undirected_tag             directed_category;
    typedef boost::disallow_parallel_edge_tag edge_parallel_category;
    typedef boost::incidence_graph_tag        traversal_category;

    // IncidenceGraph concept requirements
    typedef neighbor_iterator          out_edge_iterator;
    typedef int                        degree_size_type;
};

namespace boost
{
    template <> struct graph_traits<Maze>
    {
        typedef Maze G;

        typedef G::vertex_descriptor      vertex_descriptor;
        typedef G::edge_descriptor        edge_descriptor;
        typedef G::out_edge_iterator      out_edge_iterator;

        typedef G::directed_category      directed_category;
        typedef G::edge_parallel_category edge_parallel_category;
        typedef G::traversal_category     traversal_category;

        typedef G::degree_size_type       degree_size_type;

        typedef void in_edge_iterator;
        typedef void vertex_iterator;
        typedef void vertices_size_type;
        typedef void edge_iterator;
        typedef void edges_size_type;
    };
}

// IncidenceGraph concept requirements
std::pair<Maze::out_edge_iterator, Maze::out_edge_iterator> out_edges(Maze::vertex_descriptor v, const Maze & g);
Maze::degree_size_type out_degree(Maze::vertex_descriptor v, const Maze & g);
Maze::vertex_descriptor source(const Maze::edge_descriptor &e, const Maze & g);
Maze::vertex_descriptor target(const Maze::edge_descriptor &e, const Maze & g);

static inline bool WalkableCoord(int x, int y)
{
    return IsInsideMap(x, y) && IsWalkable(x, y);
}

static inline bool WalkableCoord(const Coord &c)
{
    return WalkableCoord(c.x, c.y);
}

// Iterator
struct neighbor_iterator :
    public boost::iterator_facade<neighbor_iterator,
                                  std::pair<Coord, Coord>,
                                  boost::forward_traversal_tag,
                                  std::pair<Coord, Coord>& >
{
public:
    neighbor_iterator()
    {
    }

    neighbor_iterator(const Coord &c, bool end)
        : coord(c)
    {
        if (end)
        {
            dx = -1;
            dy = 2;
        }
        else
        {
            dy = -2;
            dx = 1;
            increment();
        }
    }
    
    neighbor_iterator &operator=(const neighbor_iterator &that)
    {
        coord = that.coord;
        dx = that.dx;
        dy = that.dy;
        return *this;
    }

    std::pair<Coord, Coord> operator*() const
    {
        return std::make_pair(coord, Coord(coord.x + dx, coord.y + dy));
    }

    void operator++()
    {
        increment();
    }

    bool operator==(neighbor_iterator const& that) const
    {
        return coord == that.coord && dx == that.dx && dy == that.dy;
    }

    bool equal(neighbor_iterator const& that) const { return operator==(that); }

    void increment()
    {
        for (;;)
        {
            dx++;
            if (dx == 2)
            {
                dx = -1;
                dy++;
            }
            else if (dx == 0 && dy == 0)
                continue;

            if (dy >= 2)
                return;
            if (WalkableCoord(coord.x + dx, coord.y + dy))
                return;
        }
    }

private:
    Coord coord;
    int dx, dy;
};

std::pair<Maze::out_edge_iterator, Maze::out_edge_iterator> out_edges(Maze::vertex_descriptor v, const Maze & g)
{
    return std::make_pair(
        Maze::out_edge_iterator(v, false),
        Maze::out_edge_iterator(v, true) );
}

Maze::degree_size_type out_degree(Maze::vertex_descriptor v, const Maze & g)
{
    std::pair<Maze::out_edge_iterator, Maze::out_edge_iterator> iters = out_edges(v, g);
    int deg = 0;
    while (iters.first != iters.second)
    {
        iters.first++;
        deg++;
    }
    return deg;
}

Maze::vertex_descriptor source(const Maze::edge_descriptor &e, const Maze & g)
{
    return e.first;
}

Maze::vertex_descriptor target(const Maze::edge_descriptor &e, const Maze & g)
{
    return e.second;
}

// Goal visitor and heuristic functor
class MazeGoal : public boost::default_astar_visitor,
                 public boost::astar_heuristic<Maze, int>
{
public:
    MazeGoal(const Coord &goal_) : goal(goal_)
    {
    }

    // Exception thrown when the goal vertex is found
    struct found_goal {};

    // Vertex visitor
    void examine_vertex(const Coord &v, const Maze&) const
    {
        if (v == goal)
            throw found_goal();
    }

    // Heuristic
    int operator()(const Coord &v)
    {
        return distLInf(v, goal);
    }

private:
    Coord goal;
};

// A hash function for vertices.
struct vertex_hash : public std::unary_function<Coord, std::size_t>
{
    std::size_t operator()(const Coord &c) const
    {
        return (c.x << 16) | c.y;
    }
};

template <typename K, typename V>
class default_map
{
public:
    typedef K key_type;
    typedef V data_type;
    typedef std::pair<K, V> value_type;

    default_map(const V &defaultValue_)
        : defaultValue(defaultValue_)
    {
    }

    V & operator[](K const& k)
    {
        if (m.find(k) == m.end())
        {
            m[k] = defaultValue;
        }
        return m[k];
    }

private:
    std::map<K, V> m;
    V const defaultValue;
};

bool GamePathfinder::FindPath(const Coord &start, const Coord &goal)
{
    waypoints.clear();
    waypoints.push_back(start);
    
    if (!WalkableCoord(start) || !WalkableCoord(goal))
        return false;

    boost::static_property_map<int> weight(1);

    // The predecessor map is a vertex-to-vertex mapping.
    typedef boost::unordered_map<Coord,
                                 Coord,
                                 vertex_hash> pred_map;
    pred_map predecessor;
    boost::associative_property_map<pred_map> pred_pmap(predecessor);

    typedef boost::associative_property_map< default_map<Coord, int> > DistanceMap;
    typedef default_map<Coord, int> WrappedDistanceMap;
    WrappedDistanceMap wrappedMap = WrappedDistanceMap(std::numeric_limits<int>::max());
    wrappedMap[start] = 0;
    DistanceMap d = DistanceMap(wrappedMap);

    MazeGoal maze_goal(goal);

    std::map<Coord, int> index_map, rank_map;
    std::map<Coord, boost::default_color_type> color_map;

    bool found = false;

    try
    {
        astar_search_no_init(
                Maze(), start, maze_goal,
                boost::weight_map(weight)
                    .predecessor_map(pred_pmap)
                    .distance_map(d)
                    .visitor(maze_goal)
                    .vertex_index_map(boost::associative_property_map< std::map<Coord, int> >(index_map))
                    .rank_map(boost::associative_property_map< std::map<Coord, int> >(rank_map))
                    .color_map(boost::associative_property_map< std::map<Coord, boost::default_color_type> >(color_map))
                    .distance_compare(std::less<int>())
                    .distance_combine(std::plus<int>())
            );
    }
    catch (MazeGoal::found_goal fg)
    {
        found = true;
    }

    if (!found)
        return false;

    // Walk backwards from the goal through the predecessor chain adding
    // vertices to the solution path.
    std::deque<Game::Coord> solution;
    for (Coord u = goal; u != start; u = predecessor[u])
        solution.push_front(u);

    // Generate waypoints by linearizing parts of path
    while (!solution.empty())
    {
        // Find prefix of solution that can be linearized

        // Binary search
        int start = 0;
        int end = solution.size();
        while (start < end - 1)
        {
            int mid = (start + end) / 2;
            if (CheckLinearPath(waypoints.back(), solution[mid]))
                start = mid;
            else
                end = mid;
        }
        solution.erase(solution.begin(), solution.begin() + start);
        waypoints.push_back(solution.front());
        solution.pop_front();
    }

    return true;
}

bool GamePathfinder::GetNextWaypoint(Coord &c)
{
    if (waypoints.size() < 2)
    {
        c = waypoints[0];
        return false;
    }
    c = waypoints[1];
    waypoints.pop_front();
    return waypoints.size() > 1;
}

std::vector<Coord> GamePathfinder::DumpPath() const
{
    std::vector<Coord> ret;
    if (waypoints.size() < 2)
        return ret;

    PlayerState tmp;
    tmp.coord = waypoints[0];

    for (size_t i = 1, n = waypoints.size(); i < n; i++)
    {
        tmp.from = tmp.coord;
        tmp.target = waypoints[i];
        while (tmp.coord != tmp.target)
        {
            ret.push_back(tmp.coord);
            tmp.MoveTowardsWaypoint();
        }
        if (tmp.target != waypoints[i])
        {
            // If collision happened (e.g. re-entering spawn area), abandon the rest of the path

            if (!ret.empty() && ret.back() != tmp.coord)
                ret.push_back(tmp.coord);
            return ret;
        }
    }
    ret.push_back(waypoints.back());

    return ret;
}

// Helper function for creating waypoints (linear path segments)
bool GamePathfinder::CheckLinearPath(const Game::Coord &start, const Game::Coord &target)
{
    PlayerState tmp;
    tmp.from = tmp.coord = start;
    tmp.target = target;
    while (tmp.coord != tmp.target)
        tmp.MoveTowardsWaypoint();
    return tmp.coord == target;
}
