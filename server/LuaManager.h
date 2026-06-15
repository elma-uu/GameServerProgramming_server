#pragma once
#pragma warning(disable: 4819)
#include <string>
#include <vector>

#pragma comment(lib, "lua55.lib")

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// Per-thread Lua states — no global mutex, each worker thread gets its own lua_State.
// Call Init() + optional LoadScript() once in main(), then InitThread() at each worker thread start.
class LuaManager {
public:
    static bool Init(const char* script_path);
    static bool LoadScript(const char* script_path);
    static bool InitThread();
    static void Shutdown();

    static bool GetNextStep(short sx, short sy, short gx, short gy,
                            short& out_dx, short& out_dy, int max_range = 20);

    struct QuestInfo {
        std::string name;
        std::string type;
        int goal = 0;
        int exp  = 0;
        int gold = 0;
    };
    static bool GetQuestInfo(int questId, QuestInfo& out);

private:
    inline static std::vector<std::string> sScripts;
};
