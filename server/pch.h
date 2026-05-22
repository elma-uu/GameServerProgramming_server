#pragma once
#include "common.h"

class SESSION;

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

//---------
// Sector
//---------
inline int get_sector_id(short x, short y)
{
	int sector_x = x / SECTOR_SIZE;
	int sector_y = y / SECTOR_SIZE;
	return sector_y * ((WORLD_WIDTH / SECTOR_SIZE) + 1) + sector_x;
}

void disconnect(int key);

#include "SESSION.h"

inline void disconnect(int key)
{
	std::cout << "client " << key << " disconnected" << std::endl;
	std::shared_ptr<SESSION> cla = clients[key];
	if (nullptr != cla)
	{
		cla->mState = CS_LOGOUT;
		sectors[cla->mSector_id].erase(key);
		auto visible_copy = cla->m_visible_players;
		for (auto& other : visible_copy)
		{
			std::shared_ptr<SESSION> o = clients[other];
			if (nullptr == o) continue;
			if (o->mState == CS_PLAYING)
				o->sendRemovePlayer(key);
		}
		if (cla->mClient != INVALID_SOCKET)
			closesocket(cla->mClient);
		cla->mClient = INVALID_SOCKET;
	}
	clients.unsafe_erase(key);
}