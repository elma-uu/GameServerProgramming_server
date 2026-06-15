#include "pch.h"
#include "DungeonManager.h"

int DungeonManager::CreateInstance(int party_id)
{
    std::lock_guard<std::mutex> lock(sMutex);
    for (int i = 0; i < MAX_DUNGEON_INSTANCES; ++i) {
        if (!sInstances[i].is_active) {
            sInstances[i].party_id  = party_id;
            sInstances[i].is_active = true;
            sPartyToInstance[party_id] = i;
            return i;
        }
    }
    return -1; // no free slot
}

void DungeonManager::CloseInstance(int instance_id)
{
    if (instance_id < 0 || instance_id >= MAX_DUNGEON_INSTANCES) return;
    std::lock_guard<std::mutex> lock(sMutex);
    sPartyToInstance.erase(sInstances[instance_id].party_id);
    sInstances[instance_id] = {};
}

int DungeonManager::GetInstanceForParty(int party_id)
{
    std::lock_guard<std::mutex> lock(sMutex);
    auto it = sPartyToInstance.find(party_id);
    return (it != sPartyToInstance.end()) ? it->second : -1;
}

short DungeonManager::GetSpawnX(int instance_id)
{
    return static_cast<short>(DUNGEON_BASE_X + instance_id * DUNGEON_STRIDE + DUNGEON_SIZE / 2);
}

short DungeonManager::GetSpawnY(int instance_id)
{
    return static_cast<short>(DUNGEON_BASE_Y + DUNGEON_SIZE / 2);
}

short DungeonManager::GetMinX(int instance_id)
{
    return static_cast<short>(DUNGEON_BASE_X + instance_id * DUNGEON_STRIDE);
}

short DungeonManager::GetMaxX(int instance_id)
{
    return static_cast<short>(DUNGEON_BASE_X + instance_id * DUNGEON_STRIDE + DUNGEON_SIZE - 1);
}

short DungeonManager::GetMinY()
{
    return static_cast<short>(DUNGEON_BASE_Y);
}

short DungeonManager::GetMaxY()
{
    return static_cast<short>(DUNGEON_BASE_Y + DUNGEON_SIZE - 1);
}
