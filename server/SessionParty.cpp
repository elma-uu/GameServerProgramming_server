#include "pch.h"
#include "SESSION.h"
#include "PartyManager.h"

static void fillPartyUpdatePacket(S2C_PartyUpdate& out, int partyId)
{
	auto members = PartyManager::GetMembers(partyId);
	out.size = sizeof(S2C_PartyUpdate);
	out.type = S2C_PARTY_UPDATE;
	out.party_id = partyId;
	out.member_count = static_cast<unsigned char>(min((int)members.size(), 4));
	for (int i = 0; i < (int)out.member_count; ++i) {
		int mid = members[i];
		out.members[i].player_id = mid;
		auto it = clients.find(mid);
		if (it != clients.end() && it->second) {
			auto& m = it->second;
			strncpy_s(out.members[i].name, m->mUsername, MAX_NAME_LEN - 1);
			out.members[i].name[MAX_NAME_LEN - 1] = '\0';
			out.members[i].hp     = m->mHp;
			out.members[i].max_hp = m->mMaxHp;
			out.members[i].level  = m->mLevel;
		}
	}
}

void broadcastPartyUpdate(int partyId)
{
	S2C_PartyUpdate out{};
	fillPartyUpdatePacket(out, partyId);
	auto members = PartyManager::GetMembers(partyId);
	for (int mid : members) {
		auto it = clients.find(mid);
		if (it == clients.end() || !it->second) continue;
		auto& pl = it->second;
		if (!pl->is_player || pl->mState != CS_PLAYING) continue;
		pl->doSend(out.size, reinterpret_cast<char*>(&out));
	}
}

void SESSION::sendPartyUpdate(int partyId)
{
	S2C_PartyUpdate out{};
	fillPartyUpdatePacket(out, partyId);
	doSend(out.size, reinterpret_cast<char*>(&out));
}

void SESSION::sendPartyList()
{
	auto allParties = PartyManager::GetAll();
	S2C_PartyList out{};
	out.size = sizeof(S2C_PartyList);
	out.type = S2C_PARTY_LIST;
	out.party_count = static_cast<unsigned char>(min((int)allParties.size(), 8));
	for (int i = 0; i < (int)out.party_count; ++i) {
		auto& party = allParties[i];
		out.entries[i].party_id     = party.party_id;
		out.entries[i].member_count = static_cast<unsigned char>(party.member_ids.size());
		if (!party.member_ids.empty()) {
			int leaderId = party.member_ids[0];
			auto it = clients.find(leaderId);
			if (it != clients.end() && it->second) {
				strncpy_s(out.entries[i].leader_name, it->second->mUsername, MAX_NAME_LEN - 1);
				out.entries[i].leader_name[MAX_NAME_LEN - 1] = '\0';
			}
		}
	}
	doSend(out.size, reinterpret_cast<char*>(&out));
}

void SESSION::givePartyExp(unsigned long long kill_exp)
{
	unsigned long long bonus = kill_exp / 2;
	if (bonus == 0 || mPartyId < 0) return;

	auto members = PartyManager::GetMembers(mPartyId);
	for (int memberId : members) {
		if (memberId == mId) continue;
		auto it = clients.find(memberId);
		if (it == clients.end() || !it->second) continue;
		auto& member = it->second;
		if (!member->is_player || member->mState != CS_PLAYING) continue;

		member->mExp += bonus;
		while (member->mLevel < 100) {
			unsigned long long req = static_cast<unsigned long long>(member->mLevel) * member->mLevel * 20ULL;
			if (member->mExp < req) break;
			member->mExp -= req;
			member->mLevel++;
			member->mStatPoints += 5;
		}
		member->sendAvatarInfo();
	}
}
