-- Boss Belial pattern script
-- Loaded by DungeonInstance per-instance thread (not via LuaManager)

math.randomseed(os.time())

-- Returns phase number based on HP ratio
-- Phase 1: HP >= 50%
-- Phase 2: HP < 50%  (reserved for future implementation)
function boss_get_phase(hp, max_hp)
    if max_hp <= 0 then return 1 end
    if hp / max_hp >= 0.5 then
        return 1
    else
        return 2
    end
end

-- Phase 1: pick a random player Y coordinate from the list
-- players_y: 1-indexed Lua table of short Y tile values
-- count: number of entries
-- Returns the chosen Y tile (fallback = 15 if empty)
function boss_phase1_pick_target(players_y, count)
    if count <= 0 then return 15 end
    local idx = math.random(1, count)
    return players_y[idx]
end
