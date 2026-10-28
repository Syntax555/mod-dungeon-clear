/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcEarnedPlay.h"
#include "DcRingOfLaw.h"
#include "ObjectiveHookRegistry.h"

#include <atomic>
#include <optional>
#include <utility>

#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "ServerFacade.h"
#include "Player.h"
#include "Spell.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "PlayerbotAI.h"

#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFormGate.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerbotCompat.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRaidMuster.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

namespace
{
    ObjectiveArriveResult EnsureRingStarted(Player* bot, AiObjectContext* /*context*/,
                                            DungeonBossInfo const& /*info*/)
    {
        return DcRingOfLaw::Run(bot);
    }

    // The native SmartTrigger targets this spawn in the player's own instance.
    constexpr uint32 ZF_ZUMRAH = 7271;
    constexpr uint32 ZF_ZUMRAH_SPAWN = 81524;
    constexpr uint32 ZF_ZUMRAH_TRIGGER = 962;
    constexpr uint32 ZF_ZUMRAH_FACTION_FRIENDLY = 35;
    constexpr float ZF_ZUMRAH_SCAN = 60.0f;

    ObjectiveArriveResult WakeZumrah(Player* bot, AiObjectContext* context,
                                     DungeonBossInfo const& /*info*/)
    {
        if (!bot || bot->GetMapId() != 209 || !bot->IsInWorld() ||
            !GET_PLAYERBOT_AI(bot) || !bot->GetSession() || IsSelfBot(bot))
            return ObjectiveArriveResult::Blocked;
        if (!context || !DcRun::Of(context).enabled || DcRun::Of(context).paused)
            return ObjectiveArriveResult::Running;

        Creature* zumrah = bot->FindNearestCreature(ZF_ZUMRAH, ZF_ZUMRAH_SCAN, true);
        if (!zumrah || zumrah->GetSpawnId() != ZF_ZUMRAH_SPAWN)
            return ObjectiveArriveResult::Running;
        if (zumrah->IsHostileTo(bot))
            return ObjectiveArriveResult::Done;
        if (zumrah->GetFaction() != ZF_ZUMRAH_FACTION_FRIENDLY)
            return ObjectiveArriveResult::Blocked;

        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(ZF_ZUMRAH_TRIGGER);
        if (!trigger || trigger->map != 209)
            return ObjectiveArriveResult::Blocked;
        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
            bot->IsBeingTeleported() || bot->m_mover != bot ||
            bot->IsNonMeleeSpellCast(false) || bot->GetLootGUID())
            return ObjectiveArriveResult::Running;

        if (!bot->IsInAreaTriggerRadius(trigger))
        {
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            if (!bot->isMoving() && ai->CanMove() &&
                DcEarnedPlay::ReachableLeg(bot, trigger->x, trigger->y, trigger->z))
                bot->GetMotionMaster()->MovePoint(0, trigger->x, trigger->y, trigger->z,
                                                 FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                 /*generatePath*/ true, false);
            return ObjectiveArriveResult::Running;
        }

        DcMovement::StopBot(bot, DcMovement::Stop::Hold);
        // Same non-teleport path as ReachAreaTriggerAction. The core checks
        // arrival and owns SmartAI, conditions and faction changes, also on retry.
        WorldPacket packet(CMSG_AREATRIGGER);
        packet << uint32(ZF_ZUMRAH_TRIGGER);
        bot->GetSession()->HandleAreaTriggerOpcode(packet);
        return ObjectiveArriveResult::Running;
    }

    // --- The Arcatraz: DriveMellicharWaves (hook id 6) --------------------
    // Warden Mellichar's stasis-pod finale is started by DAMAGING him — there is
    // no gossip, no area trigger and no clickable object anywhere in the
    // encounter (arcatraz.cpp, npc_warden_mellicharAI::DamageTaken). He is
    // SetImmuneToAll and zeroes all incoming damage, so the hit costs him
    // nothing; it exists purely as the signal that schedules the intro EventMap.
    // He never enters combat and never loses HP, so a KillCreatureEngage step
    // aimed at him would run forever — hence a Custom step.
    //
    // DO NOT "simplify" THIS TO SetBossState(DATA_WARDEN_MELLICHAR, IN_PROGRESS).
    // The intro events are scheduled inside DamageTaken, NOT inside SetBossState,
    // so setting the state directly starts nothing — AND it permanently locks the
    // encounter out, because DamageTaken's own guard is
    // `GetBossState(...) != IN_PROGRESS`. That is a one-way trap: the run can
    // never be recovered without a full instance reset.
    //
    // THIS HOOK OWNS THE WHOLE WAVE PHASE (poke + garrison), rather than poking
    // once and handing off to a MoveToHoldUntilSpawn step, SPECIFICALLY so a wipe
    // recovers. The event must be .Persistent() (it holds a KillCreatureEngage),
    // and DungeonEventExecutor::Drive only rewinds stepIndex for NON-persistent
    // events — so a poke-once step that has already returned Done can never fire
    // again. A wipe mid-waves puts Mellichar's own Reset() back to NOT_STARTED
    // and despawns every summon, and a separate hold step would then sit waiting
    // on a Skyriss that nobody will ever release, burning the full step timeout.
    // Keeping the poke live for the entire phase makes that self-healing: the
    // party corpse-runs back, this sees NOT_STARTED again, and re-pokes.
    constexpr uint32 ARC_MELLICHAR = 20904;
    constexpr uint32 ARC_SKYRISS = 20912;
    constexpr uint32 ARC_DATA_WARDEN_MELLICHAR = 3;  // arcatraz.h DATA_WARDEN_MELLICHAR
    constexpr uint32 ARC_NOT_STARTED = 0;            // EncounterState::NOT_STARTED
    constexpr uint32 ARC_DONE = 3;                   // EncounterState::DONE
    // He is a fixed spawn on the dais ~8yd from the arena centroid the event
    // parks on; 60 covers the whole pod room without reaching outside it.
    constexpr float ARC_MELLICHAR_SCAN = 60.0f;
    // Skyriss is a TempSummon, so only a grid scan finds him; he is released
    // ~30yd from the centroid.
    constexpr float ARC_SKYRISS_SCAN = 100.0f;
    // The arena floor centroid, mirrored from ArcatrazEvents.cpp. Re-centring
    // matters because the encounter evades outright if no player is within 100yd
    // of Mellichar (EVENT_WARDEN_CHECK_PLAYERS, every 1s).
    constexpr float ARC_ARENA_X = 445.9f;
    constexpr float ARC_ARENA_Y = -161.5f;
    constexpr float ARC_ARENA_Z = 42.56f;
    constexpr float ARC_ARENA_LEASH = 15.0f;

    ObjectiveArriveResult DriveMellicharWaves(Player* bot, AiObjectContext* /*context*/,
                                              DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // Skyriss is on the field — hand off to the kill step.
        if (bot->FindNearestCreature(ARC_SKYRISS, ARC_SKYRISS_SCAN, /*alive*/ true))
            return ObjectiveArriveResult::Done;

        // Already won (a resumed run, or the human finished it) — don't restart it.
        if (inst->GetBossState(ARC_DATA_WARDEN_MELLICHAR) == ARC_DONE)
            return ObjectiveArriveResult::Done;

        if (inst->GetBossState(ARC_DATA_WARDEN_MELLICHAR) == ARC_NOT_STARTED)
        {
            Creature* mellichar = bot->FindNearestCreature(ARC_MELLICHAR, ARC_MELLICHAR_SCAN);
            if (mellichar && mellichar->IsAlive())
            {
                // Any nonzero player-sourced damage will do; DamageTaken checks
                // only `attacker->GetCharmerOrOwnerOrOwnGUID().IsPlayer() &&
                // damage > 0`. Unit::DealDamage hands the AI its DamageTaken
                // callback BEFORE any HP arithmetic, and Mellichar's override
                // zeroes the value, so this cannot overkill him or otherwise
                // perturb the script.
                Unit::DealDamage(bot, mellichar, 1);
                // Throttled: if the poke ever fails to take (another module's
                // DealDamage hook zeroing damage ahead of the script, say) this
                // retries every tick, and an unthrottled INFO would bury the log.
                static uint32 lastPokeLog = 0;
                uint32 const now = getMSTime();
                if (!lastPokeLog || GetMSTimeDiffToNow(lastPokeLog) > 10000)
                {
                    lastPokeLog = now;
                    LOG_INFO("playerbots.dungeonclear",
                             "DungeonClear: Arcatraz — poking Warden Mellichar ({}) to open the "
                             "stasis pods for {} (the encounter has no trigger but player damage)",
                             ARC_MELLICHAR, bot->GetName());
                }
            }
        }

        // Garrison the centroid between waves. The executor's own MoveTo
        // re-centring is unavailable here (this is one Custom step, deliberately
        // — see above), so mirror it: only nudge when actually displaced and not
        // already moving, so this never fights the combat AI mid-wave.
        if (!bot->isMoving() &&
            bot->GetExactDist(ARC_ARENA_X, ARC_ARENA_Y, ARC_ARENA_Z) > ARC_ARENA_LEASH)
        {
            bot->GetMotionMaster()->MovePoint(0, ARC_ARENA_X, ARC_ARENA_Y, ARC_ARENA_Z,
                                              FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                              /*generatePath*/ true, false);
        }

        return ObjectiveArriveResult::Running;
    }

    // --- Sethekk Halls: DriveAnzuSummon (hook id 7) -----------------------
    // Anzu is normally summoned by a DRUID using item 32449 (which sends event
    // 14797 to the instance). instance_sethekk_halls::ProcessEvent has NO
    // IsHeroic()/difficulty gate — "heroic-only" is enforced purely by the
    // item/quest being heroic-gated — so we replicate the send-event directly (no
    // druid / item / quest / heroic key). The blizzlike HEROIC-only restriction is
    // reinstated one level up: the Anzu event carries .HeroicOnly(), so this hook
    // is only ever driven on a heroic run. On normal the event never fires.
    // The instance guard self-de-dupes (no Voice && Anzu != DONE), but we also
    // gate our own poke on "no Anzu yet" so we never spawn a SECOND Voice/Anzu
    // once he is on the field. ~40s of theatrics then a ~16s intro precede a
    // fightable Anzu; the Custom step's 120s Timeout covers it, and Anzu's own
    // SetInCombatWithZone() (post-intro) pulls the party into the fight.
    constexpr uint32 SH_ANZU_SEND_EVENT = 14797;
    constexpr uint32 SH_ANZU_NPC        = 23035;  // TempSummon boss
    constexpr uint32 SH_VOICE_NPC       = 21851;  // TempSummon theatrics NPC
    constexpr uint32 SH_DATA_ANZU       = 1;      // sethekk_halls.h DATA_ANZU
    constexpr float  SH_ANZU_SCAN       = 60.0f;

    ObjectiveArriveResult DriveAnzuSummon(Player* bot, AiObjectContext* /*context*/,
                                          DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // Already killed (a resumed run, or the human did it) — don't re-summon.
        if (inst->GetBossState(SH_DATA_ANZU) == DONE)
            return ObjectiveArriveResult::Done;

        // Anzu is on the field (even mid-intro, still NON_ATTACKABLE) — hand off
        // to the KillCreatureEngage step and STOP poking so no second Voice fires.
        if (bot->FindNearestCreature(SH_ANZU_NPC, SH_ANZU_SCAN, /*alive*/ true))
            return ObjectiveArriveResult::Done;

        // Theatrics in progress — the Voice is alive running action list 2185100
        // toward the ~40s summon. Hold without re-poking.
        if (bot->FindNearestCreature(SH_VOICE_NPC, SH_ANZU_SCAN, /*alive*/ true))
            return ObjectiveArriveResult::Running;

        // No Voice, no Anzu, not DONE -> fire the send-event. Idempotent (the
        // instance guard no-ops if a Voice somehow already exists).
        inst->ProcessEvent(bot, SH_ANZU_SEND_EVENT);
        return ObjectiveArriveResult::Running;
    }

    // --- The Shattered Halls: StartNethekurseIntro (hook id 9) ------------
    // Grand Warlock Nethekurse (16807) spawns SetImmuneToAll; the ONLY things
    // that clear it are ACTION_START_INTRO from area trigger 4347
    // (at_rp_nethekurse — CMSG_AREATRIGGER, which a bot never sends; same class
    // as the Ring of Law / Zum'rah bugs) or his own door-watch seeing chamber
    // door 182539 lockpicked open. The DC route teleports past that door's
    // navmesh break without touching it, so a full-bot run walks into a
    // permanently immune, passive first boss and the engage loop deadlocks.
    //
    // Driven by the Conditional "Wake Grand Warlock Nethekurse" event
    // (ShatteredHallsEvents, due only while he is alive, near, and still
    // IsImmuneToPC). Fire exactly what the area-trigger script fires:
    // DoAction(ACTION_START_INTRO) on his AI — it drops SetImmuneToAll on him
    // and his four peons and starts the peon-kill RP, and it carries its own
    // `_introStarted` guard so a duplicate fire (a lagging human's real
    // trigger, say) is a no-op. From there the normal flow takes over: the
    // tank engages him (JustEngagedWith cancels the RP), or he finishes the
    // peons and AttackStarts the nearest player himself.
    //
    // Logged at INFO for the same reason WakeZumrah is: this is the one place
    // a Shattered Halls first-boss deadlock gets fixed silently, and under the
    // test harness a bot-side failure is otherwise invisible.
    constexpr uint32 SHH_NETHEKURSE = 16807;
    constexpr int32 SHH_ACTION_START_INTRO = 0;  // boss_nethekurse.cpp ACTION_START_INTRO
    // Mirrors the event predicate's scan: the party is at/near him whenever the
    // event is due, so this always resolves the same creature the gate read.
    constexpr float SHH_NETHEKURSE_SCAN = 60.0f;

    ObjectiveArriveResult StartNethekurseIntro(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        Creature* nethekurse = bot->FindNearestCreature(SHH_NETHEKURSE, SHH_NETHEKURSE_SCAN);
        if (!nethekurse || !nethekurse->IsAlive())
            return ObjectiveArriveResult::Done;  // gone/dead — nothing to wake

        // Already woken (this fired, or a human's client tripped AT 4347).
        if (!nethekurse->IsImmuneToPC())
            return ObjectiveArriveResult::Done;

        nethekurse->AI()->DoAction(SHH_ACTION_START_INTRO);
        LOG_INFO("playerbots.dungeonclear",
                 "DungeonClear: Shattered Halls — started Nethekurse's ({}) intro for {} "
                 "(area trigger 4347 never fires for an all-bot party, so he stays immune)",
                 SHH_NETHEKURSE, bot->GetName());
        // Done even if the flip somehow failed to take: the Repeatable event's
        // predicate still reads immune next tick and re-fires this hook.
        return ObjectiveArriveResult::Done;
    }

    // --- The Underbog: SendGhazanToPlatform (hook id 10) ------------------
    // Ghaz'an (18105) spawns in the lake and swims DB waypoint path 1383920 —
    // every node between z 32 and z 47, i.e. underwater. The ONLY thing that
    // moves him out is at_underbog_ghazan (areatrigger 4302), whose
    // ACTION_MOVE_TO_PLATFORM starts path 1383921 up the ramp onto his platform
    // at (256.28, -458.73, 81.37). Same class as the Ring of Law / Zum'rah /
    // Nethekurse bugs: a bot never sends CMSG_AREATRIGGER on its own.
    //
    // DcTestAreaTriggers relays that packet now, so this is the BACKSTOP, not
    // the primary path — 4302's radius is only 10yd and map 546 has no
    // hand-authored route hints, so a party can path around the volume. If it
    // does, the boss anchor points at an empty platform and the run stalls.
    //
    // Fire the same DoAction the areatrigger script fires. It carries its own
    // `_movedToPlatform` guard, so when the relay already worked this is a
    // no-op — and the event's predicate has usually already read him as up and
    // never called us at all. Driven by the Conditional + Repeatable "Send
    // Ghaz'an up to his platform" event (UnderbogEvents), whose predicate is the
    // deadlock signature: alive and still below the ramp.
    //
    // Logged at INFO for the same reason WakeZumrah is: this is the one place an
    // Underbog second-boss stall gets fixed silently, and it also tells us
    // whether the relay is doing its job — a run that logs this fired the
    // backstop, which means the route missed AT 4302.
    constexpr uint32 UB_GHAZAN = 18105;
    constexpr int32 UB_ACTION_MOVE_TO_PLATFORM = 1;  // boss_ghazan.cpp
    // Matches the event predicate's band: his patrol tops out at z 46.8 and the
    // ramp's first dry node is z 74.7, so the gap is unambiguous.
    constexpr float UB_GHAZAN_WATER_Z = 60.0f;

    ObjectiveArriveResult SendGhazanToPlatform(Player* bot, AiObjectContext* context,
                                               DungeonBossInfo const& /*info*/)
    {
        // Map-wide and already resolved this tick by the event's own predicate —
        // see UbGhazanStillInTheLake for why this is not a radius scan.
        Creature* ghazan = DcTargeting::GetLiveBoss(bot, context, UB_GHAZAN);
        if (!ghazan || !ghazan->IsAlive())
            return ObjectiveArriveResult::Done;  // gone/dead — nothing to send

        // Already climbing or up (the relay fired, or a human's client tripped
        // AT 4302). Height, not a flag: the AI's own _movedToPlatform is private
        // and his z is the thing we actually care about.
        if (ghazan->GetPositionZ() >= UB_GHAZAN_WATER_Z)
            return ObjectiveArriveResult::Done;

        ghazan->AI()->DoAction(UB_ACTION_MOVE_TO_PLATFORM);
        LOG_INFO("playerbots.dungeonclear",
                 "DungeonClear: The Underbog — sent Ghaz'an ({}) up to his platform for {} "
                 "(the route missed area trigger 4302, so he was still in the lake at z {:.1f})",
                 UB_GHAZAN, bot->GetName(), ghazan->GetPositionZ());
        // Done even if the action did not take: the Repeatable event's predicate
        // still reads him below the ramp next tick and re-fires this hook.
        return ObjectiveArriveResult::Done;
    }

    // --- Azjol-Nerub: HadronoxHasWebbedTheDoors (hook id 13) --------------
    // A WAIT, not an action — the one hook here that does nothing to the world.
    //
    // Hadronox's add swarm is infinite: three world triggers (23472) summoned in
    // her Reset() carry periodic summon auras that keep firing for as long as
    // the encounter is not done, and every add she kills while it carries her
    // Leech Poison heals her 10% of max HP. The only off-switch is
    // EVENT_HADRONOX_MOVE3 — gated on every Anub'ar Crusher (28922) being dead,
    // scheduled 70s after the pull, and re-checked every 2s — which walks her up
    // to (530.4, 560.0, 733.2) and casts Web Front Doors (53177) on arrival.
    //
    // Killing the crushers therefore only makes the web ELIGIBLE. The clear has
    // to keep the party on the platform until it has actually happened, or boss
    // navigation takes the tank down to meet her mid-climb while the swarm is
    // still pouring in.
    //
    // `_doorsWebbed` is private and has exactly one exposure: boss_hadronox::
    // GetData(me->GetEntry()), the 'Hadronox Denied' achievement probe, which
    // returns 0 once the doors are webbed and 1 while they are open. Read it
    // with her OWN entry — any other id falls through to the same `return 0`
    // and would read as "webbed" from the first tick.
    constexpr uint32 AN_HADRONOX = 28921;

    ObjectiveArriveResult HadronoxHasWebbedTheDoors(Player* bot, AiObjectContext* context,
                                                    DungeonBossInfo const& /*info*/)
    {
        // Map-wide, like the Underbog's Ghaz'an probe: she spends the whole
        // event walking a 120yd lap between z 695 and z 733 and a radius scan
        // would lose her halfway through it.
        Creature* hadronox = DcTargeting::GetLiveBoss(bot, context, AN_HADRONOX);
        if (!hadronox || !hadronox->IsAlive() || !hadronox->AI())
            return ObjectiveArriveResult::Done;  // dead/gone — nothing left to wait for

        return hadronox->AI()->GetData(AN_HADRONOX) == 0 ? ObjectiveArriveResult::Done
                                                         : ObjectiveArriveResult::Running;
    }

    // --- Drak'Tharon Keep: HoldNovosCamp (hook id 14) ---------------------
    // The whole of Novos' phase 1, and the ONLY step of map 600's conditional
    // event 1. Like the Black Morass wave driver it re-decides from live world
    // state every tick rather than walking a step list, because what the
    // encounter needs is not a sequence but a standing position preference held
    // under continuous fire. The dungeon-side reasoning — why a camp, why THIS
    // camp, what the gate is — lives in DrakTharonKeepEvents.cpp; this is the
    // mechanism.
    //
    // THREE THINGS CAN GO WRONG POSITIONALLY, and the hook is one test per:
    //
    //  1. THE ARCANE FIELD. 47346 is a PERSISTENT_AREA_AURA at Novos' feet,
    //     11.0yd, 1665 damage per second, for the whole of phase 1. The tank
    //     lands the pull IN it and the corpse stream keeps giving it reasons to
    //     drift back north. The DcHazardRegistry row and its vacate rung already
    //     push every bot out; this pushes the LEADER specifically, on the tick
    //     the driver owns, so the two agree rather than take turns.
    //  2. THE STAIRCASE. Every Risen Shadowcaster and Hulking Corpse of the phase
    //     is left standing, passive but hostile, at the spawn trigger 45yd up the
    //     stairs (56yd from the camp). A clear that wanders down there is off the
    //     80yd leash and out of the fight.
    //  3. ORDINARY DRIFT. The corpses arrive 15yd south of the camp and the
    //     handlers walk in from the room's corners, so a tank that simply chases
    //     its current victim ends the phase somewhere else entirely.
    //
    // AND ONE THING MUST NOT GO WRONG: the tank must still FIGHT. This hook runs
    // above the stock combat movers (DcRel::EventDueCombat), and Engine::
    // DoNextAction runs exactly ONE action per tick, so a driver that returns
    // Running every tick starves the rotation outright — no target, no swing, no
    // threat. That is how Black Morass wiped parties. Hence: Done is the DEFAULT
    // answer here, and Done means "nothing to steer, take your tick back"
    // (DungeonEvent::stepsOwnMovement makes DcRunEventAction yield on it).
    //
    // The melee-contact exemption is the same idea one step down. Between the
    // 6yd leash and the 25yd hard leash the tank is left alone WHILE it is
    // actually swinging at something, so the driver never yanks it off a corpse
    // it is holding at the stairs' foot; the exemption stops the moment the
    // victim dies or walks away.
    //
    // NO isMoving() GUARD, deliberately — the trap the Black Morass driver
    // documents. In combat the bot is essentially always moving under MoveChase,
    // so `if (isMoving()) return;` would make this a no-op for the entire
    // encounter. The re-issue floor below is what keeps re-taking the tick from
    // becoming spline spam instead.
    //
    // The camp, the scan radius, the Arcane Field keep-out and the two leashes
    // are all DcDrakTharonKeep (Data/Events/DungeonEventTables.h) — shared with
    // the event that drives this hook, and with t/TestDcHazard, which pins the
    // camp against the 47346 row's own keep-out cylinder. One definition each.
    //
    // Same-destination re-issue floor. The camp is ONE fixed point, so unlike the
    // Black Morass there is no "which portal" question to answer — this exists
    // purely so a MoveChase that stomps our point-move is recovered from promptly
    // without re-pathing every tick.
    constexpr uint32 DTK_REISSUE_MS = 1000;

    // Hook 40 — see DC_HOOK_RAID_MUSTER.
    ObjectiveArriveResult RaidMusterAtObjective(Player* bot, AiObjectContext* context,
                                                DungeonBossInfo const& /*info*/)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return ObjectiveArriveResult::Done;
        std::optional<DungeonBossInfo> const next =
            context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
        if (!next)
            return ObjectiveArriveResult::Done;
        // The muster is keyed on the anchor's entry and is Boss-only; present the
        // objective as the boss it is about to start.
        DungeonBossInfo asBoss = *next;
        asBoss.kind = DungeonAnchorKind::Boss;
        return DcRaidMuster::Holds(bot, botAI, context, asBoss) ? ObjectiveArriveResult::Running
                                                                : ObjectiveArriveResult::Done;
    }

    ObjectiveArriveResult HoldNovosCamp(Player* bot, AiObjectContext* context,
                                        DungeonBossInfo const& /*info*/)
    {
        Creature* novos = bot->FindNearestCreature(DcDrakTharonKeep::NOVOS,
                                                   DcDrakTharonKeep::NOVOS_SCAN, /*alive*/ true);
        if (!novos)
            return ObjectiveArriveResult::Done;  // dead / out of the room — nothing to hold for

        // The encounter's own phase bit: set by JustEngagedWith, cleared in the
        // same statement that starts phase 2. Once it is gone the pool is gone
        // with it (47346 is channeled and the phase flip interrupts it), so melee
        // must be free to close on him.
        if (!novos->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            return ObjectiveArriveResult::Done;

        float const toCamp = bot->GetExactDist2d(DcDrakTharonKeep::CAMP_X, DcDrakTharonKeep::CAMP_Y);
        bool const inField = bot->GetExactDist2d(novos) < DcDrakTharonKeep::FIELD_KEEPOUT;

        bool inMelee = false;
        if (Unit* victim = bot->GetVictim())
            inMelee = victim->IsAlive() && bot->IsWithinMeleeRange(victim);

        if (!inField && toCamp <= DcDrakTharonKeep::CAMP_HARD_LEASH &&
            (toCamp <= DcDrakTharonKeep::CAMP_LEASH || inMelee))
            return ObjectiveArriveResult::Done;  // in position — hand the tick back

        // Throttled re-issue. A point move inside one chamber is at most ~56yd
        // (the stairs), well inside what MovePoint's PathGenerator delivers, so
        // there is no long-haul spline branch to take here.
        // On the bot's own run state — see Util/DcThrottle.h for why not in a
        // file-scope thread_local map keyed by GUID.
        if (DcRun::Of(context).Throttled(DcThrottle::DtkCampIssue, DTK_REISSUE_MS))
            return ObjectiveArriveResult::Running;  // still walking home

        // Drop any escort glide first: a coast-past from the advance ladder is
        // aimed somewhere else entirely and would ride out over this point move.
        DcMovement::ResolveEscortConflict(bot);
        bot->GetMotionMaster()->MovePoint(0, DcDrakTharonKeep::CAMP_X,
                                          DcDrakTharonKeep::CAMP_Y, DcDrakTharonKeep::CAMP_Z,
                                          FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          /*generatePath*/ true, false);

        static uint32 lastHoldLog = 0;
        if (!lastHoldLog || GetMSTimeDiffToNow(lastHoldLog) > 10000)
        {
            lastHoldLog = getMSTime();
            LOG_DEBUG("playerbots.dungeonclear",
                      "DungeonClear: Drak'Tharon Keep — walking {} back to the Novos camp "
                      "({:.1f}yd out, {:.1f}yd from Novos{})",
                      bot->GetName(), toCamp, bot->GetExactDist2d(novos),
                      inField ? ", INSIDE the Arcane Field" : "");
        }

        return ObjectiveArriveResult::Running;
    }

    // --- Old Hillsbrad: GrantIncendiaryBombs (hook id 3) ------------------
    // Brazen (18725) only offers his drake ride to Durnholde Keep when the player
    // HOLDS the Pack of Incendiary Bombs (item 25853) — gossip menu 7959 option 0
    // is condition-gated on that item. A player gets the pack by talking to Erozion
    // (18723), but Erozion's grant option is quest-gated (quest 10283 "Taretha's
    // Diversion" taken/rewarded), which a bot never has — so the bot can never
    // select Erozion's gossip. Grant the pack directly (mechanically identical to
    // Erozion's AddItem) so Brazen's ride option then appears. Idempotent: Done
    // once the tank holds it. (The same pack is used to bomb the barrels in
    // objective 2 — the UseItemOnGO step also self-grants as a backstop.)
    constexpr uint32 OH_ITEM_INCENDIARY_BOMBS = 25853;

    ObjectiveArriveResult GrantIncendiaryBombs(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        if (bot->GetItemByEntry(OH_ITEM_INCENDIARY_BOMBS))
            return ObjectiveArriveResult::Done;
        bot->AddItem(OH_ITEM_INCENDIARY_BOMBS, 1);
        return bot->GetItemByEntry(OH_ITEM_INCENDIARY_BOMBS)
                   ? ObjectiveArriveResult::Done
                   : ObjectiveArriveResult::Running;  // bags full this tick — retry
    }

    // --- The Mechanar: GrantCacheKeyAndLoot (hook id 4) -------------------
    // The Cache of the Legion (GO 184465 normal / 184849 heroic) is a locked chest
    // (Lock.dbc 1706, a LOCK_KEY_ITEM lock) requiring the Cache of the Legion Key
    // (item 30438). Blizzard's key is formed by combining the two Jagged Crystals
    // the Gatewatchers drop (Gyro-Kill -> Blue 30436, Iron-Hand -> Red 30437) via
    // the crystals' on-use spell 36565 — but a bot never right-clicks a crystal,
    // and in a party the two crystals can land in different bags. Per the design
    // decision (grant the key), hand the leader key 30438 directly, then OPEN the
    // chest exactly the way the Deadmines cannon / Old Hillsbrad barrels do it:
    // USE the key item on the GO (CastItemUseSpell). This is load-bearing — the
    // original "grant key + let the stock loot pipeline open it" plan DEADLOCKED
    // (the tank parked at the cache and never looted). Two independent stock
    // failings sink that plan: (a) MaybeSkipUnworthyLoot blacklists every chest
    // with DungeonClear.IgnoreChests on (the default), and (b) even reached,
    // OpenLootAction's CanOpenLock has the LOCK_KEY_ITEM case COMMENTED OUT, so it
    // can never produce an opening spell for a key-item lock — and the core only
    // opens such a lock when the KEY ITEM ITSELF is the cast item (Spell::
    // CanOpenLock: m_CastItem->GetEntry() == lockInfo->Index[j]). So the leader
    // must use the key on the chest itself; the resulting Player::SendLoot fires
    // SMSG_LOOT_RESPONSE -> the always-on "store loot" handler auto-stores it.
    // The chest is consumable (leaves GO_READY once used), so the loot-state check
    // is the completion latch; idempotent + self-healing across an event restart.
    constexpr uint32 MECH_GO_CACHE_NORMAL = 184465;
    constexpr uint32 MECH_GO_CACHE_HEROIC = 184849;
    constexpr uint32 MECH_ITEM_CACHE_KEY  = 30438;  // on-use spell 3366 (OPEN_LOCK)
    constexpr float  MECH_CACHE_SEARCH    = 25.0f;
    // Cast reach: the OPEN_LOCK ends in Player::SendLoot, which range-checks the
    // GO's interact box (the Deadmines/Durnholde item-use plants measured failing
    // past ~6yd). Stay well inside it; the objective anchor sits ON the chest, but
    // arrival jitter can leave the tank a couple of yards out, so walk the final
    // gap in rather than spam-casting from range.
    constexpr float  MECH_CACHE_CAST_REACH = 4.0f;

    // True if a real human is in the leader's party (or is the lone actor). A member
    // counts as human when it has no PlayerbotAI at all, or is a self-bot — both
    // are someone who will walk up and loot the cache. A pure bot (a PlayerbotAI
    // driven for somebody else) does not.
    bool PartyHasRealPlayer(Player* bot)
    {
        auto const isHuman = [](Player* p) -> bool
        {
            // Pre-PR-2592 this read `!ai || ai->IsRealPlayer()`. That member is
            // gone, and the free IsRealPlayer() which inherited the NAME covers
            // only the first half of it — no bot AI at all. The second half, the
            // self-bot, is IsSelfBot(). Writing IsRealPlayer(p) alone here would
            // compile and quietly stop counting the self-bot as human.
            return DcPlayerbotCompat::IsHumanControlled(p);
        };

        Group* group = bot->GetGroup();
        if (!group)
            return isHuman(bot);

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (isHuman(ref->GetSource()))
                return true;
        return false;
    }

    ObjectiveArriveResult GrantCacheKeyAndLoot(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        // Full-bot run: skip the open entirely. The cache (184465/184849) is a
        // CONSUMABLE chest that only despawns once its loot is actually LOOTED, and in
        // a bot party nobody takes it — the stock loot pipeline ignores chests
        // (DungeonClear.IgnoreChests). So opening it just leaves the chest cycling its
        // loot-state (a visible flicker) until the step times out ~2 min later, and
        // the loot is worthless to a bot party anyway. Complete the objective and let
        // the tank move straight on to the elevator. Only when a real player is in the
        // group — who will loot it — do we run the Blizzlike open below, so normal
        // runs are unchanged. (This is why the hang never appeared in human-led runs.)
        if (!PartyHasRealPlayer(bot))
            return ObjectiveArriveResult::Done;

        GameObject* cache = bot->FindNearestGameObject(MECH_GO_CACHE_NORMAL, MECH_CACHE_SEARCH);
        if (!cache)
            cache = bot->FindNearestGameObject(MECH_GO_CACHE_HEROIC, MECH_CACHE_SEARCH);

        if (!cache)
        {
            // No cache in range: either the tank has not arrived / scanned it yet,
            // or it has already been looted and despawned (consumable). Once we
            // have handed over the key the latter is true -> Done; otherwise keep
            // waiting to arrive.
            return bot->HasItemCount(MECH_ITEM_CACHE_KEY, 1)
                       ? ObjectiveArriveResult::Done
                       : ObjectiveArriveResult::Running;
        }

        // The chest leaves GO_READY the instant the key-use OPEN_LOCK lands (the
        // loot window is up and the "store loot" handler is draining it) — a
        // stable, idempotent "this cache is done" latch.
        if (cache->getLootState() != GO_READY)
            return ObjectiveArriveResult::Done;

        // The 2s key "Opening" cast is already running — let it complete rather
        // than re-issue and interrupt ourselves every tick.
        if (bot->IsNonMeleeSpellCast(false))
            return ObjectiveArriveResult::Running;

        // Grant the key (bots never combined the crystals).
        Item* key = bot->GetItemByEntry(MECH_ITEM_CACHE_KEY);
        if (!key)
        {
            bot->AddItem(MECH_ITEM_CACHE_KEY, 1);
            key = bot->GetItemByEntry(MECH_ITEM_CACHE_KEY);
            if (!key)
                return ObjectiveArriveResult::Running;  // bags full this tick — retry
        }

        // Close the final yards if the objective anchor left us just outside the
        // interact box (SendLoot's range check is unforgiving). Nothing else moves
        // the tank while the event holds, so this walk-in sticks.
        if (bot->GetExactDist(cache) > MECH_CACHE_CAST_REACH)
        {
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, cache->GetPositionX(), cache->GetPositionY(),
                                                  cache->GetPositionZ());
            return ObjectiveArriveResult::Running;
        }

        // USE the key ON the chest: item-use supplies m_CastItem so the KEY_ITEM
        // lock opens -> SendLoot -> SMSG_LOOT_RESPONSE -> "store loot" stores it.
        // Feral-form druids can't cast the item spell at all — shift back first
        // (same gate the barrel plants use). See DcFormGate.
        DcFormGate::DropBlockingForm(bot, key);
        SpellCastTargets targets;
        targets.SetGOTarget(cache);
        bot->CastItemUseSpell(key, targets, 0, 0);
        return ObjectiveArriveResult::Running;  // the loot-state latch above confirms it
    }

    // hookId -> behaviour. To give an objective on-arrival behaviour, add a row
    // here and reference its id from a BossRosterRegistry objective (onArriveHook)
    // or a Custom event step (DungeonEventRegistry).
    //
    // Everything registered HERE is a one-shot arrival action: it does its thing
    // once and returns Running only while waiting for a latch to confirm it took.
    // A hook that instead has to re-decide every tick — owning its own movement,
    // engagement and target selection for the length of an encounter — is a
    // controller, not an action, and gets its own TU with an appender declared in
    // the header (see RegisterBlackMorassHooks). That distinction, not file size,
    // is what decides where a hook lives.
    ObjectiveHookRegistry::HookTable const& Hooks()
    {
        static ObjectiveHookRegistry::HookTable const kHooks = []
        {
            using Reg = ObjectiveHookRegistry;
            Reg::HookTable t;
            Reg::AddHook(t, 1, &EnsureRingStarted);      // BRD Ring of Law — start the arena event
            Reg::AddHook(t, 3, &GrantIncendiaryBombs);   // Old Hillsbrad — pack of bombs (unlocks Brazen)
            Reg::AddHook(t, 4, &GrantCacheKeyAndLoot);   // The Mechanar — Cache of the Legion key + loot
            Reg::AddHook(t, 5, &WakeZumrah);             // ZulFarrak — walk into Zum'rah's area trigger
            Reg::AddHook(t, 6, &DriveMellicharWaves);    // Arcatraz — poke Mellichar, hold through the waves
            Reg::AddHook(t, 7, &DriveAnzuSummon);        // Sethekk Halls — force-summon Anzu (send-event 14797)
            Reg::AddHook(t, 9, &StartNethekurseIntro);   // Shattered Halls — fire Nethekurse's client-only intro
            Reg::AddHook(t, 10, &SendGhazanToPlatform);  // The Underbog — send Ghaz'an up his ramp (AT 4302)
            Reg::AddHook(t, 13, &HadronoxHasWebbedTheDoors);  // Azjol-Nerub — hold until Hadronox webs the doors
            Reg::AddHook(t, 14, &HoldNovosCamp);         // Drak'Tharon Keep — hold the Novos camp through phase 1
            Reg::AddHook(t, DC_HOOK_RAID_MUSTER, &RaidMusterAtObjective);  // generic — raid muster at an objective

            // Controllers, one TU each. Called explicitly (not self-registering)
            // because this module is a static lib: a TU whose only output is
            // constructor side-effects has no referenced symbol and the linker
            // drops it, taking its hooks with it. Ids 8 and 12 live here.
            RegisterBlackMorassHooks(t);
            RegisterVioletHoldHooks(t);
            RegisterBlackwingLairHooks(t);
            RegisterHallsOfStoneHooks(t);
            RegisterHallsOfLightningHooks(t);
            RegisterUtgardePinnacleHooks(t);
            RegisterPitOfSaronHooks(t);
            RegisterHallsOfReflectionHooks(t);
            RegisterCullingOfStratholmeHooks(t);
            RegisterTrialOfTheChampionHooks(t);
            RegisterOculusHooks(t);
            RegisterKarazhanChessHooks(t);
            RegisterKarazhanHooks(t);
            RegisterStratholmeHooks(t);
            RegisterBlackrockDepthsHooks(t);
            RegisterBlackrockSpireHooks(t);
            RegisterBlackfathomDeepsHooks(t);
            return t;
        }();
        return kHooks;
    }
}

void ObjectiveHookRegistry::AddHook(HookTable& table, uint32 id, Hook hook)
{
    if (id == 0)
    {
        LOG_ERROR("playerbots", "DungeonClear: objective hook id 0 is reserved for "
                                "'no hook' and cannot be registered — row dropped");
        return;
    }

    if (!hook)
    {
        LOG_ERROR("playerbots", "DungeonClear: objective hook id {} registered with an "
                                "empty callable — row dropped", id);
        return;
    }

    if (!table.emplace(id, std::move(hook)).second)
    {
        // Two dungeons claimed the same id. The flat id space makes this a
        // copy-paste away, and emplace's silent first-wins would leave the loser's
        // objective latching Done with its event never running. Name it loudly; the
        // registry-integrity gtest turns it into an author-time failure.
        LOG_ERROR("playerbots", "DungeonClear: DUPLICATE objective hook id {} — a second "
                                "registration was dropped, keeping the first. Hook ids are "
                                "one flat space across all dungeons; pick an unused id.", id);
    }
}

ObjectiveArriveResult ObjectiveHookRegistry::Run(uint32 hookId, Player* bot, AiObjectContext* context,
                                                 DungeonBossInfo const& info)
{
    if (hookId == 0)
        return ObjectiveArriveResult::Done;
    // Hooks script shortcuts (granted items, faction swaps, forced damage and
    // object states). Only the audited spatial encounter drivers are admitted.
    if (DcEarnedPlay::Active() && hookId != 1 && hookId != 5)
        return ObjectiveArriveResult::Blocked;

    auto const& hooks = Hooks();
    auto it = hooks.find(hookId);
    if (it == hooks.end() || !it->second)
    {
        // A non-zero hookId that resolves to nothing is a wiring bug (a typo, or a
        // hook that was removed). Latching Done would silently pass a step/objective
        // authored to do real work; Blocked surfaces it (the run stalls for the
        // human) and the throttled warn names the id. Registry-integrity gtest
        // catches this at author time; this is the runtime backstop.
        static std::atomic<uint32> lastWarnMs{0};
        uint32 const now = getMSTime();
        uint32 prev = lastWarnMs.load(std::memory_order_relaxed);
        if (now - prev > 10000u &&
            lastWarnMs.compare_exchange_strong(prev, now, std::memory_order_relaxed))
        {
            LOG_WARN("playerbots", "DungeonClear: objective hookId {} is not registered "
                                   "(broken wiring) -> Blocked", hookId);
        }
        return ObjectiveArriveResult::Blocked;
    }

    return it->second(bot, context, info);
}

bool ObjectiveHookRegistry::Has(uint32 hookId)
{
    if (hookId == 0)
        return false;
    auto const& hooks = Hooks();
    auto it = hooks.find(hookId);
    return it != hooks.end() && it->second;
}
