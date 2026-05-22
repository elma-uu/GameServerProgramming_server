#pragma once
#include "common.h"
#include "EXP_OVER.h"

enum CL_STATE { CS_CONNECT, CS_PLAYING, CS_LOGOUT };

class SESSION {
public:
	SOCKET mClient;
	int mId;
	CL_STATE mState;
	EXP_OVER mOver;
	char mUsername[MAX_NAME_LEN];
	short mX, mY;
	int mMove_time;
	int mSector_id;
	std::unordered_set<int> m_visible_players;
	std::unordered_set<int> m_visible_npcs;
	std::mutex m_visible_mutex;
	bool is_player;

public:

	SESSION();
	SESSION(SOCKET s, int id);
	SESSION(int id, bool isPlayer);
	~SESSION();


	void sendLoginSuccess();
	void doRecv();
	void doSend(int numBytes, char* mess);
	void sendAvatarInfo();
	void sendAddPlayer(int player_id);
	void sendRemovePlayer(int player_id);
	void sendMovePacket(int mover);
	void processPacket(unsigned char* p);
};