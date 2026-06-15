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
	int mPotionCount    = 0;  // HP potions in inventory
	int mScrollCount    = 0;  // teleport scrolls in inventory
	int mWeaponEnhance  = 0;  // weapon enhancement level (0-25)

	// Combat (NPC only)
	int mAttackTick;   // ticks until next attack (0 = ready, counts down per doNpcMove call)

	// Dungeon instance (-1 = main world)
	int mDungeonInstanceId = -1;

	// GM cheat: player cannot take damage when true
	bool mInvincible = false;

	// Safe-zone regen (GetTickCount64 tick of last heal; 0 = not yet started)
	DWORD mLastRegenTick = 0;

	// Quest progress (player only)
	struct QuestEntry {
		unsigned char state    = 0;  // 0=none 1=in_progress 2=complete 3=done
		unsigned char progress = 0;
		unsigned char goal     = 0;
	};
	QuestEntry mQuests[2];  // [0]=tutorial (talk)  [1]=kill quest

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
	void sendBuyResult(bool success, ITEM_TYPE item, int itemCount, short unused_x, short unused_y);
	void sendUseItemResult(bool success, ITEM_TYPE item, int itemCount, int newHp, short newX, short newY);
	void sendRespawn();
	void sendQuestUpdate(int questId);
	void onMonsterKilled();
	bool processPacket(unsigned char* p);
	void CheckSafeZoneRegen();

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

// Active NPC tracking (NPCs with at least one viewer) — iterated by npc_timer_thread
extern std::mutex           g_active_npcs_mutex;
extern std::unordered_set<int> g_active_npcs;

// Player ID set — for fast player-only iteration (db_save, respawn notification)
extern tbb::concurrent_unordered_map<int, bool> g_player_ids;

// Sends S2C_PartyUpdate to every online member of partyId
void broadcastPartyUpdate(int partyId);

// Defined in WorkerThread.cpp
void disconnect(int key);