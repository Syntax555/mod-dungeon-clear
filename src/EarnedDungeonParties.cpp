/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#include "EarnedDungeonParties.h"
#include "DcEarnedEvents.h"
#include "DcEarnedPlay.h"
#include "DcEarnedProgression.h"
#include "DcModuleEnable.h"
#include "DcStrategyGate.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Config.h"
#include "DBCStores.h"
#include "Event.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "LFGMgr.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <shared_mutex>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Built from the Dungeon Finder, area trigger and area data of the world:
    // the dungeon's own entrance and exit portals, its target level band and,
    // for portals inside a capital, that capital's faction.
    struct Route
    {
        uint32 dungeon, map, continent, entrance, exit;
        TeamId team;
        uint8 expansion, minLevel, maxLevel;
    };
    std::vector<Route> routes;
    constexpr uint32 walkTimeout = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 runTimeout = 90 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 stallTimeout = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 retryDelay = 15 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 refusalMemory = 2 * HOUR * IN_MILLISECONDS;
    constexpr float recruitRange = 60.0f;
    constexpr float startRange = 1000.0f;
    // A bot rejoins a computed corridor within this straight distance; every
    // emitted leg is still raycast before it moves.
    constexpr float rejoinRange = 30.0f;
    // Full outdoor corridor builds run on the world thread. Spread them over
    // coordinator ticks instead of planning every member at once.
    constexpr uint32 buildsPerTick = 2;
    constexpr uint8 ExpansionLevelCap(uint32 expansion)
    {
        return expansion == 0 ? 60 : expansion == 1 ? 70 : expansion == 2 ? 80 : 0;
    }
    bool enabled = false, routesDirty = true;
    std::string configuredDungeons;
    uint32 maxConcurrent = 2, levelSpread = 3, accumulator = 0, recruitAccumulator = 0, buildBudget = 0;
    size_t nextRoute = 0;
    thread_local bool internalStart = false;
    enum class Phase { Entering, Running, Returning };
    // A corridor computed once per party leg. Members share it and rejoin it
    // after combat, looting or other movement instead of planning again.
    struct WalkState
    {
        uint32 map = 0, instance = 0, trigger = 0;
        std::shared_ptr<Movement::PointsArray const> corridor;
        size_t next = 0;
        G3D::Vector3 expected;
        bool Follows(Player* player, uint32 portal) const
        {
            return corridor && map == player->GetMapId() && instance == player->GetInstanceId() && trigger == portal;
        }
    };
    enum class Travel { Moving, Refused, NoPath };
    struct Party
    {
        Route route;
        std::array<ObjectGuid, 5> members;
        ObjectGuid group;
        std::array<std::map<std::string, bool>, 5> strategies;
        std::array<bool, 5> changedNavigation{};
        std::array<bool, 5> invited{};
        std::array<WalkState, 5> walks;
        uint32 started = 0, stalledSince = 0;
        Phase phase = Phase::Entering;
    };
    std::vector<Party> parties;
    std::map<ObjectGuid, uint32> retryAfter;
    // A portal that refused a bot stays closed to it for that dungeon until its
    // level or earned progression changes or the memory expires.
    struct Refusal { uint8 level, stage; uint32 at; };
    std::map<std::pair<ObjectGuid, uint32>, Refusal> refusals;
    constexpr std::array<char const*, 9> navigation{{
        "follow", "new rpg", "rpg", "grind", "travel", "move random", "bg", "lfg", "pvp"
    }};

    // A real player, SelfBot, controlled altbot or other bot master's companion
    // is never recruited. Account membership identifies autonomous random bots;
    // the character's own rewarded progression markers decide its earned era.
    bool Eligible(Player* player, Route const& route, Player* leader = nullptr)
    {
        if (!player || !player->IsInWorld() || !player->GetSession() || !player->IsAlive() || player->IsGameMaster() ||
            (route.team != TEAM_NEUTRAL && player->GetTeamId() != route.team) || player->GetLevel() < route.minLevel ||
            player->GetLevel() > (leader ? ExpansionLevelCap(route.expansion) : route.maxLevel) || player->InBattleground() ||
            player->InBattlegroundQueue() || !player->GetSession()->IsHeadless() ||
            sLFGMgr->GetState(player->GetGUID()) != lfg::LFG_STATE_NONE ||
            !sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId()) ||
            player->IsBeingTeleported() || player->IsInFlight() || player->GetVehicle() || player->GetTransport() ||
            player->GetSession()->PlayerLoading() ||
            DcEarnedProgression::Expansion(player) != route.expansion)
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ai->HasGameClientMaster() ||
            (ai->GetMaster() && ai->GetMaster() != leader))
            return false;
        return true;
    }

    bool Owned(Player* player, Player* leader)
    {
        PlayerbotAI* ai = player ? GET_PLAYERBOT_AI(player) : nullptr;
        return ai && player->GetSession() && player->GetSession()->IsHeadless() &&
               sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId()) &&
               !ai->HasGameClientMaster() && (!ai->GetMaster() || ai->GetMaster() == leader);
    }

    uint8 Role(Player* player, bool bySpec = false)
    {
        if (PlayerbotAI::IsTank(player, bySpec))
            return 0;
        if (PlayerbotAI::IsHeal(player, bySpec))
            return 1;
        return 2;
    }

    void SaveNavigation(Player* member, std::map<std::string, bool>& saved)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
        for (char const* name : navigation)
            saved[name] = ai->HasStrategy(name, BOT_STATE_NON_COMBAT);
    }

    void HoldNavigation(Player* member)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
        for (char const* name : navigation)
            if (ai->HasStrategy(name, BOT_STATE_NON_COMBAT))
                ai->ChangeStrategy(std::string("-") + name, BOT_STATE_NON_COMBAT);
    }

    bool Refused(Player* player, Route const& route)
    {
        auto const it = refusals.find({player->GetGUID(), route.dungeon});
        return it != refusals.end() && it->second.level == player->GetLevel() &&
               it->second.stage == DcEarnedProgression::Stage(player);
    }

    // Ask the portal's checks before recruiting: Individual Progression's map
    // gates, then the core's entry, lockout and access requirements. Teleport
    // hooks are not fired because they act on their player, for example by
    // ending a spectate or sending refusal text. Nothing is granted or relaxed.
    bool CanEnter(Player* player, AreaTriggerTeleport const& target)
    {
        return DcEarnedProgression::CanEnterMap(player, target.target_mapId, target.target_X, target.target_Y,
                                                target.target_Z) &&
               sMapMgr->PlayerCannotEnter(target.target_mapId, player, false) == Map::CAN_ENTER;
    }

    // Only a physically reached portal is submitted. The session handler keeps
    // faction, level, expansion, attunement, progression and instance checks;
    // a refused entry starts no teleport.
    std::optional<bool> NativeDoor(Player* player, AreaTrigger const* at, uint32 trigger)
    {
        if (!player->IsInAreaTriggerRadius(at))
            return std::nullopt;
        WorldPacket packet(CMSG_AREATRIGGER);
        packet << trigger;
        player->GetSession()->HandleAreaTriggerOpcode(packet);
        return player->IsBeingTeleported() || player->GetMapId() != at->map;
    }

    bool BuildCorridor(Player* player, AreaTrigger const* at, uint32 trigger, WalkState& state)
    {
        state = {};
        // Reuse Dungeon Clear's long-range/strided navmesh planner. A zero
        // boss entry bypasses its registered anchors and scripted shortcuts.
        auto const route = ChunkedPathfinder::Build(player, player->GetMapId(), 0, at->x, at->y, at->z, 4);
        if (!route.reachable || route.startFarFromPoly)
            return false;
        Movement::PointsArray corridor;
        for (auto const& segment : route.segments)
        {
            if (segment.anchored || segment.jumpDown || segment.jumpGap || segment.doorGoEntry ||
                segment.polyline.size() > 4096 - corridor.size())
                return false;
            corridor.insert(corridor.end(), segment.polyline.begin(), segment.polyline.end());
        }
        // The module can report arrival within eight yards without a
        // corridor. Finish the last yards with the core's real path; only
        // NativeDoor may decide that the portal was physically reached.
        if (corridor.empty() && route.complete && player->GetExactDist(at->x, at->y, at->z) <= 8.0f)
        {
            PathGenerator path(player);
            if (!path.CalculatePath(at->x, at->y, at->z, false) || !DcEarnedPlay::UsesRealPath(path))
                return false;
            corridor = path.GetPath();
        }
        if (corridor.empty() || corridor.size() > 4096 ||
            std::any_of(corridor.begin(), corridor.end(), [](auto const& p)
            { return !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z); }))
            return false;
        state.map = player->GetMapId();
        state.instance = player->GetInstanceId();
        state.trigger = trigger;
        state.corridor = std::make_shared<Movement::PointsArray const>(std::move(corridor));
        return true;
    }

    // Continue from the nearest point of this member's or a party member's
    // corridor for the same portal. The first emitted leg is raycast like
    // every other one, so a corridor behind a wall is never joined.
    bool Rejoin(Player* player, G3D::Vector3 const& origin, uint32 trigger,
                std::array<WalkState, 5> const& walks, size_t self, WalkState& state)
    {
        for (size_t offset = 0; offset < walks.size(); ++offset)
        {
            WalkState const& source = walks[(self + offset) % walks.size()];
            if (!source.Follows(player, trigger))
                continue;
            auto const& points = *source.corridor;
            size_t best = points.size();
            float bestDistance = rejoinRange * rejoinRange;
            for (size_t i = 0; i < points.size(); ++i)
                if (float const distance = (points[i] - origin).squaredMagnitude(); distance <= bestDistance)
                {
                    best = i;
                    bestDistance = distance;
                }
            if (best == points.size())
                continue;
            WalkState joined = source;
            joined.next = best;
            state = std::move(joined);
            return true;
        }
        return false;
    }

    Travel Walk(Player* player, uint32 trigger, std::array<WalkState, 5>& walks, size_t self)
    {
        WalkState& state = walks[self];
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(trigger);
        if (!at || at->map != player->GetMapId())
            return Travel::NoPath;
        if (std::optional<bool> const entered = NativeDoor(player, at, trigger))
        {
            state = {};
            return *entered ? Travel::Moving : Travel::Refused;
        }
        if (player->IsInCombat() || player->IsNonMeleeSpellCast(true) ||
            !GET_PLAYERBOT_AI(player)->CanMove() || player->isMoving())
            return Travel::Moving;
        G3D::Vector3 const origin(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
            return Travel::NoPath;
        bool const onCorridor = state.Follows(player, trigger) && state.next < state.corridor->size() &&
                                (state.expected - origin).magnitude() <= 0.05f;
        for (bool fresh = false; ; fresh = true)
        {
            if (fresh || (!onCorridor && !Rejoin(player, origin, trigger, walks, self, state)))
            {
                if (!buildBudget)
                {
                    // Wait for a later coordinator tick; the walk timeout
                    // still bounds the attempt.
                    state = {};
                    return Travel::Moving;
                }
                --buildBudget;
                if (!BuildCorridor(player, at, trigger, state))
                    return Travel::NoPath;
                fresh = true;
            }
            // Emit actual corridor points in short windows, never endpoint
            // chords. A partial upstream route advances and is rebuilt at its end.
            auto const& corridor = *state.corridor;
            Movement::PointsArray points{origin};
            size_t next = state.next;
            float distance = 0.0f;
            while (next < corridor.size() && points.size() < 32)
            {
                auto const& point = corridor[next];
                float const leg = (point - points.back()).magnitude();
                if (distance + leg > 80.0f)
                    break;
                ++next;
                if (leg <= 0.05f)
                    continue;
                points.push_back(point);
                distance += leg;
            }
            // Reachability flags also cover no-mmap shortcuts upstream.
            // Independently raycast every emitted leg before raw movement.
            if (DcEarnedPlay::RealSpline(player, points))
            {
                state.next = next;
                state.expected = points.back();
                DcEarnedPlay::MatchSwimming(player, points);
                player->GetMotionMaster()->MoveSplinePath(&points, FORCED_MOVEMENT_NONE);
                return Travel::Moving;
            }
            if (fresh)
                return Travel::NoPath;
        }
    }

    void Stop(Party& party, char const* reason)
    {
        LOG_INFO("playerbots.dungeonclear", "Earned dungeon party {}: {}",
                 party.group.ToString(), reason);
        Player* leader = ObjectAccessor::FindConnectedPlayer(party.members[0]);
        Group* group = party.group.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(party.group.GetCounter());
        bool ownedGroup = group && !group->isRaidGroup();
        bool allOutside = ownedGroup;
        if (ownedGroup)
            for (auto const& slot : group->GetMemberSlots())
            {
                Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
                if (std::find(party.members.begin(), party.members.end(), slot.guid) == party.members.end() ||
                    (member && !Owned(member, leader)))
                    ownedGroup = false;
                if (!member || member->GetMapId() != party.route.continent || member->IsBeingTeleported())
                    allOutside = false;
            }
        // A group claimed by a player or extended with another member is no
        // longer ours. Do not disable its tank or restore another owner's AI.
        if (group && !ownedGroup)
            return;
        if (ownedGroup && leader && leader->GetGroup() == group)
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(leader))
                if (DcRun::Of(ai).enabled)
                    DcActionShared::DisableDungeonClear(ai, reason);
        for (size_t i = 0; i < party.members.size(); ++i)
        {
            retryAfter[party.members[i]] = getMSTime();
            Player* member = ObjectAccessor::FindConnectedPlayer(party.members[i]);
            if (!member || !Owned(member, leader))
                continue;
            // Failed native invitations own no established party. Cancel only
            // the invitation sent by this run's original leader. A cancellation
            // can delete an uncreated group; never retain that pointer.
            if (party.invited[i])
                if (Group* invitation = member->GetGroupInvite())
                    if (invitation->GetLeaderGUID() == party.members[0])
                        member->UninviteFromGroup();
            Group* current = member->GetGroup();
            if (current && current->GetGUID() != party.group)
                continue;
            if (party.changedNavigation[i])
            {
                if (MotionMaster* motion = member->GetMotionMaster())
                    if (motion->GetMotionSlotType(MOTION_SLOT_ACTIVE) == ESCORT_MOTION_TYPE)
                        motion->Clear();
                member->StopMovingOnCurrentPos();
                for (auto const& [name, had] : party.strategies[i])
                    GET_PLAYERBOT_AI(member)->ChangeStrategy(std::string(had ? "+" : "-") + name, BOT_STATE_NON_COMBAT);
            }
            if (allOutside || !current)
            {
                if (GET_PLAYERBOT_AI(member)->GetMaster() == leader)
                    GET_PLAYERBOT_AI(member)->SetMaster(nullptr);
                // Group::RemoveMember can disband/delete the native group.
                if (Group* removable = allOutside ? member->GetGroup() : nullptr)
                    if (removable->GetGUID() == party.group)
                        removable->RemoveMember(member->GetGUID());
            }
        }
        // Inside-instance failures keep their native group. Deliberately
        // disbanding it would start the core's homebind teleport countdown.
        // Death, resurrection and the journey out remain normal gameplay.
    }

    bool TickParty(Party& party)
    {
        Player* leader = ObjectAccessor::FindConnectedPlayer(party.members[0]);
        Group* group = leader ? leader->GetGroup() : nullptr;
        if (!group || group->GetGUID() != party.group || group->isRaidGroup() || group->GetMembersCount() != 5)
        {
            Stop(party, "party changed or member disconnected; autonomous run stopped");
            return false;
        }
        std::array<Player*, 5> members;
        bool allInside = true, allOutside = true;
        std::array<uint8, 3> roles{};
        uint32 instance = 0;
        for (size_t i = 0; i < members.size(); ++i)
        {
            Player* member = ObjectAccessor::FindConnectedPlayer(party.members[i]);
            if (member && member->IsBeingTeleported() && Owned(member, leader) &&
                member->GetGroup() == group && party.phase != Phase::Running)
            {
                members[i] = member;
                ++roles[Role(member)];
                allInside = false;
                allOutside = false;
                continue;
            }
            if (!Eligible(member, party.route, leader) || member->GetGroup() != group)
            {
                Stop(party, "member died, changed control or lost eligibility; no resurrection or relocation granted");
                return false;
            }
            members[i] = member;
            ++roles[Role(member)];
            if (member->GetMapId() == party.route.map)
            {
                if (instance && instance != member->GetInstanceId())
                {
                    Stop(party, "party entered different instance copies; autonomous run stopped");
                    return false;
                }
                instance = member->GetInstanceId();
                allOutside = false;
            }
            else if (member->GetMapId() == party.route.continent)
                allInside = false;
            else
            {
                Stop(party, "member left the supported physical route; autonomous run stopped");
                return false;
            }
        }
        if (roles != std::array<uint8, 3>{1, 1, 3} || Role(leader) != 0)
        {
            Stop(party, "tank/healer/damage composition changed; autonomous run stopped");
            return false;
        }
        if (getMSTimeDiff(party.started, getMSTime()) >
            (party.phase == Phase::Running ? runTimeout : walkTimeout))
        {
            Stop(party, "physical route timed out; no pathfinding or encounter shortcut used");
            return false;
        }
        // Validate the whole party before changing any member's navigation;
        // a later player takeover must not affect the earlier members either.
        for (Player* member : members)
            HoldNavigation(member);
        if (party.phase == Phase::Entering)
        {
            if (allInside)
            {
                for (Player* member : members)
                    DcStrategyGate::Reconcile(member);
                // The TLS authorization can only be entered by this coordinator
                // in the world thread, never by client chat/addon messages.
                internalStart = true;
                bool started = GET_PLAYERBOT_AI(leader)->DoSpecificAction(
                    "dc on", Event("earned dungeon party", "", leader), true);
                internalStart = false;
                if (!started || !DcRun::Of(GET_PLAYERBOT_AI(leader)).enabled)
                {
                    Stop(party, "native dungeon-clear start refused");
                    return false;
                }
                party.phase = Phase::Running;
                party.started = getMSTime();
                LOG_INFO("playerbots.dungeonclear", "Earned dungeon party {} started real map {} instance {}",
                         party.group.ToString(), party.route.map, instance);
            }
            else
                for (size_t i = 0; i < members.size(); ++i)
                {
                    if (members[i]->IsBeingTeleported() || members[i]->GetMapId() != party.route.continent)
                        continue;
                    Travel const travel = Walk(members[i], party.route.entrance, party.walks, i);
                    if (travel == Travel::Refused)
                    {
                        refusals[{members[i]->GetGUID(), party.route.dungeon}] = {
                            members[i]->GetLevel(), DcEarnedProgression::Stage(members[i]), getMSTime()};
                        Stop(party, "the native portal refused a member's entry; autonomous run stopped");
                        return false;
                    }
                    if (travel == Travel::NoPath)
                    {
                        Stop(party, "no real navigation-mesh path to the entrance; autonomous run stopped");
                        return false;
                    }
                }
        }
        else if (party.phase == Phase::Running)
        {
            if (!allInside)
            {
                Stop(party, "member left the running instance; autonomous run stopped");
                return false;
            }
            auto* context = GET_PLAYERBOT_AI(leader)->GetAiObjectContext();
            std::string const& stall = context->GetValue<std::string&>(DcKey::StallReason)->Get();
            if (stall.find("No real navigation-mesh route") == 0)
            {
                Stop(party, "no real navigation-mesh route inside the dungeon; autonomous run stopped");
                return false;
            }
            // Events left to players, closed doors and unreachable objectives
            // stall the clear. Nobody resolves them for an autonomous party.
            if (stall.empty())
                party.stalledSince = 0;
            else if (!party.stalledSince)
                party.stalledSince = getMSTime() | 1;
            else if (getMSTimeDiff(party.stalledSince, getMSTime()) > stallTimeout)
            {
                Stop(party, "dungeon clear stalled on an objective earned parties cannot perform; autonomous run stopped");
                return false;
            }
            if (!DcRun::Of(GET_PLAYERBOT_AI(leader)).enabled)
            {
                party.phase = Phase::Returning;
                party.started = getMSTime();
            }
        }
        else
        {
            if (allOutside)
            {
                Stop(party, "party left through the native exit; normal autonomous play resumed");
                return false;
            }
            for (size_t i = 0; i < members.size(); ++i)
                if (!members[i]->IsBeingTeleported() && members[i]->GetMapId() == party.route.map &&
                    Walk(members[i], party.route.exit, party.walks, i) != Travel::Moving)
                {
                    Stop(party, "no real navigation-mesh path through the exit; autonomous run stopped");
                    return false;
                }
        }
        return true;
    }

    std::vector<uint32> DungeonIds(std::string const& text)
    {
        std::vector<uint32> ids;
        std::stringstream stream(text);
        for (std::string token; std::getline(stream, token, ',');)
        {
            std::erase_if(token, [](unsigned char c) { return std::isspace(c); });
            if (!token.empty() && token.size() < 10 &&
                std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); }))
                ids.push_back(uint32(std::stoul(token)));
        }
        return ids;
    }

    float Distance(float ax, float ay, float az, float bx, float by, float bz)
    {
        return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by) + (az - bz) * (az - bz));
    }

    std::optional<Route> Discover(uint32 id, std::string& reason)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(id);
        MapEntry const* instance = dungeon ? sMapStore.LookupEntry(dungeon->MapID) : nullptr;
        if (!dungeon || !instance)
        {
            reason = "unknown Dungeon Finder id";
            return std::nullopt;
        }
        if (dungeon->TypeID != lfg::LFG_TYPE_DUNGEON || dungeon->ExpansionLevel > 2 ||
            dungeon->Difficulty != DUNGEON_DIFFICULTY_NORMAL || (dungeon->Flags & lfg::LFG_FLAG_SEASONAL) ||
            !instance->IsNonRaidDungeon())
        {
            reason = "not a normal supported-expansion five-player dungeon";
            return std::nullopt;
        }
        if (!DcEarnedEvents::MapAudited(dungeon->MapID, reason))
            return std::nullopt;
        Route route{id, dungeon->MapID, 0, 0, 0, TEAM_NEUTRAL, uint8(dungeon->ExpansionLevel), 0, 0};
        // Wings sharing one map (Scarlet Monastery, Dire Maul) each use the
        // outdoor portal arriving closest to the wing's own starting point.
        lfg::LFGDungeonData const* start = sLFGMgr->GetLFGDungeon(id);
        AreaTriggerTeleport const* arrival = nullptr;
        float best = 0.0f;
        for (auto const& [trigger, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
        {
            AreaTrigger const* portal = sObjectMgr->GetAreaTrigger(trigger);
            MapEntry const* outside = portal ? sMapStore.LookupEntry(portal->map) : nullptr;
            if (teleport.target_mapId != route.map || !outside || !outside->IsContinent())
                continue;
            float const distance = start ? Distance(teleport.target_X, teleport.target_Y, teleport.target_Z,
                                                    start->x, start->y, start->z) : 0.0f;
            if (!arrival || distance < best)
            {
                arrival = &teleport;
                best = distance;
                route.entrance = trigger;
                route.continent = portal->map;
            }
        }
        if (!arrival)
        {
            reason = "no outdoor entrance portal";
            return std::nullopt;
        }
        // Leave through the exit portal nearest to where the party arrived.
        bool exitFound = false;
        for (auto const& [trigger, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
        {
            AreaTrigger const* portal = sObjectMgr->GetAreaTrigger(trigger);
            if (!portal || portal->map != route.map || teleport.target_mapId != route.continent)
                continue;
            float const distance = Distance(portal->x, portal->y, portal->z,
                                            arrival->target_X, arrival->target_Y, arrival->target_Z);
            if (!exitFound || distance < best)
            {
                exitFound = true;
                best = distance;
                route.exit = trigger;
            }
        }
        if (!exitFound)
        {
            reason = "no exit portal to the entrance continent";
            return std::nullopt;
        }
        // A portal inside a capital (Ragefire Chasm, the Stockade) is reached
        // only by that capital's faction.
        AreaTrigger const* entrance = sObjectMgr->GetAreaTrigger(route.entrance);
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(
            sMapMgr->GetZoneId(PHASEMASK_NORMAL, entrance->map, entrance->x, entrance->y, entrance->z));
        if (zone && (zone->flags & AREA_FLAG_CAPITAL))
            route.team = (zone->team & AREATEAM_ALLY) ? TEAM_ALLIANCE :
                         (zone->team & AREATEAM_HORDE) ? TEAM_HORDE : TEAM_NEUTRAL;
        // The dungeon's recommended band, widened by the configured spread and
        // kept within its native entry levels and its expansion's level cap.
        uint32 const low = std::max(dungeon->MinLevel,
                                    dungeon->TargetLevelMin ? dungeon->TargetLevelMin : dungeon->TargetLevel);
        uint32 const target = std::max(dungeon->TargetLevelMax ? dungeon->TargetLevelMax : dungeon->TargetLevel, low);
        uint32 const high = std::min<uint32>({dungeon->MaxLevel, target + levelSpread, ExpansionLevelCap(route.expansion)});
        if (!low || low > high)
        {
            reason = "no level band within the expansion level cap";
            return std::nullopt;
        }
        route.minLevel = uint8(low);
        route.maxLevel = uint8(high);
        return route;
    }

    void BuildRoutes()
    {
        routesDirty = false;
        routes.clear();
        nextRoute = 0;
        for (uint32 id : DungeonIds(configuredDungeons))
        {
            if (std::any_of(routes.begin(), routes.end(), [id](Route const& route) { return route.dungeon == id; }))
                continue;
            std::string reason;
            if (std::optional<Route> route = Discover(id, reason))
            {
                routes.push_back(*route);
                LOG_INFO("playerbots.dungeonclear", "Earned dungeon route {} enabled: map {}, expansion {}, levels {}-{}, portals {}/{}",
                         id, route->map, route->expansion, route->minLevel, route->maxLevel, route->entrance, route->exit);
            }
            else
                LOG_INFO("playerbots.dungeonclear", "Earned dungeon route {} not enabled: {}", id, reason);
        }
    }

    bool Recruit(Route const& route, std::vector<Player*> const& online)
    {
        AreaTrigger const* entrance = sObjectMgr->GetAreaTrigger(route.entrance);
        AreaTrigger const* exit = sObjectMgr->GetAreaTrigger(route.exit);
        AreaTriggerTeleport const* entranceTarget = sObjectMgr->GetAreaTriggerTeleport(route.entrance);
        AreaTriggerTeleport const* exitTarget = sObjectMgr->GetAreaTriggerTeleport(route.exit);
        if (!entrance || !exit || !entranceTarget || !exitTarget ||
            entrance->map != route.continent || exit->map != route.map ||
            entranceTarget->target_mapId != route.map || exitTarget->target_mapId != route.continent)
            return false;
        struct Candidate { Player* player; uint8 role; };
        std::vector<Candidate> candidates;
        std::array<uint32, 3> available{};
        for (Player* player : online)
        {
            if (!Eligible(player, route) || player->GetGroup() || player->GetGroupInvite() ||
                player->IsInCombat() || player->GetMapId() != route.continent)
                continue;
            auto retry = retryAfter.find(player->GetGUID());
            if ((retry != retryAfter.end() && getMSTimeDiff(retry->second, getMSTime()) < retryDelay) ||
                Refused(player, route) || !CanEnter(player, *entranceTarget))
                continue;
            uint8 const role = Role(player, true);
            candidates.push_back({player, role});
            ++available[role];
        }
        if (!available[0] || !available[1] || available[2] < 3)
            return false;
        // Resolve costly native eligibility and role checks once per player;
        // each tank's locality search then uses only cached roles and distance.
        for (Candidate const& option : candidates)
        {
            Player* tank = option.player;
            if (option.role != 0 || tank->GetExactDist(entrance->x, entrance->y, entrance->z) > startRange)
                continue;
            std::array<Player*, 5> picked{{tank, nullptr, nullptr, nullptr, nullptr}};
            size_t damage = 2;
            for (Candidate const& possible : candidates)
            {
                Player* candidate = possible.player;
                if (candidate == tank || candidate->GetMap() != tank->GetMap() || candidate->GetTeamId() != tank->GetTeamId() ||
                    candidate->GetExactDist(tank) > std::min(recruitRange, sPlayerbotAIConfig.SightDistance) ||
                    std::abs(int(candidate->GetLevel()) - int(tank->GetLevel())) > 2)
                    continue;
                uint8 const role = possible.role;
                if (role == 1 && !picked[1])
                    picked[1] = candidate;
                else if (role == 2 && damage < picked.size())
                    picked[damage++] = candidate;
            }
            if (!picked[1] || damage != picked.size())
                continue;
            Party party;
            party.route = route;
            party.started = getMSTime();
            for (size_t i = 0; i < picked.size(); ++i)
            {
                party.members[i] = picked[i]->GetGUID();
                // Acceptance resets strategies in playerbots. Preserve the
                // pre-invitation navigation state rather than those defaults.
                SaveNavigation(picked[i], party.strategies[i]);
            }
            for (size_t i = 1; i < picked.size(); ++i)
            {
                party.invited[0] = true;
                party.invited[i] = true;
                bool accepted = false;
                if (Owned(tank, nullptr) && Owned(picked[i], nullptr) &&
                    !picked[i]->GetGroup() && !picked[i]->GetGroupInvite())
                {
                    WorldPacket invite;
                    invite << picked[i]->GetName() << uint32(0);
                    tank->GetSession()->HandleGroupInviteOpcode(invite);
                    accepted = picked[i]->GetGroupInvite() &&
                               picked[i]->GetGroupInvite()->GetLeaderGUID() == tank->GetGUID() &&
                               GET_PLAYERBOT_AI(picked[i])->DoSpecificAction("accept invitation", Event("earned dungeon party"), true);
                }
                if (!accepted)
                {
                    party.group = tank->GetGroup() ? tank->GetGroup()->GetGUID() : ObjectGuid::Empty;
                    Stop(party, "normal invitation or acceptance refused; no forced grouping used");
                    return false;
                }
                party.changedNavigation[i] = true;
            }
            party.group = tank->GetGroup() ? tank->GetGroup()->GetGUID() : ObjectGuid::Empty;
            if (!tank->GetGroup() || tank->GetGroup()->isRaidGroup() || tank->GetGroup()->GetMembersCount() != 5)
            {
                Stop(party, "native invitations did not establish a complete five-player party");
                return false;
            }
            for (Player* member : picked)
                if (member->GetGroup() != tank->GetGroup() || !Owned(member, tank))
                {
                    Stop(party, "native invitation hooks changed party ownership");
                    return false;
                }
            for (size_t i = 0; i < picked.size(); ++i)
            {
                HoldNavigation(picked[i]);
                party.changedNavigation[i] = true;
            }
            LOG_INFO("playerbots.dungeonclear", "Earned dungeon party {} recruited nearby for map {}; walking to native entrance",
                     party.group.ToString(), route.map);
            parties.push_back(std::move(party));
            return true;
        }
        return false;
    }
}

namespace EarnedDungeonParties
{
    void LoadConfig()
    {
        enabled = sConfigMgr->GetOption<bool>("DungeonClear.AutonomousParties.Enable", false);
        maxConcurrent = std::clamp(sConfigMgr->GetOption<uint32>("DungeonClear.AutonomousParties.MaxConcurrent", 2), 1u, 8u);
        configuredDungeons = sConfigMgr->GetOption<std::string>("DungeonClear.AutonomousParties.Dungeons", "4,12");
        levelSpread = std::min(sConfigMgr->GetOption<uint32>("DungeonClear.AutonomousParties.LevelSpread", 3), 10u);
        // World data is not loaded during startup configuration; resolve the
        // routes on the next world tick. Running parties keep their copy.
        routesDirty = true;
    }
    bool RouteEnabled(uint32 mapId)
    {
        return std::any_of(routes.begin(), routes.end(), [mapId](Route const& route) { return route.map == mapId; });
    }
    bool AuthorizeInternalStart(Player* bot, Event const& event)
    {
        Event& mutableEvent = const_cast<Event&>(event);
        return internalStart && mutableEvent.getOwner() == bot &&
               mutableEvent.GetSource() == "earned dungeon party";
    }
    void Update(uint32 diff)
    {
        accumulator += std::min(diff, 1000u);
        recruitAccumulator += std::min(diff, 1000u);
        if (accumulator < 1000)
            return;
        accumulator = 0;
        if (!enabled || !DcModule::IsEnabled() || !DcEarnedPlay::Active() ||
            !DcEarnedProgression::StrictDefault())
        {
            for (Party& party : parties)
                Stop(party, "autonomous earned parties disabled");
            parties.clear();
            return;
        }
        buildBudget = buildsPerTick;
        std::erase_if(parties, [](Party& party) { return !TickParty(party); });
        if (routesDirty)
            BuildRoutes();
        if (recruitAccumulator < 30000 || parties.size() >= maxConcurrent || routes.empty())
            return;
        recruitAccumulator = 0;
        // The world-thread hook runs outside map updates. Player pointers are
        // only retained for this tick; ongoing parties retain GUIDs instead.
        std::vector<Player*> online;
        {
            std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
            for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                online.push_back(player);
        }
        // Rotate the first route so every enabled dungeon gets recruitment turns.
        for (size_t i = 0; i < routes.size() && parties.size() < maxConcurrent; ++i)
            Recruit(routes[(nextRoute + i) % routes.size()], online);
        nextRoute = (nextRoute + 1) % routes.size();
        uint32 const now = getMSTime();
        std::erase_if(retryAfter, [now](auto const& pair) { return getMSTimeDiff(pair.second, now) >= retryDelay; });
        std::erase_if(refusals, [now](auto const& pair) { return getMSTimeDiff(pair.second.at, now) >= refusalMemory; });
    }
}
