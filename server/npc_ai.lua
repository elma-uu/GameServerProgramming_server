-- npc_ai.lua: A* pathfinding for NPC chase behavior
-- Called from C++ via LuaManager::GetNextStep(sx, sy, gx, gy, max_range)
-- Returns: dx, dy  (one-tile step toward goal)

local function node_key(x, y)
    return x * 1000003 + y
end

local function heuristic(x, y, gx, gy)
    return math.abs(x - gx) + math.abs(y - gy)
end

function find_next_step(sx, sy, gx, gy, max_range)
    sx = math.floor(sx)
    sy = math.floor(sy)
    gx = math.floor(gx)
    gy = math.floor(gy)

    if sx == gx and sy == gy then return 0, 0 end

    -- Clamp goal inside max_range radius
    local dx_goal = gx - sx
    local dy_goal = gy - sy
    local dist = math.sqrt(dx_goal * dx_goal + dy_goal * dy_goal)
    if dist > max_range then
        local scale = max_range / dist
        gx = math.floor(sx + dx_goal * scale)
        gy = math.floor(sy + dy_goal * scale)
    end

    -- A* state
    local open      = {}   -- node_key -> {x, y, g, f}
    local g_score   = {}   -- node_key -> number
    local came_from = {}   -- node_key -> {px, py}  (nil = start)
    local closed    = {}   -- node_key -> bool

    local sk = node_key(sx, sy)
    g_score[sk]   = 0
    came_from[sk] = nil
    open[sk]      = { x = sx, y = sy, g = 0, f = heuristic(sx, sy, gx, gy) }

    local dirs = { {1,0}, {-1,0}, {0,1}, {0,-1} }
    local found = false

    for _ = 1, 600 do
        -- Pick open node with lowest f  (simple linear scan: good enough for small range)
        local best_k, best = nil, nil
        for k, n in pairs(open) do
            if best == nil or n.f < best.f then
                best_k, best = k, n
            end
        end
        if best == nil then break end

        if best.x == gx and best.y == gy then
            found = true
            break
        end

        open[best_k] = nil
        closed[best_k] = true

        for _, d in ipairs(dirs) do
            local nx = best.x + d[1]
            local ny = best.y + d[2]

            -- Stay within max_range of start and world bounds
            if  math.abs(nx - sx) <= max_range
            and math.abs(ny - sy) <= max_range
            and nx >= 0 and ny >= 0 then
                local nk = node_key(nx, ny)
                if not closed[nk] then
                    local ng = best.g + 1
                    if g_score[nk] == nil or ng < g_score[nk] then
                        g_score[nk]   = ng
                        came_from[nk] = { best.x, best.y }
                        open[nk]      = { x = nx, y = ny, g = ng,
                                          f = ng + heuristic(nx, ny, gx, gy) }
                    end
                end
            end
        end
    end

    if not found then
        -- Fallback: one step directly toward original goal
        local odx = gx - sx
        local ody = gy - sy
        if math.abs(odx) >= math.abs(ody) then
            return (odx > 0) and 1 or -1, 0
        else
            return 0, (ody > 0) and 1 or -1
        end
    end

    -- Reconstruct path: trace back from goal to find first step after start
    local cx, cy = gx, gy
    while true do
        local ck     = node_key(cx, cy)
        local parent = came_from[ck]
        if parent == nil then break end          -- reached start unexpectedly
        if parent[1] == sx and parent[2] == sy then
            return cx - sx, cy - sy             -- first step found
        end
        cx, cy = parent[1], parent[2]
    end

    return 0, 0
end
