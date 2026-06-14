#pragma once
#include <sql.h>
#include <sqlext.h>
#include <mutex>

#pragma comment(lib, "odbc32.lib")

struct PlayerSaveData {
    char username[20];
    short x, y;
    int hp, max_hp;
    unsigned long long exp;
    unsigned char level, str, intl, dex, luk, stat_points;
    unsigned char visual_id;   // 0=base 1=alice 2=metalPlate 3=pickax 4=redLotus
};

// Use DBR_ prefix to avoid Windows SDK macro collisions
enum DbLoginResult {
    DBR_OK       = 0,
    DBR_NEW_USER = 1,
    DBR_WRONG_PW = 2,
    DBR_FAIL     = 3
};

class Database {
public:
    static bool          Connect();
    static void          Disconnect();
    static DbLoginResult Login(const char* username, const char* password,
                               PlayerSaveData& out);
    static bool          SavePlayer(const PlayerSaveData& data);

private:
    static void       LogError(SQLSMALLINT type, SQLHANDLE handle);
    static SQLHENV    hEnv;
    static SQLHDBC    hDbc;
    static std::mutex g_dbMutex;
    static bool       g_connected;
};
