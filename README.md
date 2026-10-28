# Dungeon Clear

Dungeon Clear lets a tank bot lead a player group through an instance and forms
nearby random-bot parties for audited normal dungeons in
[AzerothCore Playerbots Portable](https://github.com/Syntax555/mod-playerbots-portable).
Bots walk navigation-mesh routes, fight, rest and loot with their own earned
levels, equipment and progression. This native module requires the assembly's
matching Playerbots, Individual Progression and core; it is not a client addon.

## Install

Use the assembly's [Latest release and setup guide](https://github.com/Syntax555/mod-playerbots-portable#quick-start)
for a playable Windows server. Dungeon Clear is included. The optional
[DungeonClear control addon](https://github.com/jrad7/mod-dungeon-clear-addon)
is installed separately and opens with `/dc`.

For a native build, follow the assembly's
[build guide](https://github.com/Syntax555/mod-playerbots-portable/blob/main/docs/building.md).
Its [dependency lock](https://github.com/Syntax555/mod-playerbots-portable/blob/main/versions.lock.json)
selects a tested commit of this fork's `master` branch and matching dependencies.
Rebuild worldserver after native changes; copying source into an installed
server does not install the module.

## Play and limits

Enter normally with a tank bot in your group, then use `.dc on` or `dc on` in
party chat. `.dc off` stops, `.dc pause` pauses or resumes, `.dc status` reports
progress and `.dc bosses` lists objectives. `.dc skip` advances the objective
list; it does not open doors or award encounter credit. The tank driving the
clear must be under bot control, including SelfBot.

Autonomous parties recruit five nearby same-faction random bots: tank, healer
and three damage dealers. Each member needs its own earned era, level band and
native entry eligibility. Controlled altbots, SelfBot characters and player
groups are not recruited. Parties walk to the physical portal; suitable nearby
roles and complete navigation data are required. They do not enter raids or
heroics.

Natural progression refuses teleports, free resurrection, artificial combat
recovery, granted items and unaudited event or movement controllers. Required
unsupported events need ordinary player interaction; optional ones may be
skipped. Missing routes or prerequisites can stall a clear. Provisioned test
parties and Pull Lab launches are unavailable in natural progression.

Blackfathom Deeps and Stratholme refuse autonomous recruitment because their
movement/event controllers are unaudited. Other routes have key, item or event
restrictions. Player-led groups can enter normally, with these guards still
active. See the [route list and encounter limits](https://github.com/Syntax555/mod-playerbots-portable/blob/main/docs/bot-dungeons.md).
Automated fixtures cover selected paths and guards; complete autonomous runs
still need in-game validation.

## Configuration

Edit `configs/modules/mod_dungeon_clear.conf` with the portable server stopped.
All native keys are in [conf/mod_dungeon_clear.conf.dist](conf/mod_dungeon_clear.conf.dist).

| Setting | Portable default | Native template |
| --- | --- | --- |
| `DungeonClear.Enable` | `1` | `1` |
| `DungeonClear.AutonomousParties.Enable` | `1` | `0` |
| `DungeonClear.AutonomousParties.MaxConcurrent` | `2` | `2` |
| `DungeonClear.AutonomousParties.LevelSpread` | `3` | `3` |
| `DungeonClear.AutonomousParties.Dungeons` | Normal Vanilla, TBC and Wrath candidates | `4,12` |

The portable profile keeps `Playerbots.NaturalProgression = 1` and the strict
default Individual Progression chain. A configured dungeon still needs the
runtime event audit, native entry checks and usable navigation. The startup log
reports route acceptance or refusal. See [configuration](https://github.com/Syntax555/mod-playerbots-portable/blob/main/docs/bot-dungeons.md#configuration)
for limits, disabled queue/recovery shortcuts and raid tactics.

## Source, contributions and license

Maintained in [Syntax555/mod-dungeon-clear](https://github.com/Syntax555/mod-dungeon-clear),
from [Jared Wright and contributors' upstream](https://github.com/jrad7/mod-dungeon-clear).
The [imported upstream README](https://github.com/jrad7/mod-dungeon-clear/blob/7fb6ebdefb8038af2676c280214f8a15c843b347/README.md)
describes the original project's broader tooling; the earned-play rules above
apply to this fork's portable use.

Read [AGENTS.md](AGENTS.md) and the assembly's
[contribution workflow](https://github.com/Syntax555/mod-playerbots-portable/blob/main/CONTRIBUTING.md)
before changing this fork. Portable integration CI checks relevant assembly
fixtures; native builds and release integration run in the assembly.

Copyright (C) 2026+ Jared Wright and contributors. Licensed under
[AGPL-3.0-or-later](LICENSE), inherited from and linking against Playerbots.
Original file notices and upstream history are retained.
