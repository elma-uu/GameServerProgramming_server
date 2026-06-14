#include "pch.h"
#include "LuaManager.h"
#include <cstdio>

lua_State* LuaManager::L       = nullptr;
std::mutex LuaManager::mMutex;

bool LuaManager::Init(const char* script_path)
{
    L = luaL_newstate();
    luaL_openlibs(L);

    if (luaL_dofile(L, script_path) != LUA_OK) {
        printf("[Lua] Failed to load %s: %s\n", script_path, lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_close(L);
        L = nullptr;
        return false;
    }
    printf("[Lua] npc_ai loaded from %s\n", script_path);
    return true;
}

void LuaManager::Shutdown()
{
    if (L) { lua_close(L); L = nullptr; }
}

bool LuaManager::GetNextStep(short sx, short sy, short gx, short gy,
                              short& out_dx, short& out_dy, int max_range)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!L) return false;

    lua_getglobal(L, "find_next_step");
    lua_pushinteger(L, sx);
    lua_pushinteger(L, sy);
    lua_pushinteger(L, gx);
    lua_pushinteger(L, gy);
    lua_pushinteger(L, max_range);

    if (lua_pcall(L, 5, 2, 0) != LUA_OK) {
        printf("[Lua] find_next_step error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }

    out_dx = static_cast<short>(lua_tointeger(L, -2));
    out_dy = static_cast<short>(lua_tointeger(L, -1));
    lua_pop(L, 2);
    return true;
}
