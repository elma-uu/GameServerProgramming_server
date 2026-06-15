#pragma once
#include <mutex>
#include <memory>
#include <unordered_map>
#include "protocol_2026.h"
#include "DungeonInstance.h"

class DungeonManager {
public:
    // Create a new dungeon instance for party_id.
    // Returns the instance_id [0, MAX_DUNGEON_INSTANCES) or -1 if full.
    static int CreateInstance(int party_id);

    // Stop and free the instance slot. Called when last player exits or on disconnect.
    static void CloseInstance(int instance_id);

    // Returns the instance_id for party_id, or -1 if none.
    static int GetInstanceForParty(int party_id);

    // Returns a shared_ptr to the live instance, or nullptr.
    static std::shared_ptr<DungeonInstance> GetInstance(int instance_id);

    // Dungeon-local tile spawn position for party members (same for every instance).
    static constexpr short SpawnX() { return DUNGEON_LOCAL_SPAWN_X; }
    static constexpr short SpawnY() { return DUNGEON_LOCAL_SPAWN_Y; }

private:
    struct Slot {
        int  party_id  = -1;
        bool is_active = false;
        std::shared_ptr<DungeonInstance> instance;
    };

    inline static Slot                           sSlots[MAX_DUNGEON_INSTANCES];
    inline static std::unordered_map<int, int>   sPartyToInstance;
    inline static std::mutex                     sMutex;
};
