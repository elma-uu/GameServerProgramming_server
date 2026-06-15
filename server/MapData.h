#pragma once
#include <fstream>
#include <string>
#include <cstring>

// World tile coordinate where the map file origin sits.
constexpr int MAP_ORIGIN_X  = 950;
constexpr int MAP_ORIGIN_Y  = 950;
constexpr int MAP_COLS      = 100;
constexpr int MAP_ROWS      = 100;

// 0 = floor, 1 = wall
extern unsigned char g_map[MAP_ROWS][MAP_COLS];
extern bool          g_map_loaded;

inline bool LoadMap(const char* path)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;

    memset(g_map, 1, sizeof(g_map));  // default: all walls
    std::string line;
    int row = 0;
    while (row < MAP_ROWS && std::getline(f, line)) {
        for (int col = 0; col < MAP_COLS && col < (int)line.size(); ++col)
            g_map[row][col] = (line[col] == '0') ? 0 : 1;
        ++row;
    }
    g_map_loaded = true;
    return true;
}

// Returns true if world tile (wx, wy) is a walkable floor.
// Tiles outside the map area are considered floor (open world).
inline bool IsWalkable(short wx, short wy)
{
    int col = (int)wx - MAP_ORIGIN_X;
    int row = (int)wy - MAP_ORIGIN_Y;
    if (col < 0 || col >= MAP_COLS || row < 0 || row >= MAP_ROWS)
        return true;   // outside defined map = open world
    return g_map[row][col] == 0;
}
