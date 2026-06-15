#pragma once
#pragma warning(disable: 4819)
#include "common.h"

class SESSION;

constexpr int VIEW_RANGE = 15;
constexpr int SECTOR_SIZE = 15;

extern tbb::concurrent_unordered_map<int, std::shared_ptr<SESSION>> clients;
extern tbb::concurrent_unordered_map<int, std::unordered_set<int>> sectors;
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

// Striped sector locks: 256 mutexes indexed by sector_id % 256.
// Bots in different sectors (different stripes) proceed in parallel.
constexpr int SECTOR_STRIPE_COUNT = 256;
extern std::mutex g_sector_locks[SECTOR_STRIPE_COUNT];

// RAII guard for one or two sectors (deadlock-free: always locks lower stripe first).
struct SectorLock {
	std::unique_lock<std::mutex> lk1, lk2;

	// Single-sector op
	explicit SectorLock(int sid)
		: lk1(g_sector_locks[(unsigned)sid % SECTOR_STRIPE_COUNT]) {}

	// Two-sector op (erase old + insert new). Deadlock-free via ordering.
	SectorLock(int sid_old, int sid_new) {
		unsigned s1 = (unsigned)sid_old % SECTOR_STRIPE_COUNT;
		unsigned s2 = (unsigned)sid_new % SECTOR_STRIPE_COUNT;
		if (s1 == s2) {
			lk1 = std::unique_lock<std::mutex>(g_sector_locks[s1]);
		} else if (s1 < s2) {
			lk1 = std::unique_lock<std::mutex>(g_sector_locks[s1]);
			lk2 = std::unique_lock<std::mutex>(g_sector_locks[s2]);
		} else {
			lk1 = std::unique_lock<std::mutex>(g_sector_locks[s2]);
			lk2 = std::unique_lock<std::mutex>(g_sector_locks[s1]);
		}
	}
};
