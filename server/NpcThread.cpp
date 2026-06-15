#include "pch.h"
#include "SESSION.h"
#include "ObjectPool.h"
#include "Database.h"

void npc_timer_thread()
{
	while (true) {
		std::this_thread::sleep_for(std::chrono::milliseconds(NPC_MOVE_INTERVAL));

		// Take a snapshot of active NPCs and prune those with no viewers.
		// Holds the mutex only during snapshot/prune — posting happens after.
		std::vector<int> active;
		{
			std::lock_guard<std::mutex> lock(g_active_npcs_mutex);
			std::vector<int> to_remove;
			for (int npc_id : g_active_npcs) {
				auto it = clients.find(npc_id);
				if (it == clients.end() || !it->second) {
					to_remove.push_back(npc_id);
					continue;
				}
				auto& npc = it->second;
				npc->m_visible_mutex.lock();
				bool has_viewers = !npc->m_visible_players.empty();
				npc->m_visible_mutex.unlock();
				if (has_viewers) {
					active.push_back(npc_id);
				} else {
					to_remove.push_back(npc_id);
				}
			}
			for (int id : to_remove) g_active_npcs.erase(id);
		}

		for (int npc_id : active) {
			EXP_OVER* over = ObjectPool::AcquireNpcMove();
			PostQueuedCompletionStatus(g_iocp, 1,
				static_cast<ULONG_PTR>(npc_id), &over->m_over);
		}
	}
}

void respawn_timer_thread()
{
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(30));

		int revived = 0;
		for (int i = 0; i < NUM_NPCS; ++i) {
			int npc_id = NPC_ID_START + i;
			auto it = clients.find(npc_id);
			if (it == clients.end()) continue;
			std::shared_ptr<SESSION> npc = it->second;
			if (!npc || !npc->mIsDead) continue;

			npc->mHp     = npc->mMaxHp;
			npc->mIsDead = false;
			npc->mX      = npc->mSpawnX;
			npc->mY      = npc->mSpawnY;
			npc->mTargetId       = -1;
			npc->mChaseRemaining = 0;

			int new_sector = get_sector_id(npc->mX, npc->mY);
			{
				std::lock_guard<std::mutex> lk(g_sectors_mutex);
				sectors[new_sector].insert(npc_id);
			}
			npc->mSector_id = new_sector;

			// Only iterate players (not all 200k+ clients)
			for (auto& [pid, _] : g_player_ids) {
				auto pit = clients.find(pid);
				if (pit == clients.end() || !pit->second) continue;
				auto& pl = pit->second;
				if (!pl->is_player || pl->mState != CS_PLAYING) continue;
				if (!pl->is_visible(npc->mSpawnX, npc->mSpawnY)) continue;

				pl->m_visible_mutex.lock();
				bool already = (pl->m_visible_npcs.count(npc_id) != 0);
				if (!already) pl->m_visible_npcs.insert(npc_id);
				pl->m_visible_mutex.unlock();

				npc->m_visible_mutex.lock();
				npc->m_visible_players.insert(pid);
				npc->m_visible_mutex.unlock();

				if (!already) {
					// Mark NPC active
					{
						std::lock_guard<std::mutex> lock(g_active_npcs_mutex);
						g_active_npcs.insert(npc_id);
					}
					// Send AddObject directly — visibility sets already updated above
					S2C_AddObject pkt;
					pkt.size      = sizeof(S2C_AddObject);
					pkt.type      = S2C_ADD_OBJECT;
					pkt.object_id = npc_id;
					pkt.visual_id = npc->mVisualId;
					memcpy(pkt.obj_name, npc->mUsername, MAX_NAME_LEN);
					pkt.x         = npc->mX;
					pkt.y         = npc->mY;
					pkt.hp        = npc->mHp;
					pkt.max_hp    = npc->mMaxHp;
					pkt.exp       = npc->mExp;
					pkt.level     = npc->mLevel;
					pl->doSend(pkt.size, reinterpret_cast<char*>(&pkt));
				}
			}
			++revived;
		}
		if (revived > 0)
			std::cout << "[Respawn] Revived " << revived << " NPC(s).\n";
	}
}

void db_save_thread()
{
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(10));

		// Drain disconnect saves first — these arrive async so worker threads don't block on DB
		{
			PlayerSaveData sd;
			int flushed = 0;
			while (g_pending_saves.try_pop(sd)) {
				Database::SavePlayer(sd);
				++flushed;
			}
			if (flushed > 0)
				std::cout << "[AutoSave] Flushed " << flushed << " disconnect save(s).\n";
		}

		int saved = 0;
		// Iterate only player IDs — skips all 200k NPC entries
		for (auto& [id, _] : g_player_ids) {
			auto it = clients.find(id);
			if (it == clients.end() || !it->second) continue;
			auto& session = it->second;
			if (!session->is_player || session->mState != CS_PLAYING) continue;

			PlayerSaveData data = {};
			strncpy_s(data.username, session->mUsername, 20);
			data.x   = session->mX;   data.y         = session->mY;
			data.hp  = session->mHp;  data.max_hp    = session->mMaxHp;
			data.exp = session->mExp; data.level      = session->mLevel;
			data.str = session->mStr; data.intl       = session->mIntl;
			data.dex = session->mDex; data.luk        = session->mLuk;
			data.stat_points = session->mStatPoints;
			data.visual_id   = session->mVisualId;
			data.gold        = session->mGold;

			if (Database::SavePlayer(data)) ++saved;
		}
		if (saved > 0)
			std::cout << "[AutoSave] Saved " << saved << " player(s).\n";
	}
}
