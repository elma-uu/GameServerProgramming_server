#pragma once
#pragma warning(disable: 4819)
#include "common.h"

class SESSION;

constexpr int VIEW_RANGE = 15;
constexpr int SECTOR_SIZE = 15;

extern tbb::concurrent_unordered_map<int, std::shared_ptr<SESSION>> clients;
extern tbb::concurrent_unordered_map<int, std::unordered_set<int>> sectors;
extern std::mutex g_sectors_mutex;   // guards all sectors[x].insert/erase/iterate
extern tbb::concurrent_queue<int> g_free_player_ids;


extern SOCKET g_server;
extern HANDLE g_iocp;


void error_display(const wchar_t* msg, int err_no);

//---------
// Sector
//---------
inline int get_sector_id(short x, short y)
{
	int sector_x = x / SECTOR_SIZE;
	int sector_y = y / SECTOR_SIZE;
	return sector_y * ((WORLD_WIDTH / SECTOR_SIZE) + 1) + sector_x;
}