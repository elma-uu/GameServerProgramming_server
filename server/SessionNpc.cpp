#include "pch.h"
#include "SESSION.h"
#include "LuaManager.h"
#include "MapData.h"

void SESSION::doNpcMove()
{
	if (mIsDead || mIsStationary) return;

	// Each doNpcMove call = 500 ms; cooldown 2 ticks = 1 second
	if (mAttackTick > 0) mAttackTick--;

	thread_local std::mt19937 rng(std::random_device{}());
	thread_local std::uniform_int_distribution<int> dir_dist(0, 3);

	short newX = mX, newY = mY;

	if (mTargetId >= 0) {
		auto it = clients.find(mTargetId);
		bool valid = (it != clients.end() && it->second && it->second->mState == CS_PLAYING
		              && !it->second->mIsDead);
		if (!valid) {
			mTargetId = -1;
			mChaseRemaining = 0;
		} else {
			auto tgt = it->second;
			if (is_in_safe_zone(tgt->mX, tgt->mY)) {
				mTargetId = -1;
				mChaseRemaining = 0;
			} else {
				if (abs(mX - tgt->mX) <= 1 && abs(mY - tgt->mY) <= 1) {
					if (mAttackTick == 0) {
						int damage = max(1, (int)mLevel * 2);
						if (tgt->mInvincible) damage = 0;
						tgt->mHp -= damage;

						{
							S2C_DamageNumber dn;
							dn.size        = sizeof(S2C_DamageNumber);
							dn.type        = S2C_DAMAGE_NUMBER;
							dn.attacker_id = mId;
							dn.object_id   = tgt->mId;
							dn.damage      = damage;
							dn.is_crit     = 0;
							tgt->m_visible_mutex.lock();
							auto watchers = tgt->m_visible_players;
							tgt->m_visible_mutex.unlock();
							for (int pid : watchers) {
								auto pit = clients.find(pid);
								if (pit == clients.end()) continue;
								auto pl = pit->second;
								if (!pl || pl->mState != CS_PLAYING) continue;
								pl->doSend(dn.size, reinterpret_cast<char*>(&dn));
							}
							tgt->doSend(dn.size, reinterpret_cast<char*>(&dn));
						}

						mAttackTick = 2;

						if (tgt->mHp <= 0) {
							tgt->mHp    = 0;
							tgt->mExp  /= 2;
							tgt->mIsDead = true;

							// Notify dying player and all visible players: play die animation
							S2C_PlayerDie dp;
							dp.size      = sizeof(S2C_PlayerDie);
							dp.type      = S2C_PLAYER_DIE;
							dp.object_id = tgt->mId;
							tgt->doSend(dp.size, reinterpret_cast<char*>(&dp));
							{
								tgt->m_visible_mutex.lock();
								auto watchers = tgt->m_visible_players;
								tgt->m_visible_mutex.unlock();
								for (int wid : watchers) {
									auto wit = clients.find(wid);
									if (wit == clients.end()) continue;
									auto wpl = wit->second;
									if (!wpl || wpl->mState != CS_PLAYING) continue;
									wpl->doSend(dp.size, reinterpret_cast<char*>(&dp));
								}
							}

							// Also update the dying player's own HP display
							tgt->sendAvatarInfo();

							mTargetId      = -1;
							mChaseRemaining = 0;

							// Respawn at world spawn after 10 seconds (not in dungeon)
							int pid = tgt->mId;
							std::thread([pid]() {
								std::this_thread::sleep_for(std::chrono::seconds(10));
								auto it2 = clients.find(pid);
								if (it2 == clients.end()) return;
								auto p = it2->second;
								if (!p || !p->mIsDead || p->mDungeonInstanceId >= 0) return;
								p->mHp    = p->mMaxHp;
								p->mIsDead = false;
								{
									std::lock_guard<std::mutex> lk(g_sectors_mutex);
									sectors[p->mSector_id].erase(p->mId);
								}
								p->mX = 1000;
								p->mY = 1000;
								p->mSector_id = get_sector_id(1000, 1000);
								{
									std::lock_guard<std::mutex> lk(g_sectors_mutex);
									sectors[p->mSector_id].insert(p->mId);
								}
								p->sendRespawn();
								p->sendAvatarInfo();
							}).detach();
						} else {
							S2C_StatusChange sc;
							sc.size      = sizeof(S2C_StatusChange);
							sc.type      = S2C_STATUS_CHANGE;
							sc.object_id = tgt->mId;
							sc.hp        = tgt->mHp;
							sc.max_hp    = tgt->mMaxHp;
							sc.exp       = tgt->mExp;
							sc.level     = tgt->mLevel;
							tgt->doSend(sc.size, reinterpret_cast<char*>(&sc));
						}
					}
					return; // in attack range — don't move
				}

				short dx = 0, dy = 0;
				bool moved = false;
				if (LuaManager::GetNextStep(mX, mY, tgt->mX, tgt->mY, dx, dy)) {
					if (dx != 0 || dy != 0) {
						newX  = mX + dx;
						newY  = mY + dy;
						moved = true;
					}
				}
				// Only count down when NPC can actually move toward target
				if (moved) {
					if (--mChaseRemaining <= 0) {
						mTargetId = -1;
						mChaseRemaining = 0;
					}
				}
			}
		}
	}

	if (mTargetId < 0) {
		short minX = static_cast<short>(max(0,              (int)mSpawnX - 10));
		short maxX = static_cast<short>(min(WORLD_WIDTH-1,  (int)mSpawnX + 10));
		short minY = static_cast<short>(max(0,              (int)mSpawnY - 10));
		short maxY = static_cast<short>(min(WORLD_HEIGHT-1, (int)mSpawnY + 10));

		int dir = dir_dist(rng);
		newX = mX; newY = mY;
		switch (dir) {
		case 0: if (newY > minY) newY--; break;
		case 1: if (newY < maxY) newY++; break;
		case 2: if (newX > minX) newX--; break;
		case 3: if (newX < maxX) newX++; break;
		}
	}

	if (newX < 0) newX = 0;
	if (newX >= WORLD_WIDTH)  newX = WORLD_WIDTH  - 1;
	if (newY < 0) newY = 0;
	if (newY >= WORLD_HEIGHT) newY = WORLD_HEIGHT - 1;

	if (is_in_safe_zone(newX, newY)) return;
	if (!IsWalkable((short)newX, (short)newY)) return;
	if (newX == mX && newY == mY) return;

	mX = newX;
	mY = newY;

	int new_sector_id = get_sector_id(mX, mY);
	if (new_sector_id != mSector_id) {
		std::lock_guard<std::mutex> lk(g_sectors_mutex);
		sectors[mSector_id].erase(mId);
		sectors[new_sector_id].insert(mId);
		mSector_id = new_sector_id;
	}

	S2C_MoveObject mp;
	mp.size = sizeof(S2C_MoveObject);
	mp.type = S2C_MOVE_OBJECT;
	mp.object_id = mId;
	mp.x = mX;
	mp.y = mY;
	mp.move_time = NPC_MOVE_INTERVAL;

	m_visible_mutex.lock();
	auto visible_copy = m_visible_players;
	m_visible_mutex.unlock();

	for (int pid : visible_copy) {
		auto it = clients.find(pid);
		if (it == clients.end()) continue;
		std::shared_ptr<SESSION> pl = it->second;
		if (!pl || pl->mState != CS_PLAYING) continue;

		if (!is_visible(pl->mX, pl->mY)) {
			m_visible_mutex.lock();
			m_visible_players.erase(pid);
			m_visible_mutex.unlock();

			pl->m_visible_mutex.lock();
			pl->m_visible_npcs.erase(mId);
			pl->m_visible_mutex.unlock();

			S2C_RemoveObject rp;
			rp.size = sizeof(S2C_RemoveObject);
			rp.type = S2C_REMOVE_OBJECT;
			rp.object_id = mId;
			pl->doSend(rp.size, reinterpret_cast<char*>(&rp));
		} else {
			pl->doSend(mp.size, reinterpret_cast<char*>(&mp));
		}
	}
}
