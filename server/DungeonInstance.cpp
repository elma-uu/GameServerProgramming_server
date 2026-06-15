#include "pch.h"
#include "DungeonInstance.h"
#include "SESSION.h"
#include "protocol_2026.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

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
    if (mLua) { lua_close(mLua); mLua = nullptr; }
}

void DungeonInstance::ThreadFunc()
{
    // Load boss pattern script into a dedicated Lua state for this thread
    mLua = luaL_newstate();
    luaL_openlibs(mLua);
    if (luaL_dofile(mLua, "boss_belial.lua") != LUA_OK) {
        printf("[Belial] Failed to load boss_belial.lua: %s\n",
               lua_tostring(mLua, -1));
        lua_pop(mLua, 1);
    }

    mPatState = BossPatternState::WAIT;
    mPatTimer = std::chrono::steady_clock::now()
              + std::chrono::milliseconds(PAT_INITIAL_MS);

    while (mRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        UpdatePattern();
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

// ---------------------------------------------------------------------------
// Pattern helpers
// ---------------------------------------------------------------------------

void DungeonInstance::BroadcastToAll(const void* pkt, int sz)
{
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& p : mPlayers)
        if (p) p->doSend(sz, reinterpret_cast<char*>(const_cast<void*>(pkt)));
}

void DungeonInstance::BroadcastHandMoveTo(std::shared_ptr<SESSION> hand,
                                           short tx, short ty, int ms)
{
    if (!hand) return;
    hand->mX = tx;
    hand->mY = ty;
    S2C_HandMoveTo pkt;
    pkt.size      = sizeof(S2C_HandMoveTo);
    pkt.type      = S2C_HAND_MOVE_TO;
    pkt.object_id = hand->mId;
    pkt.target_x  = tx;
    pkt.target_y  = ty;
    pkt.move_ms   = ms;
    BroadcastToAll(&pkt, pkt.size);
}

void DungeonInstance::BroadcastLaserFire(short centerY, int durationMs)
{
    S2C_LaserFire pkt;
    pkt.size        = sizeof(S2C_LaserFire);
    pkt.type        = S2C_LASER_FIRE;
    pkt.center_y    = centerY;
    pkt.duration_ms = durationMs;
    BroadcastToAll(&pkt, pkt.size);
}

void DungeonInstance::BroadcastHandAnimState(std::shared_ptr<SESSION> hand,
                                              unsigned char state)
{
    if (!hand) return;
    S2C_HandAnimState pkt;
    pkt.size       = sizeof(S2C_HandAnimState);
    pkt.type       = S2C_HAND_ANIM_STATE;
    pkt.object_id  = hand->mId;
    pkt.anim_state = state;
    BroadcastToAll(&pkt, pkt.size);
}

void DungeonInstance::ApplyLaserDamage(short centerY)
{
    // Copy player list under lock, then release before sending
    std::vector<std::shared_ptr<SESSION>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        snapshot = mPlayers;
    }

    for (auto& p : snapshot) {
        if (!p || p->mIsDead) continue;
        if (std::abs((int)p->mY - (int)centerY) > 1) continue;

        int damage = LASER_DAMAGE;
        if (p->mInvincible) damage = 0;
        p->mHp -= damage;
        if (p->mHp < 0) p->mHp = 0;

        // Status update to all dungeon players
        S2C_StatusChange sc;
        sc.size      = sizeof(S2C_StatusChange);
        sc.type      = S2C_STATUS_CHANGE;
        sc.object_id = p->mId;
        sc.hp        = p->mHp;
        sc.max_hp    = p->mMaxHp;
        sc.exp       = p->mExp;
        sc.level     = p->mLevel;
        BroadcastToAll(&sc, sc.size);

        // Floating damage number
        if (damage > 0 && mBoss) {
            S2C_DamageNumber dn;
            dn.size        = sizeof(S2C_DamageNumber);
            dn.type        = S2C_DAMAGE_NUMBER;
            dn.attacker_id = mBoss->mId;
            dn.object_id   = p->mId;
            dn.damage      = damage;
            dn.is_crit     = 0;
            BroadcastToAll(&dn, dn.size);
        }
    }
}

void DungeonInstance::UpdatePattern()
{
    if (!mBoss || mBoss->mHp <= 0) return;

    // Determine phase via Lua
    int phase = 1;
    if (mLua) {
        lua_getglobal(mLua, "boss_get_phase");
        lua_pushinteger(mLua, mBoss->mHp);
        lua_pushinteger(mLua, mBoss->mMaxHp);
        if (lua_pcall(mLua, 2, 1, 0) == LUA_OK) {
            phase = (int)lua_tointeger(mLua, -1);
            lua_pop(mLua, 1);
        } else {
            lua_pop(mLua, 1);
        }
    }

    auto now = std::chrono::steady_clock::now();
    if (phase == 1) UpdatePhase1(now);
}

void DungeonInstance::UpdatePhase1(std::chrono::steady_clock::time_point now)
{
    switch (mPatState) {

    case BossPatternState::WAIT:
        if (now < mPatTimer) break;
        {
            // Collect player Y positions
            std::vector<short> ys;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (auto& p : mPlayers)
                    if (p) ys.push_back(p->mY);
            }
            if (ys.empty()) break;  // nobody to target — try again next tick

            // Ask Lua to pick a target Y
            mHandTargetY = DUNGEON_LOCAL_BOSS_Y;
            if (mLua) {
                lua_getglobal(mLua, "boss_phase1_pick_target");
                lua_newtable(mLua);
                for (int i = 0; i < (int)ys.size(); ++i) {
                    lua_pushinteger(mLua, ys[i]);
                    lua_rawseti(mLua, -2, i + 1);
                }
                lua_pushinteger(mLua, (int)ys.size());
                if (lua_pcall(mLua, 2, 1, 0) == LUA_OK) {
                    mHandTargetY = (short)lua_tointeger(mLua, -1);
                    lua_pop(mLua, 1);
                } else {
                    lua_pop(mLua, 1);
                }
            }
            // Clamp to dungeon bounds
            mHandTargetY = max((short)0,
                           min((short)(DUNGEON_SIZE - 1), mHandTargetY));

            // Move both hands to the target row edges (0.5 s)
            BroadcastHandMoveTo(mHandL, 0,  mHandTargetY, PAT_MOVE_MS);
            BroadcastHandMoveTo(mHandR, 29, mHandTargetY, PAT_MOVE_MS);

            mPatState = BossPatternState::MOVING;
            mPatTimer = now + std::chrono::milliseconds(PAT_MOVE_MS);
        }
        break;

    case BossPatternState::MOVING:
        if (now < mPatTimer) break;
        // Hands arrived — start attack animation (frames 0-9, no laser yet)
        BroadcastHandAnimState(mHandL, 1);
        BroadcastHandAnimState(mHandR, 1);

        mPatState = BossPatternState::WINDUP;
        mPatTimer = now + std::chrono::milliseconds(PAT_WINDUP_MS);
        break;

    case BossPatternState::WINDUP:
        if (now < mPatTimer) break;
        // Frame 10 reached — fire laser and apply damage
        BroadcastLaserFire(mHandTargetY, PAT_LASER_HOLD_MS);
        ApplyLaserDamage(mHandTargetY);

        mPatState = BossPatternState::ATTACK;
        mPatTimer = now + std::chrono::milliseconds(PAT_LASER_HOLD_MS);
        break;

    case BossPatternState::ATTACK:
        if (now < mPatTimer) break;
        // Attack anim ends — return to idle and sweep hands back
        BroadcastHandAnimState(mHandL, 0);
        BroadcastHandAnimState(mHandR, 0);
        BroadcastHandMoveTo(mHandL, HAND_ORIGIN_X_L, HAND_ORIGIN_Y, PAT_RETURN_MS);
        BroadcastHandMoveTo(mHandR, HAND_ORIGIN_X_R, HAND_ORIGIN_Y, PAT_RETURN_MS);

        mPatState = BossPatternState::RECOVER;
        mPatTimer = now + std::chrono::milliseconds(PAT_RETURN_MS);
        break;

    case BossPatternState::RECOVER:
        if (now < mPatTimer) break;
        mPatState = BossPatternState::WAIT;
        mPatTimer = now + std::chrono::milliseconds(PAT_COOLDOWN_MS);
        break;
    }
}
