-- quests.lua  Quest definitions for the game server
-- Loaded by LuaManager; queried via quest_get_info(quest_id)
--
-- Quest IDs:
--   0  Tutorial  : talk to the Quest NPC (immediately complete on accept)
--   1  Kill quest: slay N monsters

local QUESTS = {
    [0] = { name = "Welcome",      type = "talk", goal = 1, exp = 100,  gold = 50  },
    [1] = { name = "Monster Hunt", type = "kill", goal = 5, exp = 500,  gold = 200 },
}

-- Returns: name, type, goal, exp, gold   (or nil if quest_id unknown)
function quest_get_info(quest_id)
    local q = QUESTS[quest_id]
    if not q then return nil end
    return q.name, q.type, q.goal, q.exp, q.gold
end

-- Returns total number of defined quests
function quest_count()
    local n = 0
    for _ in pairs(QUESTS) do n = n + 1 end
    return n
end
