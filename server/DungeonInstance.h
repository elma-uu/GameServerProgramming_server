#pragma once
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include <algorithm>
#include <chrono>

class SESSION;
struct lua_State;  // forward-declare to avoid including lua headers in .h

// Dungeon-local tile coordinates (independent of world coordinate space)
constexpr short DUNGEON_LOCAL_BOSS_X  = 15;
constexpr short DUNGEON_LOCAL_BOSS_Y  = 15;
constexpr short DUNGEON_LOCAL_SPAWN_X = 1;
constexpr short DUNGEON_LOCAL_SPAWN_Y = 15;

// Hand resting positions (must match DungeonInstance::Start spawn coords)
constexpr short HAND_ORIGIN_X_L = DUNGEON_LOCAL_BOSS_X - 6;  // 9
constexpr short HAND_ORIGIN_X_R = DUNGEON_LOCAL_BOSS_X + 8;  // 23
constexpr short HAND_ORIGIN_Y   = DUNGEON_LOCAL_BOSS_Y + 1;  // 16

// Pattern timing constants (milliseconds)
constexpr int PAT_MOVE_MS      = 500;         // hand sweep duration
constexpr int PAT_WINDUP_MS    = 10 * 83;     // 830ms: attack frames 0-9 before laser
constexpr int PAT_LASER_HOLD_MS = 8 * 83;     // 664ms: attack frames 10-17 (laser active)
constexpr int PAT_RETURN_MS    = 500;          // hands return to origin
constexpr int PAT_COOLDOWN_MS  = 2000;         // idle gap between patterns
constexpr int PAT_INITIAL_MS   = 3000;         // delay before first pattern

constexpr int LASER_DAMAGE     = 500;          // HP per laser hit

// Phase 2 sword fall
constexpr int SWORD_X_STEP           = 3;     // sword every 3 tiles (x=3,6,9,...)
constexpr int SWORD_DAMAGE           = 300;
constexpr int SWORD_FALL_INTERVAL_MS = 10000; // 10s between falls
constexpr int SWORD_FALL_DURATION_MS = 3000;  // fall animation duration

// WINDUP: attack anim playing, frames 0-9 (no laser yet)
// ATTACK: laser fires at frame 10, damage applied
enum class BossPatternState { WAIT, MOVING, WINDUP, ATTACK, RECOVER };
enum class SwordFallState   { IDLE, FALLING };

// One dungeon instance: owns a boss NPC, a player list, and its own thread.
// All visibility inside the dungeon is managed here directly (no world sectors).
class DungeonInstance : public std::enable_shared_from_this<DungeonInstance> {
public:
    DungeonInstance(int instance_id, int party_id);
    ~DungeonInstance();

    // Spawn boss and start the instance thread. Call once after construction.
    void Start();

    // Signal thread to stop and wait for it. Called by DungeonManager::CloseInstance.
    void Stop();

    // Add a player to the instance. Sends boss + existing players to them,
    // and sends them to existing players. Call from IOCP thread.
    void AddPlayer(std::shared_ptr<SESSION> player);

    // Remove a player from the instance. Sends REMOVE_OBJECT notifications
    // in both directions. Call from IOCP thread.
    void RemovePlayer(int player_id);

    // Broadcast a dungeon-local move to all other players in the instance.
    void OnPlayerMove(std::shared_ptr<SESSION> moved_player);

    int  GetInstanceId() const { return mInstanceId; }
    int  GetPartyId()    const { return mPartyId; }
    bool IsEmpty();

private:
    void ThreadFunc();
    void SendAddObject(std::shared_ptr<SESSION> to, std::shared_ptr<SESSION> obj);
    void SendRemoveObject(std::shared_ptr<SESSION> to, int obj_id);

    // Pattern helpers — called from dungeon thread only (no external locking needed)
    void UpdatePattern();
    void UpdatePhase1(std::chrono::steady_clock::time_point now);
    void BroadcastToAll(const void* pkt, int sz);
    void BroadcastHandMoveTo(std::shared_ptr<SESSION> hand, short tx, short ty, int ms);
    void BroadcastLaserFire(std::shared_ptr<SESSION> hand, short centerY, int durationMs);
    void BroadcastHandAnimState(std::shared_ptr<SESSION> hand, unsigned char state);
    void ApplyLaserDamage(short centerYL, short centerYR);

    // Phase 2: sword fall pattern
    void UpdateSwordFall(std::chrono::steady_clock::time_point now);
    void BroadcastSwordFall(int durationMs);
    void ApplySwordDamage(short swordRow);

    // Attack hitbox helpers — called from IOCP worker threads (acquire mMutex for reads)
public:
    // Returns the boss/hand SESSION that includes tile (tx, ty) in its hitbox, or nullptr.
    std::shared_ptr<SESSION> FindHittablePartAt(short tx, short ty);
    // Apply player attack damage to a boss part. Broadcasts DamageNumber + StatusChange/Remove.
    void OnPartDamage(std::shared_ptr<SESSION> part, int attackerId, int damage, bool isCrit);
private:
    // Called after laser damage: teleport all dead players to world spawn and close slot.
    // Safe to call from the dungeon thread (the thread holds its own shared_ptr reference).
    void CheckWipe();

    int  mInstanceId;
    int  mPartyId;

    std::thread       mThread;
    std::atomic<bool> mRunning{ false };

    std::mutex mMutex;
    std::vector<std::shared_ptr<SESSION>> mPlayers;
    std::shared_ptr<SESSION>              mBoss;
    std::shared_ptr<SESSION>              mHandL;  // left hand  (tile boss-6, boss+1)
    std::shared_ptr<SESSION>              mHandR;  // right hand (tile boss+8, boss+1)

    // Per-instance Lua state (not shared with IOCP worker threads)
    lua_State*         mLua      = nullptr;

    // Phase 1 pattern state machine
    BossPatternState   mPatState = BossPatternState::WAIT;
    std::chrono::steady_clock::time_point mPatTimer{};
    short              mHandTargetYL = DUNGEON_LOCAL_BOSS_Y;
    short              mHandTargetYR = DUNGEON_LOCAL_BOSS_Y;

    // Phase 2 sword fall state
    bool               mPhase2Started  = false;
    SwordFallState     mSwordFallState = SwordFallState::IDLE;
    std::chrono::steady_clock::time_point mSwordFallTimer{};
    std::chrono::steady_clock::time_point mSwordFallStart{};
    int                mSwordLastRow   = -1;
};
