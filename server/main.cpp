#include "pch.h"
#include "SESSION.h"
#include "EXP_OVER.h"
#include "Database.h"
#include "LuaManager.h"
#include "MapData.h"

// MapData globals
unsigned char g_map[MAP_ROWS][MAP_COLS] = {};
bool          g_map_loaded = false;

// Thread functions defined in their own translation units
void worker_thread();
void npc_timer_thread();
void respawn_timer_thread();
void db_save_thread();

void initNpcs()
{
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> distX(0, 1999); // keep NPCs in main world
	std::uniform_int_distribution<int> distY(0, 1999);

	for (int i = 0; i < NUM_NPCS; ++i) {
		int npc_id = NPC_ID_START + i;
		auto npc = std::make_shared<SESSION>(npc_id, false);
		do {
			npc->mX = static_cast<short>(distX(rng));
			npc->mY = static_cast<short>(distY(rng));
		} while (is_in_safe_zone(npc->mX, npc->mY));

		{
			const float MAX_DIST = sqrtf(1000.0f * 1000.0f + 1000.0f * 1000.0f);
			float dx = static_cast<float>(npc->mX - 1000);
			float dy = static_cast<float>(npc->mY - 1000);
			float dist = sqrtf(dx * dx + dy * dy);
			int level = 1 + static_cast<int>(dist / MAX_DIST * 99.0f);
			if (level > 100) level = 100;
			npc->mLevel = static_cast<unsigned char>(level);
		}
		int npcHp = npc->mLevel * 20;
		npc->mHp = npcHp;
		npc->mMaxHp = npcHp;

		{
			int lv = npc->mLevel;
			const char* npcName;
			int visualId;
			if (lv <= 30) {
				npcName = "Dog";         visualId = 0;
			} else if (lv <= 60) {
				npcName = "Skeleton";    visualId = 1;
			} else if (lv <= 80) {
				npcName = "Skel.Knight"; visualId = 2;
			} else {
				bool isMage = (rng() % 5 == 0);
				npcName  = isMage ? "Skel.Mage" : "Skel.Knight";
				visualId = isMage ? 3 : 2;
			}
			sprintf_s(npc->mUsername, MAX_NAME_LEN, "%s", npcName);
			npc->mVisualId = visualId;
		}

		npc->mSpawnX = npc->mX;
		npc->mSpawnY = npc->mY;
		npc->mIsDead = false;
		npc->mState  = CS_PLAYING;

		int sector_id = get_sector_id(npc->mX, npc->mY);
		sectors[sector_id].insert(npc_id);
		npc->mSector_id = sector_id;
		clients[npc_id] = npc;

		if (i % 10000 == 0)
			std::cout << "NPC init: " << i << "/" << NUM_NPCS << "\n";
	}
	std::cout << "Initialized " << NUM_NPCS << " NPCs.\n";
}

// Triangle layout (center 1000,1000):
//         [A]           <- Ability top vertex  (1000, 993)
//
//   [Q]         [S]    <- Quest NPC (994, 1007), Shop (1006, 1007)
void initTownNpcs()
{
	struct TownNpcDef { short x; short y; int visualId; const char* name; };
	const TownNpcDef defs[3] = {
		{ 1000, 993,  VISUAL_TOWN_ABILITY,    "Ability NPC" },
		{  994, 1007, VISUAL_TOWN_RESTAURANT, "Quest NPC"   },
		{ 1006, 1007, VISUAL_TOWN_SHOP,       "Shop"        },
	};

	for (int i = 0; i < 3; ++i) {
		int id = TOWN_NPC_ID_START + i;
		auto npc = std::make_shared<SESSION>(id, false);
		npc->mX = defs[i].x; npc->mY = defs[i].y;
		npc->mSpawnX = npc->mX; npc->mSpawnY = npc->mY;
		npc->mIsDead = false; npc->mIsStationary = true;
		npc->mVisualId = defs[i].visualId;
		npc->mHp = npc->mMaxHp = 999999;
		npc->mLevel = 1;
		sprintf_s(npc->mUsername, MAX_NAME_LEN, "%s", defs[i].name);
		npc->mState = CS_PLAYING;
		int sid = get_sector_id(npc->mX, npc->mY);
		sectors[sid].insert(id);
		npc->mSector_id = sid;
		clients[id] = npc;
	}
	std::cout << "Initialized 3 town NPCs.\n";
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	g_server = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family      = AF_INET;
	server_addr.sin_port        = htons(PORT);
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	::bind(g_server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(g_server, SOMAXCONN);

	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	CreateIoCompletionPort((HANDLE)g_server, g_iocp, -1, 0);

	EXP_OVER* accept_over = new EXP_OVER(IO_ACCEPT);
	accept_over->m_client_socket =
		WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (accept_over->m_client_socket == INVALID_SOCKET) {
		std::cout << "Failed to create initial socket for AcceptEx\n";
		delete accept_over;
		closesocket(g_server);
		WSACleanup();
		return 1;
	}

	BOOL accept_result = AcceptEx(g_server, accept_over->m_client_socket,
		&accept_over->m_ring_buffer.buffer, 0,
		sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16,
		NULL, &accept_over->m_over);
	if (!accept_result && WSAGetLastError() != WSA_IO_PENDING) {
		std::cout << "Initial AcceptEx failed: " << WSAGetLastError() << "\n";
		closesocket(accept_over->m_client_socket);
		delete accept_over;
		closesocket(g_server);
		WSACleanup();
		return 1;
	}

	if (!Database::Connect()) {
		std::cout << "Failed to connect to DB. Exiting.\n";
		closesocket(g_server);
		WSACleanup();
		return 1;
	}

	// Register Lua scripts — each worker thread loads them into its own state
	if (!LuaManager::Init("npc_ai.lua")) {
		std::cout << "Warning: Lua AI unavailable, NPCs will only wander.\n";
	}
	LuaManager::LoadScript("quests.lua");

	// Load world map (collision)
	const char* mapPaths[] = {
		"Resource\\Map\\map.txt",
		"..\\Resource\\Map\\map.txt",
		"..\\..\\Resource\\Map\\map.txt",
	};
	bool mapOk = false;
	for (const char* p : mapPaths) {
		if (LoadMap(p)) { std::cout << "[Map] Loaded: " << p << "\n"; mapOk = true; break; }
	}
	if (!mapOk) std::cout << "[Map] WARNING: map.txt not found, no wall collision.\n";

	initNpcs();
	initTownNpcs();

	std::thread(npc_timer_thread).detach();
	std::thread(respawn_timer_thread).detach();
	std::thread(db_save_thread).detach();

	std::vector<std::thread> worker_threads;
	int num_threads = std::thread::hardware_concurrency();
	for (int i = 0; i < num_threads; ++i)
		worker_threads.emplace_back(worker_thread);
	for (auto& t : worker_threads)
		t.join();

	closesocket(g_server);
	WSACleanup();
	return 0;
}
