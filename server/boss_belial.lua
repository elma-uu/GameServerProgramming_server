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

-- Phase 1: pick a random alive player Y (kept for compatibility)
function boss_phase1_pick_target(players_y, count)
    if count <= 0 then return 15 end
    return players_y[math.random(1, count)]
end

-- Phase 1: pick two target Ys for left and right hands.
-- If 2+ players are alive, picks two DIFFERENT players so each hand targets a different one.
-- Returns (yLeft, yRight).
function boss_phase1_pick_two_targets(players_y, count)
    if count <= 0 then return 15, 15 end
    if count == 1 then
        return players_y[1], players_y[1]
    end
    local i1 = math.random(1, count)
    local i2
    repeat i2 = math.random(1, count) until i2 ~= i1
    return players_y[i1], players_y[i2]
end
