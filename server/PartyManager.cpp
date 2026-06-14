#include "pch.h"
#include "PartyManager.h"

namespace PartyManager {

std::mutex           gMutex;
std::map<int, Party> gParties;
std::map<int, int>   gPlayerParty;
int                  gNextId = 0;

int CreateParty(int leader_id)
{
	std::lock_guard<std::mutex> lock(gMutex);
	if (gPlayerParty.count(leader_id)) return -1; // already in party
	int id = ++gNextId;
	Party p;
	p.party_id = id;
	p.member_ids.push_back(leader_id);
	gParties[id] = std::move(p);
	gPlayerParty[leader_id] = id;
	return id;
}

bool JoinParty(int party_id, int player_id)
{
	std::lock_guard<std::mutex> lock(gMutex);
	if (gPlayerParty.count(player_id)) return false; // already in a party
	auto it = gParties.find(party_id);
	if (it == gParties.end()) return false;
	if ((int)it->second.member_ids.size() >= MAX_PARTY_SIZE) return false;
	it->second.member_ids.push_back(player_id);
	gPlayerParty[player_id] = party_id;
	return true;
}

void LeaveParty(int player_id)
{
	std::lock_guard<std::mutex> lock(gMutex);
	auto pit = gPlayerParty.find(player_id);
	if (pit == gPlayerParty.end()) return;
	int party_id = pit->second;
	gPlayerParty.erase(pit);
	auto it = gParties.find(party_id);
	if (it == gParties.end()) return;
	auto& ids = it->second.member_ids;
	ids.erase(std::remove(ids.begin(), ids.end(), player_id), ids.end());
	if (ids.empty()) gParties.erase(it); // disband empty party
}

int GetPlayerParty(int player_id)
{
	std::lock_guard<std::mutex> lock(gMutex);
	auto it = gPlayerParty.find(player_id);
	return (it != gPlayerParty.end()) ? it->second : -1;
}

std::vector<int> GetMembers(int party_id)
{
	std::lock_guard<std::mutex> lock(gMutex);
	auto it = gParties.find(party_id);
	if (it == gParties.end()) return {};
	return it->second.member_ids;
}

std::vector<Party> GetAll()
{
	std::lock_guard<std::mutex> lock(gMutex);
	std::vector<Party> result;
	result.reserve(gParties.size());
	for (auto& [id, p] : gParties) result.push_back(p);
	return result;
}

} // namespace PartyManager
