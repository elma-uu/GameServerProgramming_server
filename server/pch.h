#pragma once
#include "common.h"
#include "SESSION.h"

constexpr int VIEW_RANGE = 15;
constexpr int SECTOR_SIZE = 10;

tbb::concurrent_unordered_map<int, std::shared_ptr<SESSION>> clients;
tbb::concurrent_unordered_map<int, std::unordered_set<int>> sectors;

SOCKET g_server;
HANDLE g_iocp;


void error_display(const wchar_t* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::wcout << msg;
	std::wcout << L" === ¿¡·¯ " << lpMsgBuf << std::endl;
	LocalFree(lpMsgBuf);
}

inline int get_sector_id(short x, short y)
{
	int sector_x = x / SECTOR_SIZE;
	int sector_y = y / SECTOR_SIZE;
	return sector_y * ((WORLD_WIDTH / SECTOR_SIZE) + 1) + sector_x;
}
