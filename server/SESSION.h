#pragma once
#include "common.h"
#include "EXP_OVER.h"
#include "pch.h"
#include "PartyManager.h"

enum CL_STATE { CS_CONNECT, CS_PLAYING, CS_LOGOUT };

class SESSION {
public:
	SOCKET mClient;
	int mId;
	CL_STATE mState;
	RingBuffer mRecvBuffer;
	char mUsername[MAX_NAME_LEN];
	short mX, mY;
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

	// Party
	int mPartyId;         // -1 = not in a party

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
	bool processPacket(unsigned char* p);

	// call this whenever the player levels up to grant stat points
	void onLevelUp() { mStatPoints += 5; sendStatInfo(); }

	// party packets
	void sendPartyUpdate(int partyId);
	void sendPartyList();
	void givePartyExp(unsigned long long kill_exp);


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
		cla->mState = CS_LOGOUT;
		sectors[cla->mSector_id].erase(key);

		// Party cleanup
		if (cla->mPartyId >= 0) {
			int partyId = cla->mPartyId;
			PartyManager::LeaveParty(key);
			cla->mPartyId = -1;
			broadcastPartyUpdate(partyId);
		}
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