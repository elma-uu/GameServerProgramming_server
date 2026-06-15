#pragma once
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include <algorithm>

class SESSION;

// Dungeon-local tile coordinates (independent of world coordinate space)
constexpr short DUNGEON_LOCAL_BOSS_X  = 15;
constexpr short DUNGEON_LOCAL_BOSS_Y  = 15;
constexpr short DUNGEON_LOCAL_SPAWN_X = 1;
constexpr short DUNGEON_LOCAL_SPAWN_Y = 15;

// One dungeon instance: owns a boss NPC, a player list, and its own thread.
// All visibility inside the dungeon is managed here directly (no world sectors).
class DungeonInstance {
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

    int  mInstanceId;
    int  mPartyId;

    std::thread       mThread;
    std::atomic<bool> mRunning{ false };

    std::mutex mMutex;
    std::vector<std::shared_ptr<SESSION>> mPlayers;
    std::shared_ptr<SESSION>              mBoss;
    std::shared_ptr<SESSION>              mHandL;  // left hand  (tile boss-6, boss+1)
    std::shared_ptr<SESSION>              mHandR;  // right hand (tile boss+6, boss+1)
};
