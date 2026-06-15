#include "pch.h"
#include "SESSION.h"
#include "ObjectPool.h"
#include "LuaManager.h"
#include "Database.h"
#include "PartyManager.h"
#include "DungeonManager.h"

void disconnect(int key)
{
	std::cout << "client " << key << " disconnected" << std::endl;
	std::shared_ptr<SESSION> cla = clients[key];
	if (cla) {
		if (cla->is_player && cla->mState == CS_PLAYING && cla->mUsername[0] != '\0') {
			PlayerSaveData sd = {};
			strncpy_s(sd.username, cla->mUsername, MAX_NAME_LEN);
			sd.x          = cla->mX;   sd.y          = cla->mY;
			sd.hp         = cla->mHp;  sd.max_hp     = cla->mMaxHp;
			sd.exp        = cla->mExp; sd.level       = cla->mLevel;
			sd.str        = cla->mStr; sd.intl        = cla->mIntl;
			sd.dex        = cla->mDex; sd.luk         = cla->mLuk;
			sd.stat_points = cla->mStatPoints;
			sd.visual_id  = cla->mVisualId;
			sd.gold       = cla->mGold;
			Database::SavePlayer(sd);
		}

		cla->mState = CS_LOGOUT;
		sectors[cla->mSector_id].erase(key);

		if (cla->mPartyId >= 0) {
			int partyId = cla->mPartyId;
			PartyManager::LeaveParty(key);
			cla->mPartyId = -1;
			broadcastPartyUpdate(partyId);
		}

		S2C_RemoveObject removePacket;
		removePacket.size      = sizeof(S2C_RemoveObject);
		removePacket.type      = S2C_REMOVE_OBJECT;
		removePacket.object_id = key;

		auto visible_copy = cla->m_visible_players;
		for (auto& other : visible_copy) {
			std::shared_ptr<SESSION> o = clients[other];
			if (!o || o->mState != CS_PLAYING) continue;
			o->m_visible_mutex.lock();
			o->m_visible_players.erase(key);
			o->m_visible_mutex.unlock();
			o->doSend(removePacket.size, reinterpret_cast<char*>(&removePacket));
		}
		if (cla->mClient != INVALID_SOCKET)
			closesocket(cla->mClient);
		cla->mClient = INVALID_SOCKET;
	}
	// Close dungeon instance if this was the last occupant
	if (cla && cla->mDungeonInstanceId >= 0) {
		int inst = cla->mDungeonInstanceId;
		bool still_occupied = false;
		for (auto& [pid, _] : g_player_ids) {
			if (pid == key) continue;
			auto it = clients.find(pid);
			if (it == clients.end() || !it->second) continue;
			if (it->second->mDungeonInstanceId == inst) {
				still_occupied = true;
				break;
			}
		}
		if (!still_occupied) DungeonManager::CloseInstance(inst);
	}

	g_player_ids.unsafe_erase(key);
	clients.unsafe_erase(key);
}

void worker_thread()
{
	// Each worker thread gets its own Lua state — no global mutex needed
	LuaManager::InitThread();

	while (true) {
		DWORD num_bytes        = 0;
		ULONG_PTR completion_key = 0;
		EXP_OVER* exp_over     = nullptr;

		BOOL success = GetQueuedCompletionStatus(
			g_iocp, &num_bytes, &completion_key,
			(LPOVERLAPPED*)&exp_over, INFINITE);

		if (!success || exp_over == nullptr) continue;

		int client_id = static_cast<int>(completion_key);

		if (exp_over->m_iotype == IO_ACCEPT) {
			SOCKET client_socket = exp_over->m_client_socket;

			// Enforce MAX_PLAYERS cap
			if (player_index >= MAX_PLAYERS) {
				closesocket(client_socket);
				delete exp_over;
				// Re-issue AcceptEx so we keep listening
				EXP_OVER* nao = new EXP_OVER(IO_ACCEPT);
				nao->m_client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
				if (nao->m_client_socket != INVALID_SOCKET)
					AcceptEx(g_server, nao->m_client_socket,
						&nao->m_ring_buffer.buffer, 0,
						sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16,
						NULL, &nao->m_over);
				else
					delete nao;
				continue;
			}

			int my_id = player_index++;
			std::cout << "Client " << my_id << " connected\n";

			auto new_client = std::make_shared<SESSION>(client_socket, my_id);
			clients[my_id]      = new_client;
			g_player_ids[my_id] = true;

			CreateIoCompletionPort((HANDLE)client_socket, g_iocp, (ULONG_PTR)my_id, 0);
			new_client->doRecv();

			EXP_OVER* new_accept_over = new EXP_OVER(IO_ACCEPT);
			new_accept_over->m_client_socket =
				WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			if (new_accept_over->m_client_socket == INVALID_SOCKET) {
				delete new_accept_over;
			} else {
				BOOL ar = AcceptEx(g_server, new_accept_over->m_client_socket,
					&new_accept_over->m_ring_buffer.buffer, 0,
					sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16,
					NULL, &new_accept_over->m_over);
				if (!ar && WSAGetLastError() != WSA_IO_PENDING) {
					closesocket(new_accept_over->m_client_socket);
					delete new_accept_over;
				}
			}
			delete exp_over;
		}
		else if (exp_over->m_iotype == IO_RECV) {
			std::shared_ptr<SESSION> cla = clients[client_id];
			if (!cla) { delete exp_over; continue; }

			if (num_bytes == 0) {
				disconnect(client_id);
				delete exp_over;
				continue;
			}

			if (!cla->mRecvBuffer.push(exp_over->m_ring_buffer.buffer, num_bytes)) {
				std::cout << "RingBuffer overflow for client[" << client_id << "]\n";
				disconnect(client_id);
				delete exp_over;
				continue;
			}

			while (cla->mRecvBuffer.get_size() > 0) {
				if (cla->mRecvBuffer.get_size() < 1) break;
				char size_byte   = cla->mRecvBuffer.buffer[cla->mRecvBuffer.head];
				int  packet_size = static_cast<unsigned char>(size_byte);
				if (packet_size < 2 || packet_size > 256) {
					disconnect(client_id);
					break;
				}
				if (cla->mRecvBuffer.get_size() < packet_size) break;
				char packet_buffer[256];
				if (cla->mRecvBuffer.pop(packet_buffer, packet_size) == -1) {
					disconnect(client_id);
					break;
				}
				if (!cla->processPacket(reinterpret_cast<unsigned char*>(packet_buffer))) {
					disconnect(client_id);
					break;
				}
			}
			cla->doRecv();
			delete exp_over;
		}
		else if (exp_over->m_iotype == IO_SEND) {
			// Return to pool instead of deleting
			ObjectPool::ReleaseSend(exp_over);
		}
		else if (exp_over->m_iotype == IO_NPC_MOVE) {
			std::shared_ptr<SESSION> npc = clients[client_id];
			if (npc && !npc->is_player && npc->mState == CS_PLAYING)
				npc->doNpcMove();
			ObjectPool::ReleaseNpcMove(exp_over);
		}
	}
}
