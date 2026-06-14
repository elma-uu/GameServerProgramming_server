#pragma once
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>

constexpr int MAX_PARTY_SIZE = 4;

struct Party {
	int              party_id;
	std::vector<int> member_ids; // first entry is the leader
};

namespace PartyManager {

extern std::mutex              gMutex;
extern std::map<int, Party>    gParties;    // party_id -> Party
extern std::map<int, int>      gPlayerParty; // player_id -> party_id
extern int                     gNextId;

// Returns new party_id, or -1 if player is already in a party
int CreateParty(int leader_id);

// Returns true on success; fails if already in a party or party is full
bool JoinParty(int party_id, int player_id);

// No-op if player is not in a party; disbands the party when last member leaves
void LeaveParty(int player_id);

// Returns party_id of the player, or -1 if not in a party
int GetPlayerParty(int player_id);

// Returns member list for the party (empty if not found)
std::vector<int> GetMembers(int party_id);

// Returns a snapshot of all current parties
std::vector<Party> GetAll();

} // namespace PartyManager
