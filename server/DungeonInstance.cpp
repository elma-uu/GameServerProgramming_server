#include "pch.h"
#include "DungeonInstance.h"
#include "DungeonManager.h"
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
    // Capture shared_from_this so the instance outlives the thread even if the
    // DungeonManager slot is released from within ThreadFunc (CheckWipe pattern).
    auto self = shared_from_this();
    mThread = std::thread([this, self]() { ThreadFunc(); });
}

void DungeonInstance::Stop()
{
    mRunning = false;
    if (mThread.joinable()) {
        // When CheckWipe clears the DungeonManager slot, the lambda's self ref becomes
        // the last reference. On thread exit the lambda destructor calls ~DungeonInstance
        // → Stop() from within its own thread. join() from self = system_error / deadlock.
        // Detect this case and detach instead (thread already finished by now).
        if (mThread.get_id() == std::this_thread::get_id())
            mThread.detach();
        else
            mThread.join();
    }
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

void DungeonInstance::BroadcastLaserFire(std::shared_ptr<SESSION> hand,
                                         short centerY, int durationMs)
{
    if (!hand) return;
    S2C_LaserFire pkt;
    pkt.size        = sizeof(S2C_LaserFire);
    pkt.type        = S2C_LASER_FIRE;
    pkt.object_id   = hand->mId;
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

void DungeonInstance::ApplyLaserDamage(short centerYL, short centerYR)
{
    // Copy player list under lock, then release before sending
    std::vector<std::shared_ptr<SESSION>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        snapshot = mPlayers;
    }

    for (auto& p : snapshot) {
        if (!p || p->mIsDead || p->mHp <= 0) continue;
        bool inRangeL = std::abs((int)p->mY - (int)centerYL) <= 1;
        bool inRangeR = std::abs((int)p->mY - (int)centerYR) <= 1;
        if (!inRangeL && !inRangeR) continue;

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

    CheckWipe();
}

// ---------------------------------------------------------------------------
// Boss hitbox helpers (called from IOCP worker threads)
// ---------------------------------------------------------------------------

std::shared_ptr<SESSION> DungeonInstance::FindHittablePartAt(short tx, short ty)
{
    // Boss head: rendered as 9×9 tiles → ±4 tile radius
    if (mBoss && mBoss->mHp > 0 &&
        std::abs((int)tx - (int)mBoss->mX) <= 4 &&
        std::abs((int)ty - (int)mBoss->mY) <= 4)
        return mBoss;
    // Left hand: rendered as 4×4 tiles → ±2 tile radius
    if (mHandL && mHandL->mHp > 0 &&
        std::abs((int)tx - (int)mHandL->mX) <= 2 &&
        std::abs((int)ty - (int)mHandL->mY) <= 2)
        return mHandL;
    // Right hand: same
    if (mHandR && mHandR->mHp > 0 &&
        std::abs((int)tx - (int)mHandR->mX) <= 2 &&
        std::abs((int)ty - (int)mHandR->mY) <= 2)
        return mHandR;
    return nullptr;
}

void DungeonInstance::OnPartDamage(std::shared_ptr<SESSION> part,
                                   int attackerId, int damage, bool isCrit)
{
    if (!part) return;

    // Protect HP modification under mMutex to prevent concurrent IOCP threads from
    // racing on the same boss part's HP (read-modify-write must be atomic).
    bool partDied = false;
    int  newHp    = 0;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (part->mHp <= 0) return;  // already dead
        part->mHp -= damage;
        if (part->mHp < 0) part->mHp = 0;
        newHp    = part->mHp;
        partDied = (newHp == 0);
    }

    // Damage number (mMutex released — BroadcastToAll will re-acquire it)
    {
        S2C_DamageNumber dn;
        dn.size        = sizeof(S2C_DamageNumber);
        dn.type        = S2C_DAMAGE_NUMBER;
        dn.attacker_id = attackerId;
        dn.object_id   = part->mId;
        dn.damage      = damage;
        dn.is_crit     = isCrit ? 1 : 0;
        BroadcastToAll(&dn, dn.size);
    }

    if (!partDied) {
        S2C_StatusChange sc;
        sc.size      = sizeof(S2C_StatusChange);
        sc.type      = S2C_STATUS_CHANGE;
        sc.object_id = part->mId;
        sc.hp        = newHp;
        sc.max_hp    = part->mMaxHp;
        sc.exp       = part->mExp;
        sc.level     = part->mLevel;
        BroadcastToAll(&sc, sc.size);
    } else {
        S2C_RemoveObject ro;
        ro.size      = sizeof(S2C_RemoveObject);
        ro.type      = S2C_REMOVE_OBJECT;
        ro.object_id = part->mId;
        BroadcastToAll(&ro, ro.size);

        // Boss head death → give rewards and end the dungeon
        if (part == mBoss) HandleBossDeath();
    }
}

// ---------------------------------------------------------------------------
// Phase 2: sword fall
// ---------------------------------------------------------------------------

void DungeonInstance::BroadcastSwordFall(int durationMs)
{
    S2C_SwordFall pkt;
    pkt.size            = sizeof(S2C_SwordFall);
    pkt.type            = S2C_SWORD_FALL;
    pkt.fall_duration_ms = durationMs;
    BroadcastToAll(&pkt, pkt.size);
}

void DungeonInstance::BroadcastSwordFallH(int durationMs)
{
    S2C_SwordFallH pkt;
    pkt.size             = sizeof(S2C_SwordFallH);
    pkt.type             = S2C_SWORD_FALL_H;
    pkt.fall_duration_ms = durationMs;
    BroadcastToAll(&pkt, pkt.size);
}

void DungeonInstance::UpdateSwordFall(std::chrono::steady_clock::time_point now)
{
    switch (mSwordFallState) {

    case SwordFallState::IDLE:
        if (now < mSwordFallTimer) break;
        // Launch vertical AND horizontal swords simultaneously
        BroadcastSwordFall(SWORD_FALL_DURATION_MS);
        BroadcastSwordFallH(SWORD_FALL_DURATION_MS);
        mSwordFallStart = now;
        mSwordLastRow   = -1;
        mSwordHLastCol  = -1;
        mSwordFallState = SwordFallState::FALLING;
        break;

    case SwordFallState::FALLING:
        {
            long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - mSwordFallStart).count();

            // Current leading edge (both vertical and horizontal advance at the same rate)
            int current = (int)((float)elapsed / SWORD_FALL_DURATION_MS * DUNGEON_SIZE);
            if (current >= DUNGEON_SIZE) current = DUNGEON_SIZE - 1;

            // Vertical: sword falls downward — fixed x=3,6,9,..., moving y row
            for (int row = mSwordLastRow + 1; row <= current; ++row)
                ApplySwordDamage((short)row);
            mSwordLastRow = current;

            // Horizontal: sword sweeps rightward — fixed y=3,6,9,..., moving x col
            for (int col = mSwordHLastCol + 1; col <= current; ++col)
                ApplySwordDamageH((short)col);
            mSwordHLastCol = current;

            if (elapsed >= SWORD_FALL_DURATION_MS) {
                mSwordFallState = SwordFallState::IDLE;
                mSwordFallTimer = now + std::chrono::milliseconds(SWORD_FALL_INTERVAL_MS);
            }
        }
        break;
    }
}

void DungeonInstance::ApplySwordDamage(short swordRow)
{
    std::vector<std::shared_ptr<SESSION>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        snapshot = mPlayers;
    }

    bool anyDamaged = false;
    for (auto& p : snapshot) {
        if (!p || p->mIsDead || p->mHp <= 0) continue;
        // Sword X columns: 3, 6, 9, ... (p->mX > 0 && p->mX % SWORD_X_STEP == 0)
        if (p->mX == 0 || p->mX % SWORD_X_STEP != 0) continue;
        if (std::abs((int)p->mY - (int)swordRow) > 1) continue;

        int damage = SWORD_DAMAGE;
        if (p->mInvincible) damage = 0;
        p->mHp -= damage;
        if (p->mHp < 0) p->mHp = 0;

        S2C_StatusChange sc;
        sc.size      = sizeof(S2C_StatusChange);
        sc.type      = S2C_STATUS_CHANGE;
        sc.object_id = p->mId;
        sc.hp        = p->mHp;
        sc.max_hp    = p->mMaxHp;
        sc.exp       = p->mExp;
        sc.level     = p->mLevel;
        BroadcastToAll(&sc, sc.size);

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
        anyDamaged = true;
    }

    if (anyDamaged) CheckWipe();
}

// Horizontal sword: fixed y rows (SWORD_X_STEP, 2*SWORD_X_STEP, ...), moving x column.
void DungeonInstance::ApplySwordDamageH(short swordCol)
{
    std::vector<std::shared_ptr<SESSION>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        snapshot = mPlayers;
    }

    bool anyDamaged = false;
    for (auto& p : snapshot) {
        if (!p || p->mIsDead || p->mHp <= 0) continue;
        // Sword Y rows: 3, 6, 9, ... (p->mY > 0 && p->mY % SWORD_X_STEP == 0)
        if (p->mY == 0 || p->mY % SWORD_X_STEP != 0) continue;
        if (std::abs((int)p->mX - (int)swordCol) > 1) continue;

        int damage = SWORD_DAMAGE;
        if (p->mInvincible) damage = 0;
        p->mHp -= damage;
        if (p->mHp < 0) p->mHp = 0;

        S2C_StatusChange sc;
        sc.size      = sizeof(S2C_StatusChange);
        sc.type      = S2C_STATUS_CHANGE;
        sc.object_id = p->mId;
        sc.hp        = p->mHp;
        sc.max_hp    = p->mMaxHp;
        sc.exp       = p->mExp;
        sc.level     = p->mLevel;
        BroadcastToAll(&sc, sc.size);

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
        anyDamaged = true;
    }

    if (anyDamaged) CheckWipe();
}

// ---------------------------------------------------------------------------
// Boss kill: distribute EXP + gold rewards, then send all players back to world.
// ---------------------------------------------------------------------------

void DungeonInstance::HandleBossDeath()
{
    constexpr unsigned long long BOSS_KILL_EXP  = 5000ULL;
    constexpr int                BOSS_KILL_GOLD = 1000;
    constexpr short WORLD_SPAWN_X = 1000;
    constexpr short WORLD_SPAWN_Y = 1000;

    // Snapshot and clear the player list atomically so concurrent calls are no-ops.
    std::vector<std::shared_ptr<SESSION>> players;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mPlayers.empty()) return;
        players  = mPlayers;
        mPlayers.clear();
    }

    for (auto& p : players) {
        if (!p || !p->is_player) continue;

        // Grant EXP and gold
        p->mExp  += BOSS_KILL_EXP;
        p->mGold += BOSS_KILL_GOLD;

        // Level-up loop (same formula as normal kills)
        while (p->mLevel < 100) {
            unsigned long long required =
                (unsigned long long)p->mLevel * p->mLevel * 20ULL;
            if (p->mExp < required) break;
            p->mExp  -= required;
            p->mLevel++;
            p->mStatPoints += 5;
            p->mMaxHp += 10;
            p->mHp     = min(p->mHp + 10, p->mMaxHp);
        }

        // Deliver reward packets first so the client sees the numbers
        p->sendGoldUpdate();
        p->sendAvatarInfo();   // also sends S2C_StatInfo (covers stat_points update)

        // Restore HP and relocate to world spawn
        p->mHp              = p->mMaxHp;
        p->mX               = WORLD_SPAWN_X;
        p->mY               = WORLD_SPAWN_Y;
        p->mDungeonInstanceId = -1;
        p->mIsDead          = false;

        int sid = get_sector_id(WORLD_SPAWN_X, WORLD_SPAWN_Y);
        p->mSector_id = sid;
        sectors[sid].insert(p->mId);

        {
            S2C_DungeonEnter de;
            de.size        = sizeof(S2C_DungeonEnter);
            de.type        = S2C_DUNGEON_ENTER;
            de.entered     = 0;
            de.instance_id = -1;
            de.x           = WORLD_SPAWN_X;
            de.y           = WORLD_SPAWN_Y;
            p->doSend(de.size, reinterpret_cast<char*>(&de));
        }
        {
            S2C_Respawn rs;
            rs.size    = sizeof(S2C_Respawn);
            rs.type    = S2C_RESPAWN;
            rs.hp      = p->mMaxHp;
            rs.max_hp  = p->mMaxHp;
            rs.x       = WORLD_SPAWN_X;
            rs.y       = WORLD_SPAWN_Y;
            p->doSend(rs.size, reinterpret_cast<char*>(&rs));
        }
    }

    DungeonManager::ReleaseSlot(mInstanceId);
    mRunning = false;
}

// ---------------------------------------------------------------------------
// Party wipe: send all dungeon players back to world spawn then close the slot.
// Must be called from the dungeon thread (which holds shared_from_this in its lambda).
// ---------------------------------------------------------------------------

void DungeonInstance::CheckWipe()
{
    // Snapshot + check under lock
    std::vector<std::shared_ptr<SESSION>> dead;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        bool anyAlive = false;
        for (auto& p : mPlayers)
            if (p && p->mHp > 0) { anyAlive = true; break; }
        if (anyAlive) return;
        dead = mPlayers;
        mPlayers.clear();
    }

    if (dead.empty()) return;

    constexpr short WORLD_SPAWN_X = 1000;
    constexpr short WORLD_SPAWN_Y = 1000;

    for (auto& p : dead) {
        if (!p) continue;

        // Restore and relocate
        p->mHp              = p->mMaxHp;
        p->mX               = WORLD_SPAWN_X;
        p->mY               = WORLD_SPAWN_Y;
        p->mDungeonInstanceId = -1;
        p->mIsDead          = false;

        // Re-insert into world sector
        int sid = get_sector_id(WORLD_SPAWN_X, WORLD_SPAWN_Y);
        p->mSector_id = sid;
        sectors[sid].insert(p->mId);

        // Tell client to exit dungeon
        {
            S2C_DungeonEnter pkt;
            pkt.size        = sizeof(S2C_DungeonEnter);
            pkt.type        = S2C_DUNGEON_ENTER;
            pkt.entered     = 0;
            pkt.instance_id = -1;
            pkt.x           = WORLD_SPAWN_X;
            pkt.y           = WORLD_SPAWN_Y;
            p->doSend(pkt.size, reinterpret_cast<char*>(&pkt));
        }

        // Respawn with full HP at world spawn
        {
            S2C_Respawn pkt;
            pkt.size   = sizeof(S2C_Respawn);
            pkt.type   = S2C_RESPAWN;
            pkt.hp     = p->mMaxHp;
            pkt.max_hp = p->mMaxHp;
            pkt.x      = WORLD_SPAWN_X;
            pkt.y      = WORLD_SPAWN_Y;
            p->doSend(pkt.size, reinterpret_cast<char*>(&pkt));
        }
    }

    // Release the DungeonManager slot so the party can re-enter fresh.
    // ReleaseSlot does NOT join the thread — the thread holds its own shared_ptr
    // and will exit naturally when mRunning becomes false.
    DungeonManager::ReleaseSlot(mInstanceId);
    mRunning = false;
}

void DungeonInstance::UpdatePattern()
{
    if (!mBoss || mBoss->mHp <= 0) return;

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
    UpdatePhase1(now);  // hand laser pattern — active in both phases

    if (phase >= 2) {
        if (!mPhase2Started) {
            mPhase2Started  = true;
            mSwordFallTimer = now + std::chrono::milliseconds(SWORD_FALL_INTERVAL_MS);
        }
        UpdateSwordFall(now);
    }
}

void DungeonInstance::UpdatePhase1(std::chrono::steady_clock::time_point now)
{
    switch (mPatState) {

    case BossPatternState::WAIT:
        if (now < mPatTimer) break;
        {
            // Collect alive player Y positions (skip dead players)
            std::vector<short> ys;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (auto& p : mPlayers)
                    if (p && !p->mIsDead && p->mHp > 0) ys.push_back(p->mY);
            }
            if (ys.empty()) break;  // all dead or nobody present — wait

            // Ask Lua to pick two (possibly different) target Ys
            mHandTargetYL = DUNGEON_LOCAL_BOSS_Y;
            mHandTargetYR = DUNGEON_LOCAL_BOSS_Y;
            if (mLua) {
                lua_getglobal(mLua, "boss_phase1_pick_two_targets");
                lua_newtable(mLua);
                for (int i = 0; i < (int)ys.size(); ++i) {
                    lua_pushinteger(mLua, ys[i]);
                    lua_rawseti(mLua, -2, i + 1);
                }
                lua_pushinteger(mLua, (int)ys.size());
                if (lua_pcall(mLua, 2, 2, 0) == LUA_OK) {
                    mHandTargetYL = (short)lua_tointeger(mLua, -2);
                    mHandTargetYR = (short)lua_tointeger(mLua, -1);
                    lua_pop(mLua, 2);
                } else {
                    lua_pop(mLua, 1);
                }
            }
            // Clamp to dungeon bounds
            mHandTargetYL = max((short)0, min((short)(DUNGEON_SIZE - 1), mHandTargetYL));
            mHandTargetYR = max((short)0, min((short)(DUNGEON_SIZE - 1), mHandTargetYR));

            // Left hand → (0, YL),  Right hand → (29, YR)
            BroadcastHandMoveTo(mHandL, 0,  mHandTargetYL, PAT_MOVE_MS);
            BroadcastHandMoveTo(mHandR, 29, mHandTargetYR, PAT_MOVE_MS);

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
        // Frame 10 reached — each hand fires its own laser at its target row
        BroadcastLaserFire(mHandL, mHandTargetYL, PAT_LASER_HOLD_MS);
        BroadcastLaserFire(mHandR, mHandTargetYR, PAT_LASER_HOLD_MS);
        ApplyLaserDamage(mHandTargetYL, mHandTargetYR);

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
