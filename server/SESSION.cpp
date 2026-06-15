#include "common.h"
#include "SESSION.h"
#include "Database.h"
#include "LuaManager.h"
#include "PartyManager.h"
#include "ObjectPool.h"

// ── Global definitions ────────────────────────────────────────────────────
tbb::concurrent_unordered_map<int, std::shared_ptr<SESSION>> clients;
tbb::concurrent_unordered_map<int, std::unordered_set<int>>  sectors;
tbb::concurrent_unordered_map<int, bool>                      g_player_ids;

std::mutex           g_active_npcs_mutex;
std::unordered_set<int> g_active_npcs;

SOCKET     g_server;
HANDLE     g_iocp;
std::mutex g_sectors_mutex;
tbb::concurrent_queue<int>            g_free_player_ids;
tbb::concurrent_queue<PlayerSaveData> g_pending_saves;

void error_display(const wchar_t* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::wcout << msg;
	std::wcout << L" === Error: " << lpMsgBuf << std::endl;
	LocalFree(lpMsgBuf);
}

// ── Constructors ─────────────────────────────────────────────────────────
SESSION::SESSION()
{
	std::cout << "SESSION create error" << std::endl;
	exit(1);
}

SESSION::SESSION(SOCKET s, int id)
{
	mClient    = s;
	mId        = id;
	mState     = CS_CONNECT;
	mX = 1000; mY = 1000;
	mMove_time = 0;
	mSector_id = 0;
	mDirection = DOWN;
	is_player  = true;
	mHp = 100; mMaxHp = 100; mExp = 0; mLevel = 1;
	mStr = 5; mIntl = 5; mDex = 5; mLuk = 5; mStatPoints = 0;
	mPartyId  = -1;
	mVisualId = 0;
	mGold     = 0;
}

SESSION::SESSION(int id, bool isPlayer)
{
	mClient    = INVALID_SOCKET;
	mId        = id;
	mState     = CS_CONNECT;
	mX = 1000; mY = 1000;
	mMove_time = 0;
	mSector_id = 0;
	mDirection = DOWN;
	is_player  = isPlayer;
	mHp = 100; mMaxHp = 100; mExp = 0; mLevel = 1;
	mStr = 5; mIntl = 5; mDex = 5; mLuk = 5; mStatPoints = 0;
	mTargetId = -1; mChaseRemaining = 0;
	mSpawnX = 1000; mSpawnY = 1000; mIsDead = false; mIsStationary = false;
	mAttackTick = 0;
	mPartyId  = -1;
	mVisualId = 0;
	mGold     = 0;
}

SESSION::~SESSION()
{
	if (mClient != INVALID_SOCKET)
		closesocket(mClient);
}

// ── I/O ──────────────────────────────────────────────────────────────────
void SESSION::doRecv()
{
	EXP_OVER* o = new EXP_OVER(IO_RECV);
	o->m_client_socket = mClient;
	DWORD recv_flag = 0;
	int recv_result = WSARecv(mClient, &o->m_wsa, 1, 0, &recv_flag, &o->m_over, nullptr);
	if (recv_result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
		std::cout << "WSARecv failed: " << WSAGetLastError() << std::endl;
		delete o;
	}
}

void SESSION::doSend(int numBytes, char* mess)
{
	EXP_OVER* o = ObjectPool::AcquireSend();
	o->m_wsa.len = numBytes;
	memcpy(o->m_ring_buffer.buffer, mess, numBytes);
	int send_result = WSASend(mClient, &o->m_wsa, 1, 0, 0, &o->m_over, nullptr);
	if (send_result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
		std::cout << "WSASend failed: " << WSAGetLastError() << std::endl;
		ObjectPool::ReleaseSend(o);
	}
}
