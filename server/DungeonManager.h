#pragma once
#include <mutex>
#include <unordered_map>
#include "protocol_2026.h"

class DungeonManager {
public:
    // Returns instance_id [0, MAX_DUNGEON_INSTANCES) on success, -1 when full.
    static int   CreateInstance(int party_id);

    // Free the slot so another party can use it.
    static void  CloseInstance(int instance_id);

    // Returns the instance_id currently assigned to party_id, or -1.
    static int   GetInstanceForParty(int party_id);

    // Spawn tile coordinates for the given instance.
    static short GetSpawnX(int instance_id);
    static short GetSpawnY(int instance_id);

    // Tile boundary (inclusive) for the given instance.
    static short GetMinX(int instance_id);
    static short GetMaxX(int instance_id);
    static short GetMinY();
    static short GetMaxY();

private:
    struct Instance {
        int  party_id  = -1;
        bool is_active = false;
    };

    inline static Instance                     sInstances[MAX_DUNGEON_INSTANCES];
    inline static std::unordered_map<int, int> sPartyToInstance;   // party_id -> instance_id
    inline static std::mutex                   sMutex;
};
