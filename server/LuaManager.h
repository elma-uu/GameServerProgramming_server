#pragma once
#pragma warning(disable: 4819)
#include <mutex>

#pragma comment(lib, "lua55.lib")

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

class LuaManager {
public:
    static bool Init(const char* script_path);
    static void Shutdown();
    static bool GetNextStep(short sx, short sy, short gx, short gy,
                            short& out_dx, short& out_dy,
                            int max_range = 20);
private:
    static lua_State* L;
    static std::mutex mMutex;
};
