#include "common.h"
#include "SESSION.h"
#include "Database.h"
#include "LuaManager.h"
#include "PartyManager.h"

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
	std::wcout << L" === ���� " << lpMsgBuf << std::endl;
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
	mDirection = DOWN;
	is_player = true;
	mHp = 100; mMaxHp = 100; mExp = 0; mLevel = 1;
	mStr = 5; mIntl = 5; mDex = 5; mLuk = 5; mStatPoints = 0;
	mPartyId = -1;
	mVisualId = 0;
}

SESSION::SESSION(int id, bool isPlayer)
{
	mClient = INVALID_SOCKET;
	mId = id;
	mState = CS_CONNECT;
	mX = 1000;
	mY = 1000;
	mMove_time = 0;
	mSector_id = 0;
	mDirection = DOWN;
	is_player = isPlayer;
	mHp = 100; mMaxHp = 100; mExp = 0; mLevel = 1;
	mStr = 5; mIntl = 5; mDex = 5; mLuk = 5; mStatPoints = 0;
	mTargetId = -1; mChaseRemaining = 0;
	mPartyId = -1;
	mVisualId = 0;
}

SESSION::~SESSION()
{
	if(mClient != INVALID_SOCKET)
		closesocket(mClient);
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
	packet.visualId = mVisualId;
	packet.x = mX;
	packet.y = mY;
	packet.exp = mExp;
	packet.level = mLevel;
	packet.hp = mHp;
	packet.max_hp = mMaxHp;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
	sendStatInfo();
}

void SESSION::sendStatInfo()
{
	S2C_StatInfo packet;
	packet.size = sizeof(S2C_StatInfo);
	packet.type = S2C_STAT_INFO;
	packet.object_id = mId;
	packet.str = mStr;
	packet.intl = mIntl;
	packet.dex = mDex;
	packet.luk = mLuk;
	packet.stat_points = mStatPoints;
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
	packet.visual_id = pl->mVisualId;
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

		// Null-terminate for safety
		packet->username[MAX_NAME_LEN - 1] = '\0';
		packet->password[MAX_NAME_LEN - 1] = '\0';

		PlayerSaveData saveData;
		int dbRes = Database::Login(packet->username, packet->password, saveData);

		S2C_LoginResult result;
		result.size = sizeof(S2C_LoginResult);
		result.type = S2C_LOGIN_RESULT;

		if (dbRes == DBR_WRONG_PW) {
			result.result = LOGIN_WRONG_PW;
			strncpy_s(result.message, "Wrong password.", sizeof(result.message));
			doSend(result.size, reinterpret_cast<char*>(&result));
			return true;
		}
		if (dbRes == DBR_FAIL) {
			result.result = LOGIN_DB_ERROR;
			strncpy_s(result.message, "Server DB error.", sizeof(result.message));
			doSend(result.size, reinterpret_cast<char*>(&result));
			return true;
		}

		// Load data from DB into session
		strncpy_s(mUsername, saveData.username, MAX_NAME_LEN);
		mX = saveData.x; mY = saveData.y;
		mHp = saveData.hp; mMaxHp = saveData.max_hp;
		mExp = saveData.exp; mLevel = saveData.level;
		mStr = saveData.str; mIntl = saveData.intl;
		mDex = saveData.dex; mLuk = saveData.luk;
		mStatPoints = saveData.stat_points;

		if (dbRes == DBR_NEW_USER) {
			result.result = LOGIN_NEW_USER;
			strncpy_s(result.message, "Welcome! New account created.", sizeof(result.message));
		} else {
			result.result = LOGIN_SUCCESS;
			strncpy_s(result.message, "Welcome back!", sizeof(result.message));
		}
		mVisualId = saveData.visual_id;

		doSend(result.size, reinterpret_cast<char*>(&result));

		std::cout << "Player[" << mId << "] logged in as " << mUsername << std::endl;

		if (dbRes == DBR_NEW_USER) {
			// New account — wait for the client to pick a character
			mState = CS_CHAR_SELECT;
		} else {
			// Existing user — enter the world immediately with saved visual_id
			enterWorld();
		}
	}
	break;
	case C2S_CHAR_SELECT:
	{
		if (mState != CS_CHAR_SELECT) break;
		C2S_CharSelect* pkt = reinterpret_cast<C2S_CharSelect*>(p);
		mVisualId = pkt->visual_id;
		std::cout << "Player[" << mId << "] chose visual " << (int)mVisualId << std::endl;

		// Persist the choice immediately
		PlayerSaveData sd = {};
		strncpy_s(sd.username, mUsername, MAX_NAME_LEN);
		sd.x = mX; sd.y = mY;
		sd.hp = mHp; sd.max_hp = mMaxHp;
		sd.exp = mExp; sd.level = mLevel;
		sd.str = mStr; sd.intl = mIntl; sd.dex = mDex; sd.luk = mLuk;
		sd.stat_points = mStatPoints;
		sd.visual_id = mVisualId;
		Database::SavePlayer(sd);

		enterWorld();
	}
	break;
	case C2S_MOVE:
	{
		C2S_Move* packet = reinterpret_cast<C2S_Move*>(p);
		mX = packet->x;
		mY = packet->y;
		mMove_time = packet->move_time;
		mDirection = packet->dir;

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

		// Handle NPC visibility changes
		{
			m_visible_mutex.lock();
			auto old_v_npcs = m_visible_npcs;
			m_visible_mutex.unlock();

			std::unordered_set<int> new_v_npcs;
			get_visible_npcs_from_sectors(new_v_npcs);

			for (int npc_id : new_v_npcs) {
				if (old_v_npcs.count(npc_id) == 0) sendAddNpc(npc_id);
			}
			for (int npc_id : old_v_npcs) {
				if (new_v_npcs.count(npc_id) == 0) sendRemoveNpc(npc_id);
			}
		}
	}
	break;
	case C2S_STAT_INVEST:
	{
		if (mStatPoints == 0) break;
		C2S_StatInvest* packet = reinterpret_cast<C2S_StatInvest*>(p);
		switch (packet->stat_type)
		{
		case STAT_STR: mStr++;  break;
		case STAT_INT: mIntl++; break;
		case STAT_DEX: mDex++;  break;
		case STAT_LUK: mLuk++;  break;
		default: break;
		}
		mStatPoints--;
		sendStatInfo();
		std::cout << "Player[" << mId << "] invested in stat " << (int)packet->stat_type
			<< " (points left: " << (int)mStatPoints << ")\n";
	}
	break;
	case C2S_ATTACK:
	{
		// Compute direction delta
		short dx = 0, dy = 0;
		switch (mDirection) {
		case UP:    dy = -1; break;
		case DOWN:  dy =  1; break;
		case LEFT:  dx = -1; break;
		case RIGHT: dx =  1; break;
		}

		// Find closest target in front (up to 2 tiles)
		int target_id = -1;
		int sectors_x = (WORLD_WIDTH / SECTOR_SIZE) + 1;
		for (int r = 1; r <= 2 && target_id == -1; ++r) {
			short tx = mX + static_cast<short>(dx * r);
			short ty = mY + static_cast<short>(dy * r);
			if (tx < 0 || tx >= WORLD_WIDTH || ty < 0 || ty >= WORLD_HEIGHT) break;
			int sid = get_sector_id(tx, ty);
			auto sit = sectors.find(sid);
			if (sit == sectors.end()) continue;
			for (int id : sit->second) {
				if (id == mId) continue;
				auto cit = clients.find(id);
				if (cit == clients.end()) continue;
				std::shared_ptr<SESSION> tgt = cit->second;
				if (!tgt || tgt->mState != CS_PLAYING) continue;
				if (tgt->mX == tx && tgt->mY == ty) { target_id = id; break; }
			}
		}

		if (target_id == -1) break;

		std::shared_ptr<SESSION> target = clients[target_id];
		if (!target) break;

		// Damage: STR * 100, crit = LUK% chance -> x2
		int damage = static_cast<int>(mStr) * 100;
		bool is_crit = (rand() % 100) < static_cast<int>(mLuk);
		if (is_crit) damage *= 2;

		target->mHp -= damage;

		// Players cannot die: floor at 1
		if (target->mHp <= 0 && target->is_player) target->mHp = 1;

		bool killed = (target->mHp <= 0 && !target->is_player);
		if (killed) target->mHp = 0;

		std::cout << "[Attack] Player[" << mId << "] -> [" << target_id << "]: "
			<< damage << (is_crit ? " CRIT" : "") << (killed ? " (killed)" : "") << "\n";

		if (killed) {
			// Notify everyone who could see this NPC
			target->m_visible_mutex.lock();
			auto watchers = target->m_visible_players;
			target->m_visible_players.clear();
			target->m_visible_mutex.unlock();

			S2C_RemoveObject rp;
			rp.size = sizeof(S2C_RemoveObject);
			rp.type = S2C_REMOVE_OBJECT;
			rp.object_id = target_id;
			for (int pid : watchers) {
				auto pit = clients.find(pid);
				if (pit == clients.end()) continue;
				std::shared_ptr<SESSION> pl = pit->second;
				if (!pl || pl->mState != CS_PLAYING) continue;
				pl->m_visible_mutex.lock();
				pl->m_visible_npcs.erase(target_id);
				pl->m_visible_mutex.unlock();
				pl->doSend(rp.size, reinterpret_cast<char*>(&rp));
			}

			// Respawn NPC with full HP (stays in place, reappears on next player move)
			target->mHp = target->mMaxHp;

			// Give EXP: lv1=10, each level +5
			unsigned long long kill_exp = static_cast<unsigned long long>(10 + (static_cast<int>(target->mLevel) - 1) * 5);
			mExp += kill_exp;
			// Level-up threshold: lv1=100, each level +20
			while (mLevel < 100) {
				unsigned long long required = 100ULL + static_cast<unsigned long long>(mLevel - 1) * 20ULL;
				if (mExp < required) break;
				mExp -= required;
				mLevel++;
				mStatPoints += 5;
			}
			sendAvatarInfo();
			givePartyExp(kill_exp);
		} else {
			// Broadcast HP change to everyone watching the target
			S2C_StatusChange sc;
			sc.size = sizeof(S2C_StatusChange);
			sc.type = S2C_STATUS_CHANGE;
			sc.object_id = target_id;
			sc.hp = target->mHp;
			sc.max_hp = target->mMaxHp;
			sc.exp = target->mExp;
			sc.level = target->mLevel;

			target->m_visible_mutex.lock();
			auto watchers = target->m_visible_players;
			target->m_visible_mutex.unlock();

			for (int pid : watchers) {
				auto pit = clients.find(pid);
				if (pit == clients.end()) continue;
				std::shared_ptr<SESSION> pl = pit->second;
				if (!pl || pl->mState != CS_PLAYING) continue;
				pl->doSend(sc.size, reinterpret_cast<char*>(&sc));
			}

			// If target is a player, send to target itself too
			if (target->is_player)
				target->doSend(sc.size, reinterpret_cast<char*>(&sc));

			// NPC hit but not killed: start chasing attacker
			if (!target->is_player) {
				target->mTargetId      = mId;
				target->mChaseRemaining = 20;
			}
		}
	}
	break;
	case C2S_AOE_ATTACK:
	{
		static const short dx_off[] = { -1,  0,  1, -1, 1, -1, 0, 1 };
		static const short dy_off[] = { -1, -1, -1,  0, 0,  1, 1, 1 };

		std::vector<int> targets;
		for (int i = 0; i < 8; ++i) {
			short tx = mX + dx_off[i];
			short ty = mY + dy_off[i];
			if (tx < 0 || tx >= WORLD_WIDTH || ty < 0 || ty >= WORLD_HEIGHT) continue;
			int sid = get_sector_id(tx, ty);
			auto sit = sectors.find(sid);
			if (sit == sectors.end()) continue;
			for (int id : sit->second) {
				if (id == mId) continue;
				auto cit = clients.find(id);
				if (cit == clients.end()) continue;
				std::shared_ptr<SESSION> tgt = cit->second;
				if (!tgt || tgt->mState != CS_PLAYING) continue;
				if (tgt->mX == tx && tgt->mY == ty)
					targets.push_back(id);
			}
		}

		bool stat_changed = false;
		for (int target_id : targets) {
			std::shared_ptr<SESSION> target = clients[target_id];
			if (!target) continue;

			int damage = static_cast<int>(mStr) * 150;
			bool is_crit = (rand() % 100) < static_cast<int>(mLuk);
			if (is_crit) damage *= 2;

			target->mHp -= damage;
			if (target->mHp <= 0 && target->is_player) target->mHp = 1;
			bool killed = (target->mHp <= 0 && !target->is_player);
			if (killed) target->mHp = 0;

			if (killed) {
				target->m_visible_mutex.lock();
				auto watchers = target->m_visible_players;
				target->m_visible_players.clear();
				target->m_visible_mutex.unlock();

				S2C_RemoveObject rp;
				rp.size = sizeof(S2C_RemoveObject);
				rp.type = S2C_REMOVE_OBJECT;
				rp.object_id = target_id;
				for (int pid : watchers) {
					auto pit = clients.find(pid);
					if (pit == clients.end()) continue;
					std::shared_ptr<SESSION> pl = pit->second;
					if (!pl || pl->mState != CS_PLAYING) continue;
					pl->m_visible_mutex.lock();
					pl->m_visible_npcs.erase(target_id);
					pl->m_visible_mutex.unlock();
					pl->doSend(rp.size, reinterpret_cast<char*>(&rp));
				}
				target->mHp = target->mMaxHp;

				// Give EXP: lv1=10, each level +5
				unsigned long long kill_exp = static_cast<unsigned long long>(10 + (static_cast<int>(target->mLevel) - 1) * 5);
				mExp += kill_exp;
				// Level-up threshold: lv1=100, each level +20
				while (mLevel < 100) {
					unsigned long long required = 100ULL + static_cast<unsigned long long>(mLevel - 1) * 20ULL;
					if (mExp < required) break;
					mExp -= required;
					mLevel++;
					mStatPoints += 5;
				}
				givePartyExp(kill_exp);
				stat_changed = true;
			} else {
				S2C_StatusChange sc;
				sc.size = sizeof(S2C_StatusChange);
				sc.type = S2C_STATUS_CHANGE;
				sc.object_id = target_id;
				sc.hp = target->mHp;
				sc.max_hp = target->mMaxHp;
				sc.exp = target->mExp;
				sc.level = target->mLevel;

				target->m_visible_mutex.lock();
				auto watchers = target->m_visible_players;
				target->m_visible_mutex.unlock();
				for (int pid : watchers) {
					auto pit = clients.find(pid);
					if (pit == clients.end()) continue;
					std::shared_ptr<SESSION> pl = pit->second;
					if (!pl || pl->mState != CS_PLAYING) continue;
					pl->doSend(sc.size, reinterpret_cast<char*>(&sc));
				}
				if (target->is_player)
					target->doSend(sc.size, reinterpret_cast<char*>(&sc));

				// NPC hit but not killed: start chasing attacker
				if (!target->is_player) {
					target->mTargetId       = mId;
					target->mChaseRemaining = 20;
				}
			}
		}

		if (stat_changed) sendAvatarInfo();
	}
	break;
	case C2S_CHAT:
	{
		C2S_Chat* packet = reinterpret_cast<C2S_Chat*>(p);

		// null-terminate for safety
		packet->message[MAX_CHAT_MSG_LEN - 1] = '\0';

		S2C_ChatMessage chatPacket;
		chatPacket.size = sizeof(S2C_ChatMessage);
		chatPacket.type = S2C_CHAT_MESSAGE;
		chatPacket.object_id = mId;
		strncpy_s(chatPacket.message, packet->message, static_cast<size_t>(MAX_CHAT_MSG_LEN - 1));
		chatPacket.message[MAX_CHAT_MSG_LEN - 1] = '\0';

		// echo to self
		doSend(chatPacket.size, reinterpret_cast<char*>(&chatPacket));

		// broadcast to visible players
		m_visible_mutex.lock();
		auto visible_copy = m_visible_players;
		m_visible_mutex.unlock();

		for (int id : visible_copy) {
			auto it = clients.find(id);
			if (it == clients.end()) continue;
			std::shared_ptr<SESSION> pl = it->second;
			if (nullptr == pl || pl->mState != CS_PLAYING) continue;
			pl->doSend(chatPacket.size, reinterpret_cast<char*>(&chatPacket));
		}

		std::cout << "[Chat] " << mUsername << ": " << packet->message << std::endl;
	}
	break;
	case C2S_PARTY_CREATE:
	{
		if (mPartyId >= 0) break; // already in a party
		int newId = PartyManager::CreateParty(mId);
		if (newId < 0) break;
		mPartyId = newId;
		sendPartyUpdate(newId); // just inform self (party of 1)
		std::cout << "Player[" << mId << "] created party " << newId << "\n";
	}
	break;
	case C2S_PARTY_JOIN:
	{
		if (mPartyId >= 0) break; // already in a party
		C2S_PartyJoin* pkt = reinterpret_cast<C2S_PartyJoin*>(p);
		int targetParty = pkt->party_id;
		if (!PartyManager::JoinParty(targetParty, mId)) break;
		mPartyId = targetParty;
		broadcastPartyUpdate(targetParty);
		std::cout << "Player[" << mId << "] joined party " << targetParty << "\n";
	}
	break;
	case C2S_PARTY_LEAVE:
	{
		if (mPartyId < 0) break;
		int oldParty = mPartyId;
		PartyManager::LeaveParty(mId);
		mPartyId = -1;
		// Tell self: no longer in a party
		{
			S2C_PartyUpdate out{};
			out.size = sizeof(S2C_PartyUpdate);
			out.type = S2C_PARTY_UPDATE;
			out.party_id = -1;
			out.member_count = 0;
			doSend(out.size, reinterpret_cast<char*>(&out));
		}
		broadcastPartyUpdate(oldParty);
		std::cout << "Player[" << mId << "] left party " << oldParty << "\n";
	}
	break;
	case C2S_PARTY_LIST_REQ:
	{
		sendPartyList();
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

void SESSION::get_visible_npcs_from_sectors(std::unordered_set<int>& visible_set)
{
	int sectors_x = (WORLD_WIDTH / SECTOR_SIZE) + 1;
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
				if (is_visible(npc->mX, npc->mY)) visible_set.insert(id);
			}
		}
	}
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

	// Map NPC name → monster visual_id (must match client MON_* enum)
	// 0=Dog  1=Skeleton(Small)  2=Skel.Knight(Big_Normal)  3=Skel.Mage(Magician_Ice)
	int monVisual = 0;
	const char* nm = npc->mUsername;
	if (strcmp(nm, "Dog") == 0)          monVisual = 0;
	else if (strcmp(nm, "Skeleton") == 0)    monVisual = 1;
	else if (strcmp(nm, "Skel.Knight") == 0) monVisual = 2;
	else if (strcmp(nm, "Skel.Mage") == 0)   monVisual = 3;

	S2C_AddObject packet;
	packet.size = sizeof(S2C_AddObject);
	packet.type = S2C_ADD_OBJECT;
	packet.object_id = npc_id;
	packet.visual_id = monVisual;
	memcpy(packet.obj_name, npc->mUsername, MAX_NAME_LEN);
	packet.x = npc->mX;
	packet.y = npc->mY;
	packet.hp = npc->mHp;
	packet.max_hp = npc->mMaxHp;
	packet.exp = npc->mExp;
	packet.level = npc->mLevel;
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
	packet.size = sizeof(S2C_RemoveObject);
	packet.type = S2C_REMOVE_OBJECT;
	packet.object_id = npc_id;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::doNpcMove()
{
	thread_local std::mt19937 rng(std::random_device{}());
	thread_local std::uniform_int_distribution<int> dir_dist(0, 3);

	short newX = mX, newY = mY;

	if (mTargetId >= 0) {
		// Chase mode: use Lua A*
		auto it = clients.find(mTargetId);
		bool valid = (it != clients.end() && it->second && it->second->mState == CS_PLAYING);
		if (!valid) {
			// Target disconnected - back to wander
			mTargetId = -1;
			mChaseRemaining = 0;
		} else {
			auto tgt = it->second;
			short dx = 0, dy = 0;
			if (LuaManager::GetNextStep(mX, mY, tgt->mX, tgt->mY, dx, dy)) {
				newX = mX + dx;
				newY = mY + dy;
			}
			if (--mChaseRemaining <= 0) {
				mTargetId = -1;  // Give up chase
				mChaseRemaining = 0;
			}
		}
	}

	if (mTargetId < 0) {
		// Wander mode: random direction
		int dir = dir_dist(rng);
		newX = mX; newY = mY;
		switch (dir) {
		case 0: if (newY > 0)              newY--; break;
		case 1: if (newY < WORLD_HEIGHT-1) newY++; break;
		case 2: if (newX > 0)              newX--; break;
		case 3: if (newX < WORLD_WIDTH-1)  newX++; break;
		}
	}

	// Clamp to world bounds
	if (newX < 0) newX = 0;
	if (newX >= WORLD_WIDTH)  newX = WORLD_WIDTH  - 1;
	if (newY < 0) newY = 0;
	if (newY >= WORLD_HEIGHT) newY = WORLD_HEIGHT - 1;

	if (newX == mX && newY == mY) return;
	mX = newX;
	mY = newY;

	int new_sector_id = get_sector_id(mX, mY);
	if (new_sector_id != mSector_id) {
		sectors[mSector_id].erase(mId);
		sectors[new_sector_id].insert(mId);
		mSector_id = new_sector_id;
	}

	S2C_MoveObject mp;
	mp.size = sizeof(S2C_MoveObject);
	mp.type = S2C_MOVE_OBJECT;
	mp.object_id = mId;
	mp.x = mX;
	mp.y = mY;
	mp.move_time = NPC_MOVE_INTERVAL;

	m_visible_mutex.lock();
	auto visible_copy = m_visible_players;
	m_visible_mutex.unlock();

	for (int pid : visible_copy) {
		auto it = clients.find(pid);
		if (it == clients.end()) continue;
		std::shared_ptr<SESSION> pl = it->second;
		if (!pl || pl->mState != CS_PLAYING) continue;

		if (!is_visible(pl->mX, pl->mY)) {
			// Player moved out of NPC's range
			m_visible_mutex.lock();
			m_visible_players.erase(pid);
			m_visible_mutex.unlock();

			pl->m_visible_mutex.lock();
			pl->m_visible_npcs.erase(mId);
			pl->m_visible_mutex.unlock();

			S2C_RemoveObject rp;
			rp.size = sizeof(S2C_RemoveObject);
			rp.type = S2C_REMOVE_OBJECT;
			rp.object_id = mId;
			pl->doSend(rp.size, reinterpret_cast<char*>(&rp));
		}
		else {
			pl->doSend(mp.size, reinterpret_cast<char*>(&mp));
		}
	}
}

// ---------------------------------------------------------------------------
// Party helpers
// ---------------------------------------------------------------------------

static void fillPartyUpdatePacket(S2C_PartyUpdate& out, int partyId)
{
	auto members = PartyManager::GetMembers(partyId);
	out.size = sizeof(S2C_PartyUpdate);
	out.type = S2C_PARTY_UPDATE;
	out.party_id = partyId;
	out.member_count = static_cast<unsigned char>(min((int)members.size(), 4));
	for (int i = 0; i < (int)out.member_count; ++i) {
		int mid = members[i];
		out.members[i].player_id = mid;
		auto it = clients.find(mid);
		if (it != clients.end() && it->second) {
			auto& m = it->second;
			strncpy_s(out.members[i].name, m->mUsername, MAX_NAME_LEN - 1);
			out.members[i].name[MAX_NAME_LEN - 1] = '\0';
			out.members[i].hp      = m->mHp;
			out.members[i].max_hp  = m->mMaxHp;
			out.members[i].level   = m->mLevel;
		}
	}
}

void broadcastPartyUpdate(int partyId)
{
	S2C_PartyUpdate out{};
	fillPartyUpdatePacket(out, partyId);
	auto members = PartyManager::GetMembers(partyId);
	for (int mid : members) {
		auto it = clients.find(mid);
		if (it == clients.end() || !it->second) continue;
		auto& pl = it->second;
		if (!pl->is_player || pl->mState != CS_PLAYING) continue;
		pl->doSend(out.size, reinterpret_cast<char*>(&out));
	}
}

void SESSION::sendPartyUpdate(int partyId)
{
	S2C_PartyUpdate out{};
	fillPartyUpdatePacket(out, partyId);
	doSend(out.size, reinterpret_cast<char*>(&out));
}

void SESSION::sendPartyList()
{
	auto allParties = PartyManager::GetAll();
	S2C_PartyList out{};
	out.size = sizeof(S2C_PartyList);
	out.type = S2C_PARTY_LIST;
	out.party_count = static_cast<unsigned char>(min((int)allParties.size(), 8));
	for (int i = 0; i < (int)out.party_count; ++i) {
		auto& party = allParties[i];
		out.entries[i].party_id     = party.party_id;
		out.entries[i].member_count = static_cast<unsigned char>(party.member_ids.size());
		if (!party.member_ids.empty()) {
			int leaderId = party.member_ids[0];
			auto it = clients.find(leaderId);
			if (it != clients.end() && it->second) {
				strncpy_s(out.entries[i].leader_name, it->second->mUsername, MAX_NAME_LEN - 1);
				out.entries[i].leader_name[MAX_NAME_LEN - 1] = '\0';
			}
		}
	}
	doSend(out.size, reinterpret_cast<char*>(&out));
}

void SESSION::givePartyExp(unsigned long long kill_exp)
{
	unsigned long long bonus = kill_exp / 2;
	if (bonus == 0 || mPartyId < 0) return;

	auto members = PartyManager::GetMembers(mPartyId);
	for (int memberId : members) {
		if (memberId == mId) continue;
		auto it = clients.find(memberId);
		if (it == clients.end() || !it->second) continue;
		auto& member = it->second;
		if (!member->is_player || member->mState != CS_PLAYING) continue;

		member->mExp += bonus;
		while (member->mLevel < 100) {
			unsigned long long req = 100ULL + static_cast<unsigned long long>(member->mLevel - 1) * 20ULL;
			if (member->mExp < req) break;
			member->mExp -= req;
			member->mLevel++;
			member->mStatPoints += 5;
		}
		member->sendAvatarInfo();
	}
}

void SESSION::enterWorld()
{
	int initial_sector_id = get_sector_id(mX, mY);
	sectors[initial_sector_id].insert(mId);
	mSector_id = initial_sector_id;

	sendAvatarInfo();
	mState = CS_PLAYING;

	std::unordered_set<int> new_v_players;
	get_visible_players_from_sectors(new_v_players);
	for (int id : new_v_players) {
		sendAddPlayer(id);
		std::shared_ptr<SESSION> pl = clients[id];
		if (nullptr == pl) continue;
		pl->sendAddPlayer(mId);
	}

	std::unordered_set<int> visible_npcs;
	get_visible_npcs_from_sectors(visible_npcs);
	for (int npc_id : visible_npcs)
		sendAddNpc(npc_id);
}
