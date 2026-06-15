#include "pch.h"
#include "Database.h"
#include <cstring>
#include <cstdio>

SQLHENV    Database::hEnv       = SQL_NULL_HENV;
SQLHDBC    Database::hDbc       = SQL_NULL_HDBC;
std::mutex Database::g_dbMutex;
bool       Database::g_connected = false;

void Database::LogError(SQLSMALLINT type, SQLHANDLE handle)
{
    SQLCHAR state[6], msg[256];
    SQLSMALLINT msgLen;
    SQLINTEGER nativeErr;
    SQLGetDiagRecA(type, handle, 1, state, &nativeErr, msg, sizeof(msg), &msgLen);
    printf("[DB Error] %s: %s\n", state, msg);
}

bool Database::Connect()
{
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

    SQLRETURN ret = SQLConnectA(hDbc,
        (SQLCHAR*)"2026_GS_PROJECT", SQL_NTS,
        NULL, 0,
        NULL, 0);

    if (!SQL_SUCCEEDED(ret)) {
        LogError(SQL_HANDLE_DBC, hDbc);
        printf("[DB] Connection failed. DSN=2026_GS_PROJECT\n");
        return false;
    }

    g_connected = true;
    printf("[DB] Connected via DSN=2026_GS_PROJECT.\n");

    // Ensure visual_id column exists (safe to run repeatedly)
    {
        SQLHSTMT hS = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hS);
        SQLExecDirectA(hS,
            (SQLCHAR*)"IF COL_LENGTH('Users','visual_id') IS NULL "
                      "ALTER TABLE Users ADD visual_id TINYINT NOT NULL DEFAULT 0",
            SQL_NTS);
        SQLFreeHandle(SQL_HANDLE_STMT, hS);
    }
    // Ensure gold column exists
    {
        SQLHSTMT hS = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hS);
        SQLExecDirectA(hS,
            (SQLCHAR*)"IF COL_LENGTH('Users','gold') IS NULL "
                      "ALTER TABLE Users ADD gold INT NOT NULL DEFAULT 0",
            SQL_NTS);
        SQLFreeHandle(SQL_HANDLE_STMT, hS);
    }
    // Ensure inventory columns exist
    {
        SQLHSTMT hS = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hS);
        SQLExecDirectA(hS,
            (SQLCHAR*)"IF COL_LENGTH('Users','potion_count') IS NULL "
                      "ALTER TABLE Users ADD potion_count INT NOT NULL DEFAULT 0",
            SQL_NTS);
        SQLFreeHandle(SQL_HANDLE_STMT, hS);
    }
    {
        SQLHSTMT hS = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hS);
        SQLExecDirectA(hS,
            (SQLCHAR*)"IF COL_LENGTH('Users','scroll_count') IS NULL "
                      "ALTER TABLE Users ADD scroll_count INT NOT NULL DEFAULT 0",
            SQL_NTS);
        SQLFreeHandle(SQL_HANDLE_STMT, hS);
    }
    {
        SQLHSTMT hS = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hS);
        SQLExecDirectA(hS,
            (SQLCHAR*)"IF COL_LENGTH('Users','weapon_enhance') IS NULL "
                      "ALTER TABLE Users ADD weapon_enhance INT NOT NULL DEFAULT 0",
            SQL_NTS);
        SQLFreeHandle(SQL_HANDLE_STMT, hS);
    }

    return true;
}

void Database::Disconnect()
{
    if (hDbc != SQL_NULL_HDBC) {
        SQLDisconnect(hDbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
    }
    if (hEnv != SQL_NULL_HENV)
        SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
    g_connected = false;
}

DbLoginResult Database::Login(const char* username, const char* password,
                               PlayerSaveData& out)
{
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_connected) return DBR_FAIL;

    SQLHSTMT hStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    SQLRETURN ret = SQLPrepareA(hStmt,
        (SQLCHAR*)"SELECT password, x, y, hp, max_hp, exp, level, "
                  "str_stat, int_stat, dex_stat, luk_stat, stat_pts, visual_id, gold, "
                  "potion_count, scroll_count, weapon_enhance "
                  "FROM Users WHERE username = ?",
        SQL_NTS);

    SQLLEN nameLen = SQL_NTS;
    SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT,
        SQL_C_CHAR, SQL_VARCHAR, 20, 0,
        (SQLPOINTER)username, 0, &nameLen);

    ret = SQLExecute(hStmt);
    if (!SQL_SUCCEEDED(ret)) {
        LogError(SQL_HANDLE_STMT, hStmt);
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return DBR_FAIL;
    }

    SQLRETURN fetchRet = SQLFetch(hStmt);

    if (fetchRet == SQL_NO_DATA) {
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

        // Insert with all columns so NOT NULL constraints are never violated
        short defX = 1000, defY = 1000;
        int defHp = 100, defMaxHp = 100;
        unsigned long long defExp = 0;
        unsigned char defLv = 1, defSt = 5, defVid = 0;
        SQLLEN ind = 0;

        SQLPrepareA(hStmt,
            (SQLCHAR*)"INSERT INTO Users "
                      "(username, password, x, y, hp, max_hp, exp, level, "
                      " str_stat, int_stat, dex_stat, luk_stat, stat_pts, visual_id, "
                      " potion_count, scroll_count, weapon_enhance) "
                      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            SQL_NTS);

        SQLLEN len1 = SQL_NTS, len2 = SQL_NTS;
        SQLBindParameter(hStmt, 1,  SQL_PARAM_INPUT, SQL_C_CHAR,     SQL_VARCHAR,  20, 0, (SQLPOINTER)username, 0, &len1);
        SQLBindParameter(hStmt, 2,  SQL_PARAM_INPUT, SQL_C_CHAR,     SQL_VARCHAR,  20, 0, (SQLPOINTER)password, 0, &len2);
        SQLBindParameter(hStmt, 3,  SQL_PARAM_INPUT, SQL_C_SSHORT,   SQL_SMALLINT,  5, 0, &defX,      0, &ind);
        SQLBindParameter(hStmt, 4,  SQL_PARAM_INPUT, SQL_C_SSHORT,   SQL_SMALLINT,  5, 0, &defY,      0, &ind);
        SQLBindParameter(hStmt, 5,  SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &defHp,     0, &ind);
        SQLBindParameter(hStmt, 6,  SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &defMaxHp,  0, &ind);
        SQLBindParameter(hStmt, 7,  SQL_PARAM_INPUT, SQL_C_UBIGINT,  SQL_BIGINT,   19, 0, &defExp,    0, &ind);
        SQLBindParameter(hStmt, 8,  SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,   3, 0, &defLv,     0, &ind);
        SQLBindParameter(hStmt, 9,  SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,   3, 0, &defSt,     0, &ind);
        SQLBindParameter(hStmt, 10, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,   3, 0, &defSt,     0, &ind);
        SQLBindParameter(hStmt, 11, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,   3, 0, &defSt,     0, &ind);
        SQLBindParameter(hStmt, 12, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,   3, 0, &defSt,     0, &ind);
        SQLBindParameter(hStmt, 13, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,   3, 0, &defSt,     0, &ind);
        SQLBindParameter(hStmt, 14, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,   3, 0, &defVid,    0, &ind);
        int defZero = 0;
        SQLBindParameter(hStmt, 15, SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &defZero,   0, &ind);
        SQLBindParameter(hStmt, 16, SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &defZero,   0, &ind);
        SQLBindParameter(hStmt, 17, SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &defZero,   0, &ind);

        SQLRETURN insRet = SQLExecute(hStmt);
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);

        if (!SQL_SUCCEEDED(insRet)) {
            return DBR_FAIL;
        }

        strncpy_s(out.username, username, 20);
        out.x = 1000; out.y = 1000;
        out.hp = 100; out.max_hp = 100;
        out.exp = 0; out.level = 1;
        out.str = out.intl = out.dex = out.luk = 5;
        out.stat_points = 0;
        out.visual_id = 0xFF;   // sentinel: char not selected yet
        out.gold = 0;
        out.potion_count = 0; out.scroll_count = 0; out.weapon_enhance = 0;

        printf("[DB] Registered new user: %s\n", username);
        return DBR_NEW_USER;
    }

    char dbPw[21] = {};
    SQLLEN colLen = 0;
    SQLGetData(hStmt, 1, SQL_C_CHAR, dbPw, sizeof(dbPw), &colLen);

    if (strncmp(dbPw, password, 20) != 0) {
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return DBR_WRONG_PW;
    }

    short x = 1000, y = 1000;
    int hp = 100, max_hp = 100, gold = 0;
    int potion_count = 0, scroll_count = 0, weapon_enhance = 0;
    unsigned long long exp = 0;
    unsigned char level = 1, str = 5, intl = 5, dex = 5, luk = 5, sp = 0, vid = 0;

    SQLGetData(hStmt, 2,  SQL_C_SSHORT,   &x,             0, &colLen);
    SQLGetData(hStmt, 3,  SQL_C_SSHORT,   &y,             0, &colLen);
    SQLGetData(hStmt, 4,  SQL_C_LONG,     &hp,            0, &colLen);
    SQLGetData(hStmt, 5,  SQL_C_LONG,     &max_hp,        0, &colLen);
    SQLGetData(hStmt, 6,  SQL_C_UBIGINT,  &exp,           0, &colLen);
    SQLGetData(hStmt, 7,  SQL_C_UTINYINT, &level,         0, &colLen);
    SQLGetData(hStmt, 8,  SQL_C_UTINYINT, &str,           0, &colLen);
    SQLGetData(hStmt, 9,  SQL_C_UTINYINT, &intl,          0, &colLen);
    SQLGetData(hStmt, 10, SQL_C_UTINYINT, &dex,           0, &colLen);
    SQLGetData(hStmt, 11, SQL_C_UTINYINT, &luk,           0, &colLen);
    SQLGetData(hStmt, 12, SQL_C_UTINYINT, &sp,            0, &colLen);
    SQLGetData(hStmt, 13, SQL_C_UTINYINT, &vid,           0, &colLen);
    SQLGetData(hStmt, 14, SQL_C_LONG,     &gold,          0, &colLen);
    SQLGetData(hStmt, 15, SQL_C_LONG,     &potion_count,  0, &colLen);
    SQLGetData(hStmt, 16, SQL_C_LONG,     &scroll_count,  0, &colLen);
    SQLGetData(hStmt, 17, SQL_C_LONG,     &weapon_enhance,0, &colLen);

    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);

    strncpy_s(out.username, username, 20);
    out.x = x; out.y = y;
    out.hp = hp; out.max_hp = max_hp;
    out.exp = exp; out.level = level;
    out.str = str; out.intl = intl; out.dex = dex; out.luk = luk;
    out.stat_points = sp;
    out.visual_id    = vid;
    out.gold         = gold;
    out.potion_count  = potion_count;
    out.scroll_count  = scroll_count;
    out.weapon_enhance = weapon_enhance;

    printf("[DB] Login OK: %s\n", username);
    return DBR_OK;
}

bool Database::SavePlayer(const PlayerSaveData& data)
{
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_connected) return false;

    SQLHSTMT hStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    SQLPrepareA(hStmt,
        (SQLCHAR*)"UPDATE Users SET x=?, y=?, hp=?, max_hp=?, exp=?, level=?, "
                  "str_stat=?, int_stat=?, dex_stat=?, luk_stat=?, stat_pts=?, visual_id=?, gold=?, "
                  "potion_count=?, scroll_count=?, weapon_enhance=? "
                  "WHERE username=?",
        SQL_NTS);

    short x = data.x, y = data.y;
    int hp = data.hp, max_hp = data.max_hp, gld = data.gold;
    int pot = data.potion_count, scr = data.scroll_count, enh = data.weapon_enhance;
    unsigned long long exp = data.exp;
    unsigned char lv = data.level, st = data.str, it = data.intl,
                  dx = data.dex,   lk = data.luk,  sp = data.stat_points,
                  vid = data.visual_id;

    SQLLEN ind = 0, nameInd = SQL_NTS;
    SQLBindParameter(hStmt, 1,  SQL_PARAM_INPUT, SQL_C_SSHORT,   SQL_SMALLINT, 5,  0, &x,      0, &ind);
    SQLBindParameter(hStmt, 2,  SQL_PARAM_INPUT, SQL_C_SSHORT,   SQL_SMALLINT, 5,  0, &y,      0, &ind);
    SQLBindParameter(hStmt, 3,  SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &hp,     0, &ind);
    SQLBindParameter(hStmt, 4,  SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &max_hp, 0, &ind);
    SQLBindParameter(hStmt, 5,  SQL_PARAM_INPUT, SQL_C_UBIGINT,  SQL_BIGINT,   19, 0, &exp,    0, &ind);
    SQLBindParameter(hStmt, 6,  SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,  3,  0, &lv,     0, &ind);
    SQLBindParameter(hStmt, 7,  SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,  3,  0, &st,     0, &ind);
    SQLBindParameter(hStmt, 8,  SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,  3,  0, &it,     0, &ind);
    SQLBindParameter(hStmt, 9,  SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,  3,  0, &dx,     0, &ind);
    SQLBindParameter(hStmt, 10, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,  3,  0, &lk,     0, &ind);
    SQLBindParameter(hStmt, 11, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,  3,  0, &sp,     0, &ind);
    SQLBindParameter(hStmt, 12, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,  3,  0, &vid,    0, &ind);
    SQLBindParameter(hStmt, 13, SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &gld,    0, &ind);
    SQLBindParameter(hStmt, 14, SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &pot,    0, &ind);
    SQLBindParameter(hStmt, 15, SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &scr,    0, &ind);
    SQLBindParameter(hStmt, 16, SQL_PARAM_INPUT, SQL_C_LONG,     SQL_INTEGER,  10, 0, &enh,    0, &ind);
    SQLBindParameter(hStmt, 17, SQL_PARAM_INPUT, SQL_C_CHAR,     SQL_VARCHAR,  20, 0,
        (SQLPOINTER)data.username, 0, &nameInd);

    SQLRETURN ret = SQLExecute(hStmt);
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);

    return SQL_SUCCEEDED(ret);
}
