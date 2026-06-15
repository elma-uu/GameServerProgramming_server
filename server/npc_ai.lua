
local function node_key(x, y)
    return x * 100003 + y
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

    local dx_goal = gx - sx
    local dy_goal = gy - sy
    local manhattan = math.abs(dx_goal) + math.abs(dy_goal)
    if manhattan > max_range then
        local scale = max_range / manhattan
        gx = math.floor(sx + dx_goal * scale)
        gy = math.floor(sy + dy_goal * scale)

        if not is_walkable(gx, gy) then
            gx = gx + (dx_goal > 0 and -1 or 1)
            gy = gy + (dy_goal > 0 and -1 or 1)
        end
    end

    -- A* state
    local open      = {}  -- node_key -> {x, y, g, f}
    local g_score   = {}  -- node_key -> number
    local came_from = {}  -- node_key -> {px, py}  (nil = start)
    local closed    = {}  -- node_key -> bool

    local sk = node_key(sx, sy)
    g_score[sk]   = 0
    came_from[sk] = nil
    open[sk]      = { x = sx, y = sy, g = 0, f = heuristic(sx, sy, gx, gy) }

    local dirs = { {1,0}, {-1,0}, {0,1}, {0,-1} }
    local found = false

    for _ = 1, 800 do

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

            local mdist = math.abs(nx - sx) + math.abs(ny - sy)
            if  mdist <= max_range
            and nx >= 0 and ny >= 0
            and is_walkable(nx, ny) then
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

        local odx = gx - sx
        local ody = gy - sy
        local sdx = (odx > 0) and 1 or (odx < 0 and -1 or 0)
        local sdy = (ody > 0) and 1 or (ody < 0 and -1 or 0)

        local candidates
        if math.abs(odx) >= math.abs(ody) then
            candidates = { {sdx,0}, {0,sdy}, {0,-sdy}, {-sdx,0} }
        else
            candidates = { {0,sdy}, {sdx,0}, {-sdx,0}, {0,-sdy} }
        end
        for _, c in ipairs(candidates) do
            if c[1] ~= 0 or c[2] ~= 0 then
                if is_walkable(sx + c[1], sy + c[2]) then
                    return c[1], c[2]
                end
            end
        end
        return 0, 0
    end

    local cx, cy = gx, gy
    local steps = 0
    while steps < max_range + 2 do
        steps = steps + 1
        local ck     = node_key(cx, cy)
        local parent = came_from[ck]
        if parent == nil then
            break
        end
        if parent[1] == sx and parent[2] == sy then
            return cx - sx, cy - sy   -- first step found
        end
        cx, cy = parent[1], parent[2]
    end

    local odx = gx - sx
    local ody = gy - sy
    local sdx = (odx > 0) and 1 or (odx < 0 and -1 or 0)
    local sdy = (ody > 0) and 1 or (ody < 0 and -1 or 0)
    if sdx ~= 0 and is_walkable(sx + sdx, sy) then return sdx, 0 end
    if sdy ~= 0 and is_walkable(sx, sy + sdy) then return 0, sdy end
    return 0, 0
end
