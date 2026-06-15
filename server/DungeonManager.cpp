#include "pch.h"
#include "DungeonManager.h"
#include "DungeonInstance.h"

int DungeonManager::CreateInstance(int party_id)
{
    std::lock_guard<std::mutex> lock(sMutex);
    for (int i = 0; i < MAX_DUNGEON_INSTANCES; ++i) {
        if (!sSlots[i].is_active) {
            sSlots[i].party_id  = party_id;
            sSlots[i].is_active = true;
            sSlots[i].instance  = std::make_shared<DungeonInstance>(i, party_id);
            sSlots[i].instance->Start();
            sPartyToInstance[party_id] = i;
            return i;
        }
    }
    return -1;
}

void DungeonManager::CloseInstance(int instance_id)
{
    if (instance_id < 0 || instance_id >= MAX_DUNGEON_INSTANCES) return;
    std::shared_ptr<DungeonInstance> inst;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (!sSlots[instance_id].is_active) return;
        sPartyToInstance.erase(sSlots[instance_id].party_id);
        inst = std::move(sSlots[instance_id].instance);
        sSlots[instance_id] = {};
    }
    // Stop thread outside the lock to avoid deadlock
    if (inst) inst->Stop();
}

int DungeonManager::GetInstanceForParty(int party_id)
{
    std::lock_guard<std::mutex> lock(sMutex);
    auto it = sPartyToInstance.find(party_id);
    return (it != sPartyToInstance.end()) ? it->second : -1;
}

std::shared_ptr<DungeonInstance> DungeonManager::GetInstance(int instance_id)
{
    if (instance_id < 0 || instance_id >= MAX_DUNGEON_INSTANCES) return nullptr;
    std::lock_guard<std::mutex> lock(sMutex);
    return sSlots[instance_id].is_active ? sSlots[instance_id].instance : nullptr;
}

void DungeonManager::ReleaseSlot(int instance_id)
{
    if (instance_id < 0 || instance_id >= MAX_DUNGEON_INSTANCES) return;
    std::shared_ptr<DungeonInstance> inst;  // keep alive until after lock release
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (!sSlots[instance_id].is_active) return;
        sPartyToInstance.erase(sSlots[instance_id].party_id);
        inst = std::move(sSlots[instance_id].instance);  // transfer ref, don't destroy under lock
        sSlots[instance_id] = {};
    }
    // inst goes out of scope here. If the calling thread is the dungeon thread that
    // captured shared_from_this() in its lambda, this just decrements the ref count;
    // the lambda's own ref keeps the object alive until ThreadFunc returns.
    // If it's the last ref (external call), ~DungeonInstance joins the finished thread.
}
