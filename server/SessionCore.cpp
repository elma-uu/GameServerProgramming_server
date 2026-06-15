#include "pch.h"
#include "SESSION.h"
#include "LuaManager.h"

void SESSION::sendAvatarInfo()
{
	S2C_AvatarInfo packet;
	packet.size     = sizeof(S2C_AvatarInfo);
	packet.type     = S2C_AVATAR_INFO;
	packet.playerId = mId;
	packet.visualId = mVisualId;
	packet.x        = mX;
	packet.y        = mY;
	packet.exp      = mExp;
	packet.level    = mLevel;
	packet.hp       = mHp;
	packet.max_hp   = mMaxHp;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
	sendStatInfo();
}

void SESSION::sendStatInfo()
{
	S2C_StatInfo packet;
	packet.size        = sizeof(S2C_StatInfo);
	packet.type        = S2C_STAT_INFO;
	packet.object_id   = mId;
	packet.str         = mStr;
	packet.intl        = mIntl;
	packet.dex         = mDex;
	packet.luk         = mLuk;
	packet.stat_points = mStatPoints;
	doSend(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::sendGoldUpdate()
{
	S2C_GoldUpdate pkt;
	pkt.size = sizeof(S2C_GoldUpdate);
	pkt.type = S2C_GOLD_UPDATE;
	pkt.gold = mGold;
	doSend(pkt.size, reinterpret_cast<char*>(&pkt));
}

// Safe-zone HP regeneration — called once per packet received while in the world.
// Heals 5% of max HP every 3 seconds when the player is near world spawn (tile radius 30).
void SESSION::CheckSafeZoneRegen()
{
	if (mState != CS_PLAYING) return;
	if (mIsDead) return;
	if (mDungeonInstanceId >= 0) return;  // no regen inside dungeons
	if (mHp >= mMaxHp) return;            // already full

	DWORD now = (DWORD)GetTickCount64();
	if (mLastRegenTick == 0) { mLastRegenTick = now; return; }
	if (now - mLastRegenTick < 3000) return;

	// Radius-30 circular safe zone around town spawn (1000, 1000)
	int dx = (int)mX - 1000;
	int dy = (int)mY - 1000;
	if (dx * dx + dy * dy > 30 * 30) return;

	mLastRegenTick = now;
	int heal = max(1, mMaxHp * 5 / 100);
	mHp = min(mMaxHp, mHp + heal);
	sendAvatarInfo();
}

// new_hp field is repurposed to carry the item count after purchase.
void SESSION::sendBuyResult(bool success, ITEM_TYPE item, int itemCount, short /*unused_x*/, short /*unused_y*/)
{
	S2C_BuyResult pkt;
	pkt.size      = sizeof(S2C_BuyResult);
	pkt.type      = S2C_BUY_RESULT;
	pkt.success   = success ? 1 : 0;
	pkt.item_type = item;
	pkt.gold      = mGold;
	pkt.new_hp    = itemCount;  // client reads this as the new inventory count
	pkt.new_x     = 0;
	pkt.new_y     = 0;
	doSend(pkt.size, reinterpret_cast<char*>(&pkt));
}

void SESSION::sendUseItemResult(bool success, ITEM_TYPE item, int itemCount, int newHp, short newX, short newY)
{
	S2C_UseItemResult pkt;
	pkt.size       = sizeof(S2C_UseItemResult);
	pkt.type       = S2C_USE_ITEM_RESULT;
	pkt.success    = success ? 1 : 0;
	pkt.item_type  = item;
	pkt.item_count = itemCount;
	pkt.new_hp     = newHp;
	pkt.new_x      = newX;
	pkt.new_y      = newY;
	doSend(pkt.size, reinterpret_cast<char*>(&pkt));
}

void SESSION::sendRespawn()
{
	S2C_Respawn pkt;
	pkt.size   = sizeof(S2C_Respawn);
	pkt.type   = S2C_RESPAWN;
	pkt.hp     = mHp;
	pkt.max_hp = mMaxHp;
	pkt.x      = mX;
	pkt.y      = mY;
	doSend(pkt.size, reinterpret_cast<char*>(&pkt));
}

void SESSION::sendQuestUpdate(int questId)
{
	if (questId < 0 || questId > 1) return;
	S2C_QuestUpdate pkt;
	pkt.size        = sizeof(S2C_QuestUpdate);
	pkt.type        = S2C_QUEST_UPDATE;
	pkt.quest_id    = (unsigned char)questId;
	pkt.quest_state = mQuests[questId].state;
	pkt.progress    = mQuests[questId].progress;
	pkt.goal        = mQuests[questId].goal;
	doSend(pkt.size, reinterpret_cast<char*>(&pkt));
}

void SESSION::onMonsterKilled()
{
	if (mQuests[1].state != 1) return;
	mQuests[1].progress++;
	if (mQuests[1].progress >= mQuests[1].goal) {
		mQuests[1].state    = 2;
		mQuests[1].progress = mQuests[1].goal;
	}
	sendQuestUpdate(1);
}

void SESSION::enterWorld()
{
	// Dungeon state is not persisted — if saved inside a dungeon, spawn at town
	if (mX >= DUNGEON_BASE_X) {
		mX = 1000; mY = 1000;
		mDungeonInstanceId = -1;
	}

	int initial_sector_id = get_sector_id(mX, mY);
	{
		SectorLock _lk(initial_sector_id);
		sectors[initial_sector_id].insert(mId);
	}
	mSector_id = initial_sector_id;

	sendAvatarInfo();
	mState = CS_PLAYING;

	// Sync inventory counts so client display matches server state after re-login
	{
		S2C_BuyResult p;
		p.size      = sizeof(S2C_BuyResult);
		p.type      = S2C_BUY_RESULT;
		p.success   = 1;
		p.gold      = mGold;
		p.new_x     = 0; p.new_y = 0;

		p.item_type = ITEM_HP_POTION;
		p.new_hp    = mPotionCount;
		doSend(p.size, reinterpret_cast<char*>(&p));

		p.item_type = ITEM_TELEPORT_SCROLL;
		p.new_hp    = mScrollCount;
		doSend(p.size, reinterpret_cast<char*>(&p));
	}
	if (mWeaponEnhance > 0) {
		S2C_EnhanceResult e;
		e.size      = sizeof(S2C_EnhanceResult);
		e.type      = S2C_ENHANCE_RESULT;
		e.result    = 1;
		e.new_level = (unsigned char)mWeaponEnhance;
		e.gold      = mGold;
		doSend(e.size, reinterpret_cast<char*>(&e));
	}

	std::unordered_set<int> new_v_players;
	get_visible_players_from_sectors(new_v_players);
	for (int id : new_v_players) {
		sendAddPlayer(id);
		std::shared_ptr<SESSION> pl = clients[id];
		if (!pl) continue;
		pl->sendAddPlayer(mId);
	}

	std::unordered_set<int> visible_npcs;
	get_visible_npcs_from_sectors(visible_npcs);
	for (int npc_id : visible_npcs)
		sendAddNpc(npc_id);

	sendGoldUpdate();
}
