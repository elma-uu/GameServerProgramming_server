#include "common.h"
#include "SESSION.h"

// Global variable definitions
tbb::concurrent_unordered_map<int, std::shared_ptr<SESSION>> clients;
tbb::concurrent_unordered_map<int, std::unordered_set<int>> sectors;

SOCKET g_server;
HANDLE g_iocp;

std::atomic<int> player_index = 0;

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

SESSION::SESSION()
{
	std::cout << "SESSION create error" << std::endl;
	exit(1);
}

SESSION::SESSION(SOCKET s, int id)
{
	mClient = s;
	mId = id;
	mState = CS_CONNECT;
	mX = 1000;
	mY = 1000;
	mMove_time = 0;
	mSector_id = 0;
	is_player = true;
}

SESSION::SESSION(int id, bool isPlayer)
{
	mId = id;
	mState = CS_CONNECT;
	mX = 1000;
	mY = 1000;
	mMove_time = 0;
	mSector_id = 0;
	is_player = isPlayer;
}

SESSION::~SESSION()
{
	if(mClient != INVALID_SOCKET)
		closesocket(mClient);
}

void SESSION::sendLoginSuccess()
{
	S2C_LoginResult packet;
	packet.size = sizeof(S2C_LoginResult);
	packet.type = S2C_LOGIN_RESULT;
	packet.success = true;
	strncpy_s(packet.message, "Login successful.", sizeof(packet.message));
	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::doRecv()
{
	EXP_OVER* o = new EXP_OVER(IO_RECV);
	o->m_client_socket = mClient;
	DWORD recv_flag = 0;
	int recv_result = WSARecv(mClient, &o->m_wsa, 1, 0, &recv_flag, &o->m_over, nullptr);
	if (recv_result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
		std::cout << "WSARecv failed with error: " << WSAGetLastError() << std::endl;
		delete o;
	}
}

void SESSION::doSend(int numBytes, char* mess)
{
	EXP_OVER* o = new EXP_OVER(IO_SEND);
	o->m_wsa.len = numBytes;
	memcpy(o->m_ring_buffer.buffer, mess, numBytes);
	int send_result = WSASend(mClient, &o->m_wsa, 1, 0, 0, &o->m_over, nullptr);
	if (send_result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
		std::cout << "WSASend failed with error: " << WSAGetLastError() << std::endl;
		delete o;
	}
}

void SESSION::sendAvatarInfo()
{
	S2C_AvatarInfo packet;
	packet.size = sizeof(S2C_AvatarInfo);
	packet.type = S2C_AVATAR_INFO;
	packet.playerId = mId;
	packet.visualId = 0;
	packet.x = mX;
	packet.y = mY;
	packet.exp = 0;
	packet.level = 1;
	packet.hp = 100;
	packet.max_hp = 100;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::sendAddPlayer(int player_id)
{
	S2C_AddObject packet;
	packet.size = sizeof(S2C_AddObject);
	packet.type = S2C_ADD_OBJECT;
	packet.object_id = player_id;
	packet.visual_id = 0;
	std::shared_ptr<SESSION> pl = clients[player_id];
	if (nullptr == pl) return;
	memcpy(packet.obj_name, pl->mUsername, sizeof(packet.obj_name));
	packet.x = pl->mX;
	packet.y = pl->mY;
	packet.exp = pl->mExp;
	packet.level = pl->mLevel;
	packet.hp = pl->mHp;
	packet.max_hp = pl->mMaxHp;
	m_visible_mutex.lock();
	if (m_visible_players.count(player_id) > 0) {
		m_visible_mutex.unlock();
		return;
	}
	m_visible_players.insert(player_id);
	m_visible_mutex.unlock();

	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::sendRemovePlayer(int player_id)
{
	S2C_RemoveObject packet;
	packet.size = sizeof(S2C_RemoveObject);
	packet.type = S2C_REMOVE_OBJECT;
	packet.object_id = player_id;

	m_visible_mutex.lock();
	if (m_visible_players.count(player_id) == 0) {
		m_visible_mutex.unlock();
		return;
	}
	m_visible_players.erase(player_id);
	m_visible_mutex.unlock();

	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::sendMovePacket(int mover)
{
	S2C_MoveObject packet;
	packet.size = sizeof(S2C_MoveObject);
	packet.type = S2C_MOVE_OBJECT;
	packet.object_id = mover;
	std::shared_ptr<SESSION> pl = clients[mover];
	if (nullptr == pl) return;
	packet.x = pl->mX;
	packet.y = pl->mY;
	packet.move_time = pl->mMove_time;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

bool SESSION::processPacket(unsigned char* p)
{
	if (not is_player) return true;

	PACKET_TYPE type = *reinterpret_cast<PACKET_TYPE*>(&p[1]);
	switch (type) {
	case C2S_LOGIN:
	{
		C2S_Login* packet = reinterpret_cast<C2S_Login*>(p);
		strncpy_s(mUsername, packet->username, MAX_NAME_LEN);
		std::cout << "Player[" << mId << "] logged in as " << mUsername << std::endl;

		// Initialize sector
		int initial_sector_id = get_sector_id(mX, mY);
		sectors[initial_sector_id].insert(mId);
		mSector_id = initial_sector_id;

		sendAvatarInfo();
		mState = CS_PLAYING;

		// Get visible players from sectors
		std::unordered_set<int> new_v_players;
		get_visible_players_from_sectors(new_v_players);

		// Send add player packets for visible players
		for (int id : new_v_players)
		{
			sendAddPlayer(id);
			std::shared_ptr<SESSION> pl = clients[id];
			if (nullptr == pl) continue;
			pl->sendAddPlayer(mId);
		}

	}
	break;
	case C2S_MOVE:
	{
		C2S_Move* packet = reinterpret_cast<C2S_Move*>(p);
		mX = packet->x;
		mY = packet->y;
		mMove_time = packet->move_time;

		auto old_v_players = m_visible_players;

		int new_sector_id = get_sector_id(mX, mY);
		if (new_sector_id != mSector_id) {
			sectors[mSector_id].erase(mId);
			sectors[new_sector_id].insert(mId);
			mSector_id = new_sector_id;
		}

		std::unordered_set<int> new_v_players;
		get_visible_players_from_sectors(new_v_players);

		// Send move to visible players
		for (int id : new_v_players) {
			if (old_v_players.count(id) == 0) {
				// New player came into view
				sendAddPlayer(id);
				std::shared_ptr<SESSION> pl = clients[id];
				if (nullptr == pl) continue;
				pl->sendAddPlayer(mId);
			}
			else {
				// Player still in view - send move update
				std::shared_ptr<SESSION> pl = clients[id];
				if (nullptr == pl) continue;
				pl->sendMovePacket(mId);
			}
		}

		// Remove players out of view
		for (int id : old_v_players) {
			if (new_v_players.count(id) == 0) {
				sendRemovePlayer(id);
				std::shared_ptr<SESSION> pl = clients[id];
				if (nullptr == pl) continue;
				pl->sendRemovePlayer(mId);
			}
		}
	}
	break;
	default:
		std::cout << "Unknown packet type received from player[" << mId << "].\n";
		return false;
	}
	return true;
}

bool SESSION::is_visible(short x, short y)
{
	return abs(mX - x) <= VIEW_RANGE
		&& abs(mY - y) <= VIEW_RANGE;
}

void SESSION::get_visible_players_from_sectors(std::unordered_set<int>& visible_set)
{
	int sectors_x = (WORLD_WIDTH / SECTOR_SIZE) + 1;
	int my_sector_x = mSector_id % sectors_x;
	int my_sector_y = mSector_id / sectors_x;

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			int sector_x = my_sector_x + dx;
			int sector_y = my_sector_y + dy;

			if (sector_x < 0 || sector_x >= sectors_x || sector_y < 0 || sector_y >= sectors_x)
				continue;

			int sector_id = sector_y * sectors_x + sector_x;
			auto it = sectors.find(sector_id);
			if (it == sectors.end()) continue;

			for (int player_id : it->second) {
				std::shared_ptr<SESSION> pl = clients[player_id];
				if (nullptr == pl) continue;
				if (pl->mId == mId) continue;
				if (!pl->is_player) continue;
				if (pl->mState != CS_PLAYING) continue;
				if (is_visible(pl->mX, pl->mY))
					visible_set.insert(player_id);
			}
		}
	}
}
