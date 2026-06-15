#include "pch.h"
#include "LuaManager.h"
#include <cstdio>

thread_local lua_State* tL = nullptr;

bool LuaManager::Init(const char* script_path)
{
    sScripts.clear();
    sScripts.push_back(script_path);
    // Validate by test-loading into a temporary state
    lua_State* test = luaL_newstate();
    luaL_openlibs(test);
    bool ok = (luaL_dofile(test, script_path) == LUA_OK);
    if (!ok)
        printf("[Lua] Failed to validate %s: %s\n", script_path, lua_tostring(test, -1));
    lua_close(test);
    return ok;
}

bool LuaManager::LoadScript(const char* script_path)
{
    sScripts.push_back(script_path);
    return true;
}

bool LuaManager::InitThread()
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    for (const auto& s : sScripts) {
        if (luaL_dofile(L, s.c_str()) != LUA_OK) {
            printf("[Lua] Thread failed to load %s: %s\n",
                   s.c_str(), lua_tostring(L, -1));
            lua_pop(L, 1);
            lua_close(L);
            tL = nullptr;
            return false;
        }
    }
    tL = L;
    return true;
}

void LuaManager::Shutdown() {}

bool LuaManager::GetQuestInfo(int questId, QuestInfo& out)
{
    if (!tL) return false;
    lua_getglobal(tL, "quest_get_info");
    lua_pushinteger(tL, questId);
    if (lua_pcall(tL, 1, 5, 0) != LUA_OK) {
        printf("[Lua] quest_get_info error: %s\n", lua_tostring(tL, -1));
        lua_pop(tL, 1);
        return false;
    }
    if (lua_isnil(tL, -5)) { lua_pop(tL, 5); return false; }
    out.name = lua_tostring(tL, -5);
    out.type = lua_tostring(tL, -4);
    out.goal = (int)lua_tointeger(tL, -3);
    out.exp  = (int)lua_tointeger(tL, -2);
    out.gold = (int)lua_tointeger(tL, -1);
    lua_pop(tL, 5);
    return true;
}

bool LuaManager::GetNextStep(short sx, short sy, short gx, short gy,
                              short& out_dx, short& out_dy, int max_range)
{
    if (!tL) return false;
    lua_getglobal(tL, "find_next_step");
    lua_pushinteger(tL, sx);
    lua_pushinteger(tL, sy);
    lua_pushinteger(tL, gx);
    lua_pushinteger(tL, gy);
    lua_pushinteger(tL, max_range);
    if (lua_pcall(tL, 5, 2, 0) != LUA_OK) {
        printf("[Lua] find_next_step error: %s\n", lua_tostring(tL, -1));
        lua_pop(tL, 1);
        return false;
    }
    out_dx = static_cast<short>(lua_tointeger(tL, -2));
    out_dy = static_cast<short>(lua_tointeger(tL, -1));
    lua_pop(tL, 2);
    return true;
}
