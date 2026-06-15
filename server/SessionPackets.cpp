#include "pch.h"
#include "SESSION.h"
#include "Database.h"
#include "LuaManager.h"
#include "PartyManager.h"
#include "DungeonManager.h"
#include "MapData.h"

// After teleporting a player to a new position (sector + mX/mY already updated,
// mDungeonInstanceId already set), send AddObject for all newly visible entities.
static void reEstablishVisibility(std::shared_ptr<SESSION> self, int mid)
{
    std::unordered_set<int> new_v;
    self->get_visible_players_from_sectors(new_v);
    for (int vid : new_v) {
        self->sendAddPlayer(vid);
        auto pl = clients.find(vid);
        if (pl != clients.end() && pl->second) pl->second->sendAddPlayer(mid);
    }
    std::unordered_set<int> new_v_npcs;
    self->get_visible_npcs_from_sectors(new_v_npcs);
    for (int nid : new_v_npcs) self->sendAddNpc(nid);
}

// Send RemoveObject for every currently-visible NPC/player, then clear the visibility
// sets.  Call before forcibly teleporting a player so no ghost objects remain.
static void clearVisibilityBeforeTeleport(std::shared_ptr<SESSION> member, int mid)
{
    member->m_visible_mutex.lock();
    auto old_npcs    = member->m_visible_npcs;
    auto old_players = member->m_visible_players;
    member->m_visible_npcs.clear();
    member->m_visible_players.clear();
    member->m_visible_mutex.unlock();

    S2C_RemoveObject rm;
    rm.size = sizeof(S2C_RemoveObject);
    rm.type = S2C_REMOVE_OBJECT;

    for (int nid : old_npcs) {
        rm.object_id = nid;
        member->doSend(rm.size, reinterpret_cast<char*>(&rm));
        auto nit = clients.find(nid);
        if (nit != clients.end() && nit->second) {
            nit->second->m_visible_mutex.lock();
            nit->second->m_visible_players.erase(mid);
            nit->second->m_visible_mutex.unlock();
        }
    }
    for (int pid : old_players) {
        if (pid == mid) continue;
        rm.object_id = pid;
        member->doSend(rm.size, reinterpret_cast<char*>(&rm));
        auto pit = clients.find(pid);
        if (pit != clients.end() && pit->second) {
            S2C_RemoveObject rm2;
            rm2.size      = sizeof(S2C_RemoveObject);
            rm2.type      = S2C_REMOVE_OBJECT;
            rm2.object_id = mid;
            pit->second->doSend(rm2.size, reinterpret_cast<char*>(&rm2));
            pit->second->m_visible_mutex.lock();
            pit->second->m_visible_players.erase(mid);
            pit->second->m_visible_mutex.unlock();
        }
    }
}

bool SESSION::processPacket(unsigned char* p)
{
	if (not is_player) return true;
	CheckSafeZoneRegen();

	PACKET_TYPE type = *reinterpret_cast<PACKET_TYPE*>(&p[1]);
	switch (type) {
	case C2S_LOGIN:
	{
		C2S_Login* packet = reinterpret_cast<C2S_Login*>(p);

		packet->username[MAX_NAME_LEN - 1] = '\0';
		packet->password[MAX_NAME_LEN - 1] = '\0';

		PlayerSaveData saveData;
		int dbRes = Database::Login(packet->username, packet->password, saveData);

		S2C_LoginResult result;
		result.size = sizeof(S2C_LoginResult);
		result.type = S2C_LOGIN_RESULT;

		if (dbRes == DBR_WRONG_PW) {
			result.result = LOGIN_WRONG_PW;
			strncpy_s(result.message, "Wrong password.", sizeof(result.message));
			doSend(result.size, reinterpret_cast<char*>(&result));
			return true;
		}
		if (dbRes == DBR_FAIL) {
			result.result = LOGIN_DB_ERROR;
			strncpy_s(result.message, "Server DB error.", sizeof(result.message));
			doSend(result.size, reinterpret_cast<char*>(&result));
			return true;
		}

		strncpy_s(mUsername, saveData.username, MAX_NAME_LEN);
		mX = saveData.x; mY = saveData.y;
		mExp = saveData.exp; mLevel = saveData.level;
		mMaxHp = 100 + (mLevel - 1) * 10;           // recalculate from level
		mHp    = min(saveData.hp, mMaxHp);
		mStr = saveData.str; mIntl = saveData.intl;
		mDex = saveData.dex; mLuk = saveData.luk;
		mStatPoints = saveData.stat_points;
		mGold = saveData.gold;

		if (dbRes == DBR_NEW_USER) {
			result.result = LOGIN_NEW_USER;
			strncpy_s(result.message, "Welcome! New account created.", sizeof(result.message));
		} else {
			result.result = LOGIN_SUCCESS;
			strncpy_s(result.message, "Welcome back!", sizeof(result.message));
		}
		mVisualId = saveData.visual_id;

		doSend(result.size, reinterpret_cast<char*>(&result));

		std::cout << "Player[" << mId << "] logged in as " << mUsername << std::endl;

		if (dbRes == DBR_NEW_USER) {
			mState = CS_CHAR_SELECT;
		} else {
			enterWorld();
		}
	}
	break;
	case C2S_CHAR_SELECT:
	{
		if (mState != CS_CHAR_SELECT) break;
		C2S_CharSelect* pkt = reinterpret_cast<C2S_CharSelect*>(p);
		mVisualId = pkt->visual_id;
		std::cout << "Player[" << mId << "] chose visual " << (int)mVisualId << std::endl;

		PlayerSaveData sd = {};
		strncpy_s(sd.username, mUsername, MAX_NAME_LEN);
		sd.x = mX; sd.y = mY;
		sd.hp = mHp; sd.max_hp = mMaxHp;
		sd.exp = mExp; sd.level = mLevel;
		sd.str = mStr; sd.intl = mIntl; sd.dex = mDex; sd.luk = mLuk;
		sd.stat_points = mStatPoints;
		sd.visual_id = mVisualId;
		sd.gold = mGold;
		Database::SavePlayer(sd);

		enterWorld();
	}
	break;
	case C2S_MOVE:
	{
		C2S_Move* packet = reinterpret_cast<C2S_Move*>(p);

		// Dungeon players use local coordinates (0..DUNGEON_SIZE-1).
		// Bypass the world sector system entirely.
		if (mDungeonInstanceId >= 0) {
			short nx = packet->x;
			short ny = packet->y;
			if (nx < 0) nx = 0; else if (nx >= (short)DUNGEON_SIZE) nx = (short)(DUNGEON_SIZE - 1);
			if (ny < 0) ny = 0; else if (ny >= (short)DUNGEON_SIZE) ny = (short)(DUNGEON_SIZE - 1);
			mX         = nx;
			mY         = ny;
			mMove_time = packet->move_time;
			mDirection = packet->dir;
			auto self  = clients[mId];
			if (self) {
				auto inst = DungeonManager::GetInstance(mDungeonInstanceId);
				if (inst) inst->OnPlayerMove(self);
			}
			break;
		}

		// Bounds + wall collision check
		short nx = packet->x, ny = packet->y;
		if (nx < 0 || nx >= (short)WORLD_WIDTH || ny < 0 || ny >= (short)WORLD_HEIGHT) break;
		if (!IsWalkable(nx, ny)) break;

		mX = packet->x;
		mY = packet->y;
		mMove_time = packet->move_time;
		mDirection = packet->dir;

		auto old_v_players = m_visible_players;

		int new_sector_id = get_sector_id(mX, mY);
		if (new_sector_id != mSector_id) {
			sectors[mSector_id].erase(mId);
			sectors[new_sector_id].insert(mId);
			mSector_id = new_sector_id;
		}

		std::unordered_set<int> new_v_players;
		get_visible_players_from_sectors(new_v_players);

		for (int id : new_v_players) {
			if (old_v_players.count(id) == 0) {
				sendAddPlayer(id);
				std::shared_ptr<SESSION> pl = clients[id];
				if (!pl) continue;
				pl->sendAddPlayer(mId);
			} else {
				std::shared_ptr<SESSION> pl = clients[id];
				if (!pl) continue;
				pl->sendMovePacket(mId);
			}
		}

		for (int id : old_v_players) {
			if (new_v_players.count(id) == 0) {
				sendRemovePlayer(id);
				std::shared_ptr<SESSION> pl = clients[id];
				if (!pl) continue;
				pl->sendRemovePlayer(mId);
			}
		}

		{
			m_visible_mutex.lock();
			auto old_v_npcs = m_visible_npcs;
			m_visible_mutex.unlock();

			std::unordered_set<int> new_v_npcs;
			get_visible_npcs_from_sectors(new_v_npcs);

			for (int npc_id : new_v_npcs) {
				if (old_v_npcs.count(npc_id) == 0) sendAddNpc(npc_id);
			}
			for (int npc_id : old_v_npcs) {
				if (new_v_npcs.count(npc_id) == 0) sendRemoveNpc(npc_id);
			}
		}
	}
	break;
	case C2S_STAT_INVEST:
	{
		if (mStatPoints == 0) break;
		if (!is_in_safe_zone(mX, mY)) break;
		C2S_StatInvest* packet = reinterpret_cast<C2S_StatInvest*>(p);
		switch (packet->stat_type)
		{
		case STAT_STR: mStr++;  break;
		case STAT_INT: mIntl++; break;
		case STAT_DEX: mDex++;  break;
		case STAT_LUK: mLuk++;  break;
		default: break;
		}
		mStatPoints--;
		sendStatInfo();
		std::cout << "Player[" << mId << "] invested in stat " << (int)packet->stat_type
			<< " (points left: " << (int)mStatPoints << ")\n";
	}
	break;
	case C2S_ATTACK:
	{
		C2S_Attack* atk_pkt = reinterpret_cast<C2S_Attack*>(p);
		mDirection = atk_pkt->dir;

		short dx = 0, dy = 0;
		switch (mDirection) {
		case UP:    dy = -1; break;
		case DOWN:  dy =  1; break;
		case LEFT:  dx = -1; break;
		case RIGHT: dx =  1; break;
		}

		// Perpendicular axis: for horizontal attack → Y offset; for vertical → X offset
		short px = (dx == 0) ? 1 : 0;
		short py = (dy == 0) ? 1 : 0;

		// Hit area: range-1 straight + range-1 perp± + range-2 straight
		const short tiles[4][2] = {
			{ (short)(mX + dx),      (short)(mY + dy)      }, // r1 straight
			{ (short)(mX + dx + px), (short)(mY + dy + py) }, // r1 perp+
			{ (short)(mX + dx - px), (short)(mY + dy - py) }, // r1 perp-
			{ (short)(mX + dx*2),    (short)(mY + dy*2)    }, // r2 straight
		};

		// Dungeon: boss parts are not in world sectors — use bounding-box lookup
		if (mDungeonInstanceId >= 0) {
			auto inst = DungeonManager::GetInstance(mDungeonInstanceId);
			if (inst) {
				int damage  = 10 + static_cast<int>(mStr) * 3;
				bool isCrit = (rand() % 100) < static_cast<int>(mLuk);
				if (isCrit) damage = damage * 3 / 2;
				for (int i = 0; i < 4; ++i) {
					auto part = inst->FindHittablePartAt(tiles[i][0], tiles[i][1]);
					if (part) { inst->OnPartDamage(part, mId, damage, isCrit); break; }
				}
			}
			break;
		}

		int target_id = -1;
		for (int i = 0; i < 4 && target_id == -1; ++i) {
			short tx = tiles[i][0];
			short ty = tiles[i][1];
			if (tx < 0 || tx >= WORLD_WIDTH || ty < 0 || ty >= WORLD_HEIGHT) continue;
			int sid = get_sector_id(tx, ty);
			auto sit = sectors.find(sid);
			if (sit == sectors.end()) continue;
			for (int id : sit->second) {
				if (id == mId) continue;
				auto cit = clients.find(id);
				if (cit == clients.end()) continue;
				std::shared_ptr<SESSION> tgt = cit->second;
				if (!tgt || tgt->mState != CS_PLAYING) continue;
				if (tgt->mX == tx && tgt->mY == ty) { target_id = id; break; }
			}
		}

		if (target_id == -1) break;

		std::shared_ptr<SESSION> target = clients[target_id];
		if (!target) break;
		if (target->is_player) break;

		int damage = 10 + static_cast<int>(mStr) * 3 + mWeaponEnhance * 10;
		bool is_crit = (rand() % 100) < static_cast<int>(mLuk);
		if (is_crit) damage = damage * 3 / 2;

		target->mHp -= damage;

		if (target->mHp <= 0 && target->is_player) target->mHp = 1;

		bool killed = (target->mHp <= 0 && !target->is_player);
		if (killed) target->mHp = 0;

		std::cout << "[Attack] Player[" << mId << "] -> [" << target_id << "]: "
			<< damage << (is_crit ? " CRIT" : "") << (killed ? " (killed)" : "") << "\n";

		{
			S2C_DamageNumber dn;
			dn.size        = sizeof(S2C_DamageNumber);
			dn.type        = S2C_DAMAGE_NUMBER;
			dn.attacker_id = mId;
			dn.object_id   = target_id;
			dn.damage      = damage;
			dn.is_crit     = is_crit ? 1 : 0;
			target->m_visible_mutex.lock();
			auto dn_watchers = target->m_visible_players;
			target->m_visible_mutex.unlock();
			for (int pid : dn_watchers) {
				auto pit = clients.find(pid);
				if (pit == clients.end()) continue;
				std::shared_ptr<SESSION> pl = pit->second;
				if (!pl || pl->mState != CS_PLAYING) continue;
				pl->doSend(dn.size, reinterpret_cast<char*>(&dn));
			}
		}

		if (killed) {
			target->m_visible_mutex.lock();
			auto watchers = target->m_visible_players;
			target->m_visible_players.clear();
			target->m_visible_mutex.unlock();

			S2C_RemoveObject rp;
			rp.size = sizeof(S2C_RemoveObject);
			rp.type = S2C_REMOVE_OBJECT;
			rp.object_id = target_id;
			for (int pid : watchers) {
				auto pit = clients.find(pid);
				if (pit == clients.end()) continue;
				std::shared_ptr<SESSION> pl = pit->second;
				if (!pl || pl->mState != CS_PLAYING) continue;
				pl->m_visible_mutex.lock();
				pl->m_visible_npcs.erase(target_id);
				pl->m_visible_mutex.unlock();
				pl->doSend(rp.size, reinterpret_cast<char*>(&rp));
			}

			target->mIsDead = true;
			sectors[target->mSector_id].erase(target_id);

			unsigned long long kill_exp = static_cast<unsigned long long>(target->mLevel) * target->mLevel * 2ULL;
			mExp += kill_exp;
			while (mLevel < 100) {
				unsigned long long required = static_cast<unsigned long long>(mLevel) * mLevel * 20ULL;
				if (mExp < required) break;
				mExp -= required;
				mLevel++;
				mStatPoints += 5;
				mMaxHp += 10;
				mHp     = min(mHp + 10, mMaxHp);
			}
			mGold += static_cast<int>(target->mLevel) * 10;
			sendGoldUpdate();
			sendAvatarInfo();
			givePartyExp(kill_exp);
			onMonsterKilled();
		} else {
			S2C_StatusChange sc;
			sc.size = sizeof(S2C_StatusChange);
			sc.type = S2C_STATUS_CHANGE;
			sc.object_id = target_id;
			sc.hp = target->mHp;
			sc.max_hp = target->mMaxHp;
			sc.exp = target->mExp;
			sc.level = target->mLevel;

			target->m_visible_mutex.lock();
			auto watchers = target->m_visible_players;
			target->m_visible_mutex.unlock();

			for (int pid : watchers) {
				auto pit = clients.find(pid);
				if (pit == clients.end()) continue;
				std::shared_ptr<SESSION> pl = pit->second;
				if (!pl || pl->mState != CS_PLAYING) continue;
				pl->doSend(sc.size, reinterpret_cast<char*>(&sc));
			}

			if (target->is_player)
				target->doSend(sc.size, reinterpret_cast<char*>(&sc));

			if (!target->is_player) {
				target->mTargetId       = mId;
				target->mChaseRemaining = 20;
			}
		}
	}
	break;
	case C2S_AOE_ATTACK:
	{
		C2S_AoeAttack* aoe_pkt = reinterpret_cast<C2S_AoeAttack*>(p);
		mDirection = aoe_pkt->dir;

		static const short dx_off[] = { -1,  0,  1, -1, 1, -1, 0, 1 };
		static const short dy_off[] = { -1, -1, -1,  0, 0,  1, 1, 1 };

		// Dungeon: boss parts are not in world sectors — use bounding-box lookup
		if (mDungeonInstanceId >= 0) {
			auto inst = DungeonManager::GetInstance(mDungeonInstanceId);
			if (inst) {
				int damage  = 15 + static_cast<int>(mIntl) * 4 + mWeaponEnhance * 10;
				bool isCrit = (rand() % 100) < static_cast<int>(mLuk);
				if (isCrit) damage = damage * 3 / 2;
				std::shared_ptr<SESSION> alreadyHit;
				for (int i = 0; i < 8; ++i) {
					auto part = inst->FindHittablePartAt(
						(short)(mX + dx_off[i]), (short)(mY + dy_off[i]));
					if (part && part != alreadyHit) {
						inst->OnPartDamage(part, mId, damage, isCrit);
						alreadyHit = part;  // hit each part at most once per AOE
					}
				}
			}
			break;
		}

		std::vector<int> targets;
		for (int i = 0; i < 8; ++i) {
			short tx = mX + dx_off[i];
			short ty = mY + dy_off[i];
			if (tx < 0 || tx >= WORLD_WIDTH || ty < 0 || ty >= WORLD_HEIGHT) continue;
			int sid = get_sector_id(tx, ty);
			auto sit = sectors.find(sid);
			if (sit == sectors.end()) continue;
			for (int id : sit->second) {
				if (id == mId) continue;
				auto cit = clients.find(id);
				if (cit == clients.end()) continue;
				std::shared_ptr<SESSION> tgt = cit->second;
				if (!tgt || tgt->mState != CS_PLAYING) continue;
				if (tgt->mX == tx && tgt->mY == ty)
					targets.push_back(id);
			}
		}

		bool stat_changed = false;
		for (int target_id : targets) {
			std::shared_ptr<SESSION> target = clients[target_id];
			if (!target) continue;
			if (target->is_player) continue;

			int damage = 15 + static_cast<int>(mIntl) * 4 + mWeaponEnhance * 10;
			bool is_crit = (rand() % 100) < static_cast<int>(mLuk);
			if (is_crit) damage = damage * 3 / 2;

			target->mHp -= damage;
			if (target->mHp <= 0 && target->is_player) target->mHp = 1;
			bool killed = (target->mHp <= 0 && !target->is_player);

			{
				S2C_DamageNumber dn;
				dn.size        = sizeof(S2C_DamageNumber);
				dn.type        = S2C_DAMAGE_NUMBER;
				dn.attacker_id = mId;
				dn.object_id   = target_id;
				dn.damage      = damage;
				dn.is_crit     = is_crit ? 1 : 0;
				target->m_visible_mutex.lock();
				auto dn_w = target->m_visible_players;
				target->m_visible_mutex.unlock();
				for (int pid : dn_w) {
					auto pit = clients.find(pid);
					if (pit == clients.end()) continue;
					std::shared_ptr<SESSION> pl = pit->second;
					if (!pl || pl->mState != CS_PLAYING) continue;
					pl->doSend(dn.size, reinterpret_cast<char*>(&dn));
				}
			}
			if (killed) target->mHp = 0;

			if (killed) {
				target->m_visible_mutex.lock();
				auto watchers = target->m_visible_players;
				target->m_visible_players.clear();
				target->m_visible_mutex.unlock();

				S2C_RemoveObject rp;
				rp.size = sizeof(S2C_RemoveObject);
				rp.type = S2C_REMOVE_OBJECT;
				rp.object_id = target_id;
				for (int pid : watchers) {
					auto pit = clients.find(pid);
					if (pit == clients.end()) continue;
					std::shared_ptr<SESSION> pl = pit->second;
					if (!pl || pl->mState != CS_PLAYING) continue;
					pl->m_visible_mutex.lock();
					pl->m_visible_npcs.erase(target_id);
					pl->m_visible_mutex.unlock();
					pl->doSend(rp.size, reinterpret_cast<char*>(&rp));
				}
				target->mHp = target->mMaxHp;

				unsigned long long kill_exp = static_cast<unsigned long long>(target->mLevel) * target->mLevel * 2ULL;
				mExp += kill_exp;
				while (mLevel < 100) {
					unsigned long long required = static_cast<unsigned long long>(mLevel) * mLevel * 20ULL;
					if (mExp < required) break;
					mExp -= required;
					mLevel++;
					mStatPoints += 5;
					mMaxHp += 10;
					mHp     = min(mHp + 10, mMaxHp);
				}
				mGold += static_cast<int>(target->mLevel) * 10;
				givePartyExp(kill_exp);
				onMonsterKilled();
				stat_changed = true;
			} else {
				S2C_StatusChange sc;
				sc.size = sizeof(S2C_StatusChange);
				sc.type = S2C_STATUS_CHANGE;
				sc.object_id = target_id;
				sc.hp = target->mHp;
				sc.max_hp = target->mMaxHp;
				sc.exp = target->mExp;
				sc.level = target->mLevel;

				target->m_visible_mutex.lock();
				auto watchers = target->m_visible_players;
				target->m_visible_mutex.unlock();
				for (int pid : watchers) {
					auto pit = clients.find(pid);
					if (pit == clients.end()) continue;
					std::shared_ptr<SESSION> pl = pit->second;
					if (!pl || pl->mState != CS_PLAYING) continue;
					pl->doSend(sc.size, reinterpret_cast<char*>(&sc));
				}
				if (target->is_player)
					target->doSend(sc.size, reinterpret_cast<char*>(&sc));

				if (!target->is_player) {
					target->mTargetId       = mId;
					target->mChaseRemaining = 20;
				}
			}
		}

		if (stat_changed) { sendGoldUpdate(); sendAvatarInfo(); }
	}
	break;
	case C2S_ENHANCE_WEAPON:
	{
		if (mWeaponEnhance >= 25) break;

		// Gold cost per level (index = current level, 0-24)
		static const int ENHANCE_COST[25] = {
			500,   500,   500,   500,   500,   // +0→+5
			1500,  1500,  1500,  1500,  1500,  // +5→+10
			5000,  5000,  5000,  5000,  5000,  // +10→+15
			15000, 15000, 15000, 15000, 15000, // +15→+20
			50000, 50000, 50000, 50000, 50000, // +20→+25
		};
		// Success rate % (index = current level)
		static const int ENHANCE_SUCCESS[25] = {
			90, 85, 80, 75, 70,  // +0~+4
			65, 60, 55, 50, 45,  // +5~+9
			40, 35, 30, 28, 25,  // +10~+14
			22, 20, 18, 15, 13,  // +15~+19
			11,  9,  7,  5,  3,  // +20~+24
		};
		// Reset chance % on failure (0 = no reset, only from +12 onwards)
		static const int ENHANCE_RESET[25] = {
			 0,  0,  0,  0,  0,  // +0~+4
			 0,  0,  0,  0,  0,  // +5~+9
			 0,  0,  5,  8, 12,  // +10~+14 (+12 starts at 5%)
			16, 20, 25, 30, 35,  // +15~+19
			40, 50, 60, 70, 80,  // +20~+24
		};

		int lv   = mWeaponEnhance;
		int cost = ENHANCE_COST[lv];
		if (mGold < cost) {
			S2C_EnhanceResult res;
			res.size      = sizeof(S2C_EnhanceResult);
			res.type      = S2C_ENHANCE_RESULT;
			res.result    = 0;
			res.new_level = (unsigned char)mWeaponEnhance;
			res.gold      = mGold;
			doSend(res.size, reinterpret_cast<char*>(&res));
			break;
		}
		mGold -= cost;

		unsigned char result;
		int roll = rand() % 100;
		if (roll < ENHANCE_SUCCESS[lv]) {
			mWeaponEnhance++;
			result = 1; // success
		} else {
			int resetRoll = rand() % 100;
			if (ENHANCE_RESET[lv] > 0 && resetRoll < ENHANCE_RESET[lv]) {
				mWeaponEnhance = 0;
				result = 2; // fail + reset
			} else {
				result = 0; // fail, level unchanged
			}
		}

		S2C_EnhanceResult res;
		res.size      = sizeof(S2C_EnhanceResult);
		res.type      = S2C_ENHANCE_RESULT;
		res.result    = result;
		res.new_level = (unsigned char)mWeaponEnhance;
		res.gold      = mGold;
		doSend(res.size, reinterpret_cast<char*>(&res));
	}
	break;
	case C2S_BUY_ITEM:
	{
		C2S_BuyItem* pkt = reinterpret_cast<C2S_BuyItem*>(p);
		ITEM_TYPE item = pkt->item_type;

		int price = (item == ITEM_HP_POTION) ? SHOP_POTION_PRICE : SHOP_TELEPORT_PRICE;
		if (mGold < price) {
			sendBuyResult(false, item, 0, 0, 0);
			break;
		}
		mGold -= price;

		// Add to inventory — effect is applied later via C2S_USE_ITEM
		if (item == ITEM_HP_POTION) {
			mPotionCount = min(mPotionCount + 1, 99);
			sendBuyResult(true, item, mPotionCount, 0, 0);
		} else {
			mScrollCount = min(mScrollCount + 1, 99);
			sendBuyResult(true, item, mScrollCount, 0, 0);
		}
		sendGoldUpdate();
	}
	break;
	case C2S_USE_ITEM:
	{
		C2S_UseItem* pkt = reinterpret_cast<C2S_UseItem*>(p);
		ITEM_TYPE item = pkt->item_type;

		if (item == ITEM_HP_POTION) {
			if (mPotionCount <= 0) {
				sendUseItemResult(false, item, 0, mHp, 0, 0);
				break;
			}
			mPotionCount--;
			int heal = max(50, mMaxHp / 3);
			mHp = min(mMaxHp, mHp + heal);
			sendUseItemResult(true, item, mPotionCount, mHp, 0, 0);
		} else {
			if (mScrollCount <= 0) {
				sendUseItemResult(false, item, 0, 0, 0, 0);
				break;
			}
			mScrollCount--;
			// Individual escape: remove self from dungeon (if inside), then teleport to town.
			if (mDungeonInstanceId >= 0) {
				int old_inst       = mDungeonInstanceId;
				mDungeonInstanceId = -1;
				auto inst = DungeonManager::GetInstance(old_inst);
				if (inst) {
					inst->RemovePlayer(mId);
					if (inst->IsEmpty()) DungeonManager::CloseInstance(old_inst);
				}
				// Player was removed from world sectors on dungeon entry; no erase needed.
			} else {
				auto self = clients[mId];
				if (self) clearVisibilityBeforeTeleport(self, mId);
				sectors[mSector_id].erase(mId);
			}
			mX = 1000; mY = 1000;
			mSector_id = get_sector_id(mX, mY);
			sectors[mSector_id].insert(mId);
			sendUseItemResult(true, item, mScrollCount, 0, mX, mY);
			{
				auto self = clients[mId];
				if (self) reEstablishVisibility(self, mId);
			}
		}
	}
	break;
	case C2S_CHAT:
	{
		C2S_Chat* packet = reinterpret_cast<C2S_Chat*>(p);
		packet->message[MAX_CHAT_MSG_LEN - 1] = '\0';
		char* msg = packet->message;

		// ── GM cheat commands (start with '/') ───────────────────────────────
		if (msg[0] == '/') {
			// Helper: send a private server notice back to this player only
			auto sendNotice = [&](const char* text) {
				S2C_ChatMessage n;
				n.size = sizeof(S2C_ChatMessage);
				n.type = S2C_CHAT_MESSAGE;
				n.object_id = mId;
				strncpy_s(n.message, text, MAX_CHAT_MSG_LEN - 1);
				n.message[MAX_CHAT_MSG_LEN - 1] = '\0';
				doSend(n.size, reinterpret_cast<char*>(&n));
			};

			// /levelup N  ─── raise level by N, grant stat points, refill HP
			int lvN = 0;
			if (sscanf_s(msg, "/levelup %d", &lvN) == 1 && lvN > 0) {
				int gained = min(lvN, 255 - (int)mLevel);
				mLevel      = (unsigned char)(mLevel + gained);
				int newSP   = (int)mStatPoints + gained * 5;
				mStatPoints = (unsigned char)min(newSP, 255);
				mMaxHp     += gained * 100;
				mHp         = mMaxHp;
				sendAvatarInfo();
				sendStatInfo();
				char buf[64];
				sprintf_s(buf, "[GM] Level %d  HP %d", (int)mLevel, mMaxHp);
				sendNotice(buf);
				break;
			}

			// /showmethemoney  ─── add 999999 gold
			if (strncmp(msg, "/showmethemoney", 15) == 0 &&
			    (msg[15] == '\0' || msg[15] == ' ')) {
				mGold = min(mGold + 999999, 999999999);
				sendGoldUpdate();
				sendNotice("[GM] +999999 Gold");
				break;
			}

			// /tp x, y  or  /tp x y  ─── teleport to tile
			int tpX = -1, tpY = -1;
			if (sscanf_s(msg, "/tp %d , %d", &tpX, &tpY) != 2)
				if (sscanf_s(msg, "/tp %d, %d", &tpX, &tpY) != 2)
					sscanf_s(msg, "/tp %d %d",  &tpX, &tpY);
			if (tpX >= 0 && tpY >= 0) {
				if (mDungeonInstanceId >= 0) {
					// Dungeon: local tile (0..DUNGEON_SIZE-1)
					mX = (short)max(0, min(tpX, DUNGEON_SIZE - 1));
					mY = (short)max(0, min(tpY, DUNGEON_SIZE - 1));
					auto self = clients[mId];
					if (self) {
						auto inst = DungeonManager::GetInstance(mDungeonInstanceId);
						if (inst) inst->OnPlayerMove(self);
					}
					sendRespawn();
				} else {
					// World: tile (0..1999)
					short nx = (short)max(0, min(tpX, 1999));
					short ny = (short)max(0, min(tpY, 1999));
					auto self = clients[mId];
					if (self) clearVisibilityBeforeTeleport(self, mId);
					sectors[mSector_id].erase(mId);
					mX = nx; mY = ny;
					mSector_id = get_sector_id(mX, mY);
					sectors[mSector_id].insert(mId);
					sendRespawn();
					if (self) reEstablishVisibility(self, mId);
				}
				char buf[48];
				sprintf_s(buf, "[GM] Teleported to (%d, %d)", (int)mX, (int)mY);
				sendNotice(buf);
				break;
			}

			// /godmode  ─── toggle invincibility
			bool isInvCmd = (strncmp(msg, "/godmode", 8) == 0 && (msg[8] == '\0' || msg[8] == ' '));
			if (isInvCmd) {
				mInvincible = !mInvincible;
				sendNotice(mInvincible ? "[GM] Invincible ON" : "[GM] Invincible OFF");
				break;
			}

			// Unknown slash command: fall through to broadcast it as normal chat
		}
		// ─────────────────────────────────────────────────────────────────────

		S2C_ChatMessage chatPacket;
		chatPacket.size = sizeof(S2C_ChatMessage);
		chatPacket.type = S2C_CHAT_MESSAGE;
		chatPacket.object_id = mId;
		strncpy_s(chatPacket.message, msg, static_cast<size_t>(MAX_CHAT_MSG_LEN - 1));
		chatPacket.message[MAX_CHAT_MSG_LEN - 1] = '\0';

		doSend(chatPacket.size, reinterpret_cast<char*>(&chatPacket));

		m_visible_mutex.lock();
		auto visible_copy = m_visible_players;
		m_visible_mutex.unlock();

		for (int id : visible_copy) {
			auto it = clients.find(id);
			if (it == clients.end()) continue;
			std::shared_ptr<SESSION> pl = it->second;
			if (!pl || pl->mState != CS_PLAYING) continue;
			pl->doSend(chatPacket.size, reinterpret_cast<char*>(&chatPacket));
		}

		std::cout << "[Chat] " << mUsername << ": " << msg << std::endl;
	}
	break;
	case C2S_PARTY_CREATE:
	{
		if (mPartyId >= 0) break;
		int newId = PartyManager::CreateParty(mId);
		if (newId < 0) break;
		mPartyId = newId;
		sendPartyUpdate(newId);
		std::cout << "Player[" << mId << "] created party " << newId << "\n";
	}
	break;
	case C2S_PARTY_JOIN:
	{
		if (mPartyId >= 0) break;
		C2S_PartyJoin* pkt = reinterpret_cast<C2S_PartyJoin*>(p);
		int targetParty = pkt->party_id;
		if (!PartyManager::JoinParty(targetParty, mId)) break;
		mPartyId = targetParty;
		broadcastPartyUpdate(targetParty);
		std::cout << "Player[" << mId << "] joined party " << targetParty << "\n";
	}
	break;
	case C2S_PARTY_LEAVE:
	{
		if (mPartyId < 0) break;
		int oldParty = mPartyId;
		PartyManager::LeaveParty(mId);
		mPartyId = -1;
		{
			S2C_PartyUpdate out{};
			out.size = sizeof(S2C_PartyUpdate);
			out.type = S2C_PARTY_UPDATE;
			out.party_id = -1;
			out.member_count = 0;
			doSend(out.size, reinterpret_cast<char*>(&out));
		}
		broadcastPartyUpdate(oldParty);
		std::cout << "Player[" << mId << "] left party " << oldParty << "\n";
	}
	break;
	case C2S_PARTY_LIST_REQ:
	{
		sendPartyList();
	}
	break;
	case C2S_QUEST_INTERACT:
	{
		int dx = (int)mX - 994;
		int dy = (int)mY - 1007;
		if (dx * dx + dy * dy > 9) break;

		auto levelUp = [&]() {
			while (mLevel < 100) {
				unsigned long long req = (unsigned long long)mLevel * mLevel * 20ULL;
				if (mExp < req) break;
				mExp -= req;
				mLevel++;
				mStatPoints += 5;
				mMaxHp += 10;
				mHp     = min(mHp + 10, mMaxHp);
			}
		};

		if (mQuests[0].state == 0) {
			LuaManager::QuestInfo qi;
			if (!LuaManager::GetQuestInfo(0, qi)) { qi.goal = 1; qi.exp = 100; qi.gold = 50; }
			mQuests[0].goal     = (unsigned char)qi.goal;
			mQuests[0].progress = (unsigned char)qi.goal;
			mQuests[0].state    = 2;
			sendQuestUpdate(0);
			break;
		}
		if (mQuests[0].state == 2) {
			LuaManager::QuestInfo qi;
			if (!LuaManager::GetQuestInfo(0, qi)) { qi.exp = 100; qi.gold = 50; }
			mExp  += (unsigned long long)qi.exp;
			mGold += qi.gold;
			levelUp();
			mQuests[0].state = 3;
			sendQuestUpdate(0);
			sendGoldUpdate();
			sendAvatarInfo();
			LuaManager::QuestInfo qi1;
			if (!LuaManager::GetQuestInfo(1, qi1)) { qi1.goal = 5; qi1.exp = 500; qi1.gold = 200; }
			mQuests[1].state    = 1;
			mQuests[1].progress = 0;
			mQuests[1].goal     = (unsigned char)qi1.goal;
			sendQuestUpdate(1);
			break;
		}
		if (mQuests[1].state == 1) {
			sendQuestUpdate(1);
			break;
		}
		if (mQuests[1].state == 2) {
			LuaManager::QuestInfo qi;
			if (!LuaManager::GetQuestInfo(1, qi)) { qi.exp = 500; qi.gold = 200; }
			mExp  += (unsigned long long)qi.exp;
			mGold += qi.gold;
			levelUp();
			mQuests[1].state = 3;
			sendQuestUpdate(1);
			sendGoldUpdate();
			sendAvatarInfo();
			break;
		}
		sendQuestUpdate(0);
		sendQuestUpdate(1);
	}
	break;
	case C2S_DUNGEON_ENTER:
	{
		if (mState != CS_PLAYING) break;
		if (mPartyId < 0) break;

		std::vector<int> members = PartyManager::GetMembers(mPartyId);
		if (members.empty() || members[0] != mId) break;  // party leader only

		// ── EXIT: leader takes whole party back to world ──────────────────────
		if (mDungeonInstanceId >= 0) {
			int old_inst = mDungeonInstanceId;
			auto inst = DungeonManager::GetInstance(old_inst);

			// Collect members currently in this instance
			std::vector<std::shared_ptr<SESSION>> in_dungeon;
			for (int mid : members) {
				auto mit = clients.find(mid);
				if (mit == clients.end() || !mit->second) continue;
				if (mit->second->mDungeonInstanceId == old_inst)
					in_dungeon.push_back(mit->second);
			}

			// Remove each member from the dungeon instance (sends REMOVE_OBJECT)
			for (auto& member : in_dungeon) {
				if (inst) inst->RemovePlayer(member->mId);
				member->mDungeonInstanceId = -1;
				member->mX         = 1000;
				member->mY         = 1000;
				member->mSector_id = get_sector_id(1000, 1000);
				sectors[member->mSector_id].insert(member->mId);

				S2C_DungeonEnter pkt;
				pkt.size        = sizeof(S2C_DungeonEnter);
				pkt.type        = S2C_DUNGEON_ENTER;
				pkt.entered     = 0;
				pkt.instance_id = -1;
				pkt.x           = 1000;
				pkt.y           = 1000;
				member->doSend(pkt.size, reinterpret_cast<char*>(&pkt));
			}

			// Re-establish world visibility for each returned member
			for (auto& member : in_dungeon)
				reEstablishVisibility(member, member->mId);

			DungeonManager::CloseInstance(old_inst);

			std::cout << "[Dungeon] Party " << mPartyId
			          << " exited instance " << old_inst << "\n";
			break;
		}

		// ── ENTRY: leader brings whole party into a new instance ──────────────
		if (mLevel < 60) break;  // minimum level 60 to enter boss dungeon

		int dx = (int)mX - 994;
		int dy = (int)mY - 1007;
		if (dx * dx + dy * dy > 9) break;          // must be near Quest NPC

		if (DungeonManager::GetInstanceForParty(mPartyId) >= 0) break; // already inside

		int inst_id = DungeonManager::CreateInstance(mPartyId);
		if (inst_id < 0) break;

		auto inst = DungeonManager::GetInstance(inst_id);
		if (!inst) { DungeonManager::CloseInstance(inst_id); break; }

		// Collect and teleport all online party members into the dungeon
		// Local spawn coordinates: tile (1, 15) -- independent of world space
		const short spawn_x = DungeonManager::SpawnX();   // = 1
		const short spawn_y = DungeonManager::SpawnY();   // = 15

		std::vector<std::shared_ptr<SESSION>> entering;
		for (int mid : members) {
			auto mit = clients.find(mid);
			if (mit == clients.end() || !mit->second) continue;
			std::shared_ptr<SESSION> member = mit->second;
			if (!member->is_player || member->mState != CS_PLAYING) continue;

			// Clear world visibility (sends REMOVE_OBJECT to/from world players)
			clearVisibilityBeforeTeleport(member, mid);

			// Remove from world sector; dungeon players are NOT in any world sector
			sectors[member->mSector_id].erase(mid);

			// Set dungeon-local position (NOT world offset)
			member->mX                 = spawn_x;
			member->mY                 = spawn_y;
			member->mDungeonInstanceId = inst_id;
			// mSector_id left as stale world value; harmless since player is in no sector

			S2C_DungeonEnter pkt;
			pkt.size        = sizeof(S2C_DungeonEnter);
			pkt.type        = S2C_DUNGEON_ENTER;
			pkt.entered     = 1;
			pkt.instance_id = inst_id;
			pkt.x           = spawn_x;
			pkt.y           = spawn_y;
			member->doSend(pkt.size, reinterpret_cast<char*>(&pkt));

			entering.push_back(member);
		}

		// Add to instance: DungeonInstance sends boss + partner ADD_OBJECT packets
		for (auto& member : entering)
			inst->AddPlayer(member);

		std::cout << "[Dungeon] Party " << mPartyId << " entered instance " << inst_id
		          << " (local spawn " << spawn_x << "," << spawn_y << ")\n";
	}
	break;
	default:
		std::cout << "Unknown packet type from player[" << mId << "].\n";
		return false;
	}
	return true;
}
