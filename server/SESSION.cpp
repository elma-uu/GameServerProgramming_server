#include "common.h"
#include "SESSION.h"

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
	mX = 0;
	mY = 0;
	mMove_time = 0;
	mSector_id = 0;
	is_player = true;
}

SESSION::SESSION(int id, bool isPlayer)
{
	mId = id;
	mState = CS_CONNECT;
	mX = 0;
	mY = 0;
	mMove_time = 0;
	mSector_id = 0;
	is_player = isPlayer;
}

SESSION::~SESSION()
{
}

void SESSION::sendLoginSuccess()
{
}

void SESSION::doRecv()
{
}

void SESSION::doSend(int numBytes, char* mess)
{
}

void SESSION::sendAvatarInfo()
{
}

void SESSION::sendAddPlayer(int player_id)
{
}

void SESSION::sendRemovePlayer(int player_id)
{
}

void SESSION::sendMovePacket(int mover)
{
}

void SESSION::processPacket(unsigned char* p)
{
}
