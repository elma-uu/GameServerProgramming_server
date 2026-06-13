#include "pch.h"
#include "SESSION.h"
#include "EXP_OVER.h"
#include "Database.h"

void initNpcs()
{
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> distX(0, WORLD_WIDTH - 1);
	std::uniform_int_distribution<int> distY(0, WORLD_HEIGHT - 1);

	for (int i = 0; i < NUM_NPCS; ++i) {
		int npc_id = NPC_ID_START + i;
		auto npc = std::make_shared<SESSION>(npc_id, false);
		npc->mX = static_cast<short>(distX(rng));
		npc->mY = static_cast<short>(distY(rng));
		// Level scales with distance from center (1000, 1000): min 1, max 100
		// Max possible distance: corner (0,0) or (2000,2000) -> sqrt(1000^2 + 1000^2) ~ 1414
		{
			const float MAX_DIST = sqrtf(1000.0f * 1000.0f + 1000.0f * 1000.0f);
			float dx = static_cast<float>(npc->mX - 1000);
			float dy = static_cast<float>(npc->mY - 1000);
			float dist = sqrtf(dx * dx + dy * dy);
			int level = 1 + static_cast<int>(dist / MAX_DIST * 99.0f);
			if (level > 100) level = 100;
			npc->mLevel = static_cast<unsigned char>(level);
		}
		npc->mHp = 50;
		npc->mMaxHp = 50;

		// Assign monster name by level tier
		// lv 1-30: W.Goblin  / lv 31-60: Goblin
		// lv 61-80: Gob.Paladin / lv 81-100: Gob.Paladin(4) or Gob.Shaman(1)
		{
			int lv = npc->mLevel;
			const char* npcName;
			if (lv <= 30) {
				npcName = "¶°µ¹ÀÌ °íºí¸°";
			} else if (lv <= 60) {
				npcName = "°íºí¸°";
			} else if (lv <= 80) {
				npcName = "°íºí¸° ÆÈ¶óµò";
			} else {
				npcName = (rng() % 5 == 0) ? "°íºí¸° »þ¸Õ" : "°íºí¸° ÆÈ¶óµò";
			}
			sprintf_s(npc->mUsername, MAX_NAME_LEN, "%s", npcName);
		}

		npc->mState = CS_PLAYING;

		int sector_id = get_sector_id(npc->mX, npc->mY);
		sectors[sector_id].insert(npc_id);
		npc->mSector_id = sector_id;

		clients[npc_id] = npc;

		if (i % 10000 == 0)
			std::cout << "NPC init: " << i << "/" << NUM_NPCS << "\n";
	}
	std::cout << "Initialized " << NUM_NPCS << " NPCs.\n";
}

void npc_timer_thread()
{
	while (true) {
		std::this_thread::sleep_for(std::chrono::milliseconds(NPC_MOVE_INTERVAL));
		for (int i = 0; i < NUM_NPCS; ++i) {
			int npc_id = NPC_ID_START + i;
			auto it = clients.find(npc_id);
			if (it == clients.end()) continue;
			std::shared_ptr<SESSION> npc = it->second;
			if (!npc || npc->m_visible_players.empty()) continue;
			EXP_OVER* over = new EXP_OVER(IO_NPC_MOVE);
			PostQueuedCompletionStatus(g_iocp, 1,
				static_cast<ULONG_PTR>(npc_id), &over->m_over);
		}
	}
}

void db_save_thread()
{
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(10));

		int saved = 0;
		for (auto& [id, session] : clients) {
			if (!session || !session->is_player) continue;
			if (session->mState != CS_PLAYING) continue;

			PlayerSaveData data;
			strncpy_s(data.username, session->mUsername, 20);
			data.x  = session->mX;   data.y       = session->mY;
			data.hp = session->mHp;  data.max_hp  = session->mMaxHp;
			data.exp = session->mExp; data.level   = session->mLevel;
			data.str = session->mStr; data.intl    = session->mIntl;
			data.dex = session->mDex; data.luk     = session->mLuk;
			data.stat_points = session->mStatPoints;

			if (Database::SavePlayer(data)) ++saved;
		}
		if (saved > 0)
			std::cout << "[AutoSave] Saved " << saved << " player(s).\n";
	}
}

void worker_thread()
{
	while (true) {
		DWORD num_bytes = 0;
		ULONG_PTR completion_key = 0;
		EXP_OVER* exp_over = nullptr;

		BOOL success = GetQueuedCompletionStatus(g_iocp, &num_bytes, &completion_key, (LPOVERLAPPED*)&exp_over, INFINITE);

		if (!success || exp_over == nullptr) {
			continue;
		}

		int client_id = static_cast<int>(completion_key);

		if (exp_over->m_iotype == IO_ACCEPT) {
			SOCKET client_socket = exp_over->m_client_socket;
			int my_id = player_index++;

			std::cout << "Client " << my_id << " connected" << std::endl;

			std::shared_ptr<SESSION> new_client = std::make_shared<SESSION>(client_socket, my_id);
			clients[my_id] = new_client;
			CreateIoCompletionPort((HANDLE)client_socket, g_iocp, (ULONG_PTR)my_id, 0);

			new_client->doRecv();

			// Issue new AcceptEx for next connection
			EXP_OVER* new_accept_over = new EXP_OVER(IO_ACCEPT);
			new_accept_over->m_client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			if (new_accept_over->m_client_socket == INVALID_SOCKET) {
				std::cout << "Failed to create socket for AcceptEx" << std::endl;
				delete new_accept_over;
			}
			else {
				BOOL accept_result = AcceptEx(g_server, new_accept_over->m_client_socket, &new_accept_over->m_ring_buffer.buffer, 0,
					sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
					NULL, &new_accept_over->m_over);
				if (!accept_result && WSAGetLastError() != WSA_IO_PENDING) {
					std::cout << "AcceptEx failed with error: " << WSAGetLastError() << std::endl;
					closesocket(new_accept_over->m_client_socket);
					delete new_accept_over;
				}
			}

			delete exp_over;
		}
		else if (exp_over->m_iotype == IO_RECV) {
			std::shared_ptr<SESSION> cla = clients[client_id];
			if (nullptr == cla) {
				delete exp_over;
				continue;
			}

			if (num_bytes == 0) {
				disconnect(client_id);
				delete exp_over;
				continue;
			}

			// Push received data into SESSION's RingBuffer
			if (!cla->mRecvBuffer.push(exp_over->m_ring_buffer.buffer, num_bytes)) {
				std::cout << "RingBuffer overflow for client[" << client_id << "]" << std::endl;
				disconnect(client_id);
				delete exp_over;
				continue;
			}

			// Process complete packets from ring buffer
			while (cla->mRecvBuffer.get_size() > 0) {
				// Need at least 1 byte for size field
				if (cla->mRecvBuffer.get_size() < 1) {
					break;
				}

				// Peek at the size field (first byte)
				char size_byte = cla->mRecvBuffer.buffer[cla->mRecvBuffer.head];
				int packet_size = static_cast<unsigned char>(size_byte);

				// Validate packet size
				if (packet_size < 2 || packet_size > 256) {
					std::cout << "Invalid packet size[" << packet_size << "] for client[" << client_id << "]" << std::endl;
					disconnect(client_id);
					break;
				}

				// Check if we have the complete packet
				if (cla->mRecvBuffer.get_size() < packet_size) {
					break;
				}

				// Extract complete packet from ring buffer
				char packet_buffer[256];
				int pop_result = cla->mRecvBuffer.pop(packet_buffer, packet_size);
				if (pop_result == -1) {
					std::cout << "Failed to pop packet from RingBuffer for client[" << client_id << "]" << std::endl;
					disconnect(client_id);
					break;
				}

				// Process the packet
				if (!cla->processPacket(reinterpret_cast<unsigned char*>(packet_buffer))) {
					std::cout << "Failed to process packet for client[" << client_id << "]" << std::endl;
					disconnect(client_id);
					break;
				}
			}

			// Issue next recv
			cla->doRecv();
			delete exp_over;
		}
		else if (exp_over->m_iotype == IO_SEND) {
			// Send completed
			delete exp_over;
		}
		else if (exp_over->m_iotype == IO_NPC_MOVE) {
			std::shared_ptr<SESSION> npc = clients[client_id];
			if (npc && !npc->is_player && npc->mState == CS_PLAYING)
				npc->doNpcMove();
			delete exp_over;
		}
	}
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	g_server = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	::bind(g_server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(g_server, SOMAXCONN);

	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	CreateIoCompletionPort((HANDLE)g_server, g_iocp, -1, 0);

	EXP_OVER* accept_over = new EXP_OVER(IO_ACCEPT);
	accept_over->m_client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (accept_over->m_client_socket == INVALID_SOCKET) {
		std::cout << "Failed to create initial socket for AcceptEx" << std::endl;
		delete accept_over;
		closesocket(g_server);
		WSACleanup();
		return 1;
	}

	BOOL accept_result = AcceptEx(g_server, accept_over->m_client_socket, &accept_over->m_ring_buffer.buffer, 0,
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
		NULL, &accept_over->m_over);
	if (!accept_result && WSAGetLastError() != WSA_IO_PENDING) {
		std::cout << "Initial AcceptEx failed with error: " << WSAGetLastError() << std::endl;
		closesocket(accept_over->m_client_socket);
		delete accept_over;
		closesocket(g_server);
		WSACleanup();
		return 1;
	}

	// Connect to database (must succeed before accepting players)
	if (!Database::Connect()) {
		std::cout << "Failed to connect to DB. Exiting.\n";
		closesocket(g_server);
		WSACleanup();
		return 1;
	}

	// Initialize NPCs before accepting players
	initNpcs();

	// Start background threads
	std::thread(npc_timer_thread).detach();
	std::thread(db_save_thread).detach();

	std::vector<std::thread> worker_threads;
	int num_threads = std::thread::hardware_concurrency();

	for (int i = 0; i < num_threads; ++i) {
		worker_threads.emplace_back(worker_thread);
	}

	for (auto& t : worker_threads) {
		t.join();
	}

	closesocket(g_server);
	WSACleanup();

	return 0;
}