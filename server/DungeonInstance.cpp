#include "pch.h"
#include "DungeonInstance.h"
#include "SESSION.h"
#include "protocol_2026.h"

DungeonInstance::DungeonInstance(int instance_id, int party_id)
    : mInstanceId(instance_id), mPartyId(party_id)
{}

DungeonInstance::~DungeonInstance()
{
    Stop();
}

// Helper: build a stationary dungeon NPC (boss parts are never in world sectors)
static std::shared_ptr<SESSION> MakePart(int id, int instanceId,
    const char* name, short x, short y, int visualId, int hp)
{
    auto s = std::make_shared<SESSION>(id, false);
    strncpy_s(s->mUsername, name, MAX_NAME_LEN - 1);
    s->mX = x;  s->mY = y;
    s->mSpawnX = x; s->mSpawnY = y;
    s->mVisualId       = visualId;
    s->mLevel          = 50;
    s->mMaxHp          = hp;
    s->mHp             = hp;
    s->mExp            = 0ULL;
    s->mIsDead         = false;
    s->mIsStationary   = true;
    s->mDungeonInstanceId = instanceId;
    s->mState          = CS_PLAYING;
    s->mSector_id      = -1;
    return s;
}

void DungeonInstance::Start()
{
    // Head (boss body)
    mBoss  = MakePart(DUNGEON_BOSS_ID_START   + mInstanceId, mInstanceId,
                      "Belial",
                      DUNGEON_LOCAL_BOSS_X, DUNGEON_LOCAL_BOSS_Y,
                      VISUAL_BOSS_BELIAL, 5000);
    mBoss->mExp = 5000ULL;

    // Hands: 1 tile below head, ±6 tiles left/right
    mHandL = MakePart(DUNGEON_HAND_L_ID_START + mInstanceId, mInstanceId,
                      "",
                      (short)(DUNGEON_LOCAL_BOSS_X - 6), (short)(DUNGEON_LOCAL_BOSS_Y + 1),
                      VISUAL_BOSS_BELIAL_HAND_L, 3000);

    mHandR = MakePart(DUNGEON_HAND_R_ID_START + mInstanceId, mInstanceId,
                      "",
                      (short)(DUNGEON_LOCAL_BOSS_X + 8), (short)(DUNGEON_LOCAL_BOSS_Y + 1),
                      VISUAL_BOSS_BELIAL_HAND_R, 3000);

    mRunning = true;
    mThread  = std::thread(&DungeonInstance::ThreadFunc, this);
}

void DungeonInstance::Stop()
{
    mRunning = false;
    if (mThread.joinable())
        mThread.join();
}

void DungeonInstance::ThreadFunc()
{
    while (mRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(NPC_MOVE_INTERVAL));
        // Boss is stationary (idle-only). Future: add boss attack AI here.
    }
}

void DungeonInstance::AddPlayer(std::shared_ptr<SESSION> player)
{
    std::lock_guard<std::mutex> lock(mMutex);

    // New player sees boss head and both hands
    SendAddObject(player, mBoss);
    SendAddObject(player, mHandL);
    SendAddObject(player, mHandR);

    // Cross-visibility with already-present players
    for (auto& existing : mPlayers) {
        SendAddObject(player,   existing);
        SendAddObject(existing, player);
    }

    mPlayers.push_back(player);
}

void DungeonInstance::RemovePlayer(int player_id)
{
    std::lock_guard<std::mutex> lock(mMutex);

    auto it = std::find_if(mPlayers.begin(), mPlayers.end(),
        [player_id](const std::shared_ptr<SESSION>& p) {
            return p && p->mId == player_id;
        });
    if (it == mPlayers.end()) return;

    auto leaving = *it;
    mPlayers.erase(it);

    // Tell remaining dungeon players to remove the leaving player
    for (auto& p : mPlayers)
        SendRemoveObject(p, player_id);

    // Tell the leaving player to remove boss parts and remaining players
    SendRemoveObject(leaving, mBoss->mId);
    SendRemoveObject(leaving, mHandL->mId);
    SendRemoveObject(leaving, mHandR->mId);
    for (auto& p : mPlayers)
        SendRemoveObject(leaving, p->mId);
}

void DungeonInstance::OnPlayerMove(std::shared_ptr<SESSION> moved_player)
{
    std::lock_guard<std::mutex> lock(mMutex);

    S2C_MoveObject pkt;
    pkt.size      = sizeof(S2C_MoveObject);
    pkt.type      = S2C_MOVE_OBJECT;
    pkt.object_id = moved_player->mId;
    pkt.x         = moved_player->mX;
    pkt.y         = moved_player->mY;
    pkt.move_time = moved_player->mMove_time;

    for (auto& p : mPlayers) {
        if (p && p->mId != moved_player->mId)
            p->doSend(pkt.size, reinterpret_cast<char*>(&pkt));
    }
}

bool DungeonInstance::IsEmpty()
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mPlayers.empty();
}

void DungeonInstance::SendAddObject(std::shared_ptr<SESSION> to, std::shared_ptr<SESSION> obj)
{
    if (!to || !obj) return;
    S2C_AddObject pkt;
    pkt.size      = sizeof(S2C_AddObject);
    pkt.type      = S2C_ADD_OBJECT;
    pkt.object_id = obj->mId;
    pkt.visual_id = obj->mVisualId;
    memcpy(pkt.obj_name, obj->mUsername, MAX_NAME_LEN);
    pkt.x         = obj->mX;
    pkt.y         = obj->mY;
    pkt.hp        = obj->mHp;
    pkt.max_hp    = obj->mMaxHp;
    pkt.exp       = obj->mExp;
    pkt.level     = obj->mLevel;
    to->doSend(pkt.size, reinterpret_cast<char*>(&pkt));
}

void DungeonInstance::SendRemoveObject(std::shared_ptr<SESSION> to, int obj_id)
{
    if (!to) return;
    S2C_RemoveObject pkt;
    pkt.size      = sizeof(S2C_RemoveObject);
    pkt.type      = S2C_REMOVE_OBJECT;
    pkt.object_id = obj_id;
    to->doSend(pkt.size, reinterpret_cast<char*>(&pkt));
}
