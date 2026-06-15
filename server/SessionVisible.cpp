#include "pch.h"
#include "SESSION.h"

bool SESSION::is_visible(short x, short y)
{
	return abs(mX - x) <= VIEW_RANGE
		&& abs(mY - y) <= VIEW_RANGE;
}

void SESSION::get_visible_players_from_sectors(std::unordered_set<int>& visible_set)
{
	int sectors_x   = (WORLD_WIDTH / SECTOR_SIZE) + 1;
	int my_sector_x = mSector_id % sectors_x;
	int my_sector_y = mSector_id / sectors_x;

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			int sector_x = my_sector_x + dx;
			int sector_y = my_sector_y + dy;
			if (sector_x < 0 || sector_x >= sectors_x ||
				sector_y < 0 || sector_y >= sectors_x) continue;
			int sector_id = sector_y * sectors_x + sector_x;
			auto it = sectors.find(sector_id);
			if (it == sectors.end()) continue;
			for (int player_id : it->second) {
				std::shared_ptr<SESSION> pl = clients[player_id];
				if (!pl || pl->mId == mId) continue;
				if (!pl->is_player || pl->mState != CS_PLAYING) continue;
				if (pl->mDungeonInstanceId != mDungeonInstanceId) continue;
				if (is_visible(pl->mX, pl->mY)) visible_set.insert(player_id);
			}
		}
	}
}

void SESSION::get_visible_npcs_from_sectors(std::unordered_set<int>& visible_set)
{
	int sectors_x   = (WORLD_WIDTH / SECTOR_SIZE) + 1;
	int my_sector_x = mSector_id % sectors_x;
	int my_sector_y = mSector_id / sectors_x;

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			int sx = my_sector_x + dx;
			int sy = my_sector_y + dy;
			if (sx < 0 || sx >= sectors_x || sy < 0 || sy >= sectors_x) continue;
			int sector_id = sy * sectors_x + sx;
			auto it = sectors.find(sector_id);
			if (it == sectors.end()) continue;
			for (int id : it->second) {
				if (id < NPC_ID_START) continue;
				auto cit = clients.find(id);
				if (cit == clients.end()) continue;
				std::shared_ptr<SESSION> npc = cit->second;
				if (!npc || npc->mState != CS_PLAYING) continue;
				// Dungeon instance filtering
				if (mDungeonInstanceId >= 0) {
					// Dungeon player: only see NPCs in the same instance
					if (npc->mDungeonInstanceId != mDungeonInstanceId) continue;
				} else {
					// World player: only see world NPCs
					if (npc->mDungeonInstanceId >= 0) continue;
				}
				if (is_visible(npc->mX, npc->mY)) visible_set.insert(id);
			}
		}
	}
}

void SESSION::sendAddPlayer(int player_id)
{
	std::shared_ptr<SESSION> pl = clients[player_id];
	if (!pl) return;

	S2C_AddObject packet;
	packet.size      = sizeof(S2C_AddObject);
	packet.type      = S2C_ADD_OBJECT;
	packet.object_id = player_id;
	packet.visual_id = pl->mVisualId;
	memcpy(packet.obj_name, pl->mUsername, sizeof(packet.obj_name));
	packet.x         = pl->mX;
	packet.y         = pl->mY;
	packet.exp       = pl->mExp;
	packet.level     = pl->mLevel;
	packet.hp        = pl->mHp;
	packet.max_hp    = pl->mMaxHp;

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
	packet.size      = sizeof(S2C_RemoveObject);
	packet.type      = S2C_REMOVE_OBJECT;
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
	std::shared_ptr<SESSION> pl = clients[mover];
	if (!pl) return;

	S2C_MoveObject packet;
	packet.size      = sizeof(S2C_MoveObject);
	packet.type      = S2C_MOVE_OBJECT;
	packet.object_id = mover;
	packet.x         = pl->mX;
	packet.y         = pl->mY;
	packet.move_time = pl->mMove_time;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::sendAddNpc(int npc_id)
{
	auto it = clients.find(npc_id);
	if (it == clients.end()) return;
	std::shared_ptr<SESSION> npc = it->second;
	if (!npc || npc->mState != CS_PLAYING) return;

	m_visible_mutex.lock();
	if (m_visible_npcs.count(npc_id) > 0) {
		m_visible_mutex.unlock();
		return;
	}
	m_visible_npcs.insert(npc_id);
	m_visible_mutex.unlock();

	npc->m_visible_mutex.lock();
	npc->m_visible_players.insert(mId);
	npc->m_visible_mutex.unlock();

	// Register NPC as active so npc_timer_thread picks it up
	{
		std::lock_guard<std::mutex> lock(g_active_npcs_mutex);
		g_active_npcs.insert(npc_id);
	}

	S2C_AddObject packet;
	packet.size      = sizeof(S2C_AddObject);
	packet.type      = S2C_ADD_OBJECT;
	packet.object_id = npc_id;
	packet.visual_id = npc->mVisualId;
	memcpy(packet.obj_name, npc->mUsername, MAX_NAME_LEN);
	packet.x         = npc->mX;
	packet.y         = npc->mY;
	packet.hp        = npc->mHp;
	packet.max_hp    = npc->mMaxHp;
	packet.exp       = npc->mExp;
	packet.level     = npc->mLevel;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::sendRemoveNpc(int npc_id)
{
	m_visible_mutex.lock();
	if (m_visible_npcs.count(npc_id) == 0) {
		m_visible_mutex.unlock();
		return;
	}
	m_visible_npcs.erase(npc_id);
	m_visible_mutex.unlock();

	auto it = clients.find(npc_id);
	if (it != clients.end() && it->second) {
		it->second->m_visible_mutex.lock();
		it->second->m_visible_players.erase(mId);
		it->second->m_visible_mutex.unlock();
	}

	S2C_RemoveObject packet;
	packet.size      = sizeof(S2C_RemoveObject);
	packet.type      = S2C_REMOVE_OBJECT;
	packet.object_id = npc_id;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
}
