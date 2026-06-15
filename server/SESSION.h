#pragma once
#include "common.h"
#include "EXP_OVER.h"
#include "pch.h"
#include "PartyManager.h"
#include "Database.h"

enum CL_STATE { CS_CONNECT, CS_CHAR_SELECT, CS_PLAYING, CS_LOGOUT };

class SESSION {
public:
	SOCKET mClient;
	int mId;
	CL_STATE mState;
	RingBuffer mRecvBuffer;
	char mUsername[MAX_NAME_LEN];
	short mX, mY;      // tile coordinates
	int mMove_time;
	int mSector_id;
	DIRECTION mDirection;
	int mHp;
	int mMaxHp;
	unsigned long long mExp;
	unsigned char mLevel;
	unsigned char mStr;
	unsigned char mIntl;
	unsigned char mDex;
	unsigned char mLuk;
	unsigned char mStatPoints;
	std::unordered_set<int> m_visible_players;
	std::unordered_set<int> m_visible_npcs;
	std::mutex m_visible_mutex;
	bool is_player;

	// Chase state (NPC only)
	int mTargetId;        // player ID being chased; -1 = wandering
	int mChaseRemaining;  // move steps left before giving up chase

	// Spawn / roam state (NPC only)
	short mSpawnX, mSpawnY; // original spawn tile; roam stays within +/-10 tiles
	bool  mIsDead;          // true while waiting for respawn timer
	bool  mIsStationary;    // true for town NPCs that never move

	// Party
	int mPartyId;         // -1 = not in a party

	// Visual
	int mVisualId;        // 0=base 1=alice 2=metalPlate 3=pickax 4=redLotus

	// Economy
	int mGold;

	// Combat (NPC only)
	int mAttackTick;   // ticks until next attack (0 = ready, counts down per doNpcMove call)

public:

	SESSION();
	SESSION(SOCKET s, int id);
	SESSION(int id, bool isPlayer);
	~SESSION();


	// packet
	void doRecv();
	void doSend(int numBytes, char* mess);
	void sendAvatarInfo();
	void sendAddPlayer(int player_id);
	void sendRemovePlayer(int player_id);
	void sendMovePacket(int mover);
	void sendStatInfo();
	void sendGoldUpdate();
	void sendBuyResult(bool success, ITEM_TYPE item, int newHp, short newX, short newY);
	void sendRespawn();
	bool processPacket(unsigned char* p);

	// call this whenever the player levels up to grant stat points
	void onLevelUp() { mStatPoints += 5; sendStatInfo(); }

	// party packets
	void sendPartyUpdate(int partyId);
	void sendPartyList();
	void givePartyExp(unsigned long long kill_exp);

	// called after character is selected to join the world
	void enterWorld();


	// sector
	bool is_visible(short x, short y);
	void get_visible_players_from_sectors(std::unordered_set<int>& visible_set);
	void get_visible_npcs_from_sectors(std::unordered_set<int>& visible_set);

	// NPC
	void sendAddNpc(int npc_id);
	void sendRemoveNpc(int npc_id);
	void doNpcMove();
};

// Sends S2C_PartyUpdate to every online member of partyId
void broadcastPartyUpdate(int partyId);

inline void disconnect(int key)
{
	std::cout << "client " << key << " disconnected" << std::endl;
	std::shared_ptr<SESSION> cla = clients[key];
	if (nullptr != cla)
	{
		// Save player data (including visual_id) before cleanup
		if (cla->is_player && cla->mState == CS_PLAYING && cla->mUsername[0] != '\0') {
			PlayerSaveData sd = {};
			strncpy_s(sd.username, cla->mUsername, MAX_NAME_LEN);
			sd.x = cla->mX; sd.y = cla->mY;
			sd.hp = cla->mHp; sd.max_hp = cla->mMaxHp;
			sd.exp = cla->mExp; sd.level = cla->mLevel;
			sd.str = cla->mStr; sd.intl = cla->mIntl;
			sd.dex = cla->mDex; sd.luk = cla->mLuk;
			sd.stat_points = cla->mStatPoints;
			sd.visual_id = cla->mVisualId;
			sd.gold = cla->mGold;
			Database::SavePlayer(sd);
		}

		cla->mState = CS_LOGOUT;
		sectors[cla->mSector_id].erase(key);

		// Party cleanup
		if (cla->mPartyId >= 0) {
			int partyId = cla->mPartyId;
			PartyManager::LeaveParty(key);
			cla->mPartyId = -1;
			broadcastPartyUpdate(partyId);
		}

		// Build REMOVE packet once
		S2C_RemoveObject removePacket;
		removePacket.size = sizeof(S2C_RemoveObject);
		removePacket.type = S2C_REMOVE_OBJECT;
		removePacket.object_id = key;

		// Notify all players that had this player visible
		// Use direct send (bypass sendRemovePlayer's set-check) to prevent ghost
		auto visible_copy = cla->m_visible_players;
		for (auto& other : visible_copy)
		{
			std::shared_ptr<SESSION> o = clients[other];
			if (nullptr == o || o->mState != CS_PLAYING) continue;
			// Remove from their visible set
			o->m_visible_mutex.lock();
			o->m_visible_players.erase(key);
			o->m_visible_mutex.unlock();
			// Send REMOVE unconditionally
			o->doSend(removePacket.size, reinterpret_cast<char*>(&removePacket));
		}
		if (cla->mClient != INVALID_SOCKET)
			closesocket(cla->mClient);
		cla->mClient = INVALID_SOCKET;
	}
	clients.unsafe_erase(key);
}