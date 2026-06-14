#pragma once
#include "protocol_2026.h"
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <queue>
#include <unordered_set>
#include <random>
#include <cmath>
#include <tbb/concurrent_unordered_map.h>

#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "WS2_32.lib")

// Town safe zone: 30-tile radius around (1000, 1000). Monsters cannot enter or spawn here.
constexpr int TOWN_CENTER_X = 1000;
constexpr int TOWN_CENTER_Y = 1000;
constexpr int TOWN_SAFE_R   = 30;

inline bool is_in_safe_zone(short x, short y)
{
	int dx = (int)x - TOWN_CENTER_X;
	int dy = (int)y - TOWN_CENTER_Y;
	return dx * dx + dy * dy <= TOWN_SAFE_R * TOWN_SAFE_R;
}

