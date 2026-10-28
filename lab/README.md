# Pull Lab

Pull Lab is Dungeon Clear's inherited developer harness for staging encounters,
recording action traces and scoring pull behavior. It provisions or resets test
parties, changes encounter state and injects faults; these operations do not
belong to earned play.

In [AzerothCore Playerbots Portable](https://github.com/Syntax555/mod-playerbots-portable),
`Playerbots.NaturalProgression = 1` refuses `.dc lab run` and `.dc lab batch`,
including background party provisioning. `.dc lab status` and `.dc lab stop`
remain available. Provisioned `.dc test` starts/plans are also refused. The
[normal dungeon guide](https://github.com/Syntax555/mod-playerbots-portable/blob/main/docs/bot-dungeons.md)
describes supported earned-party runs.

## Existing records and sources

Scenario data lives in `scenarios/`; human-puppet scripts live in `humans/`.
Existing lab records use `dc_labruns.jsonl` and `lab_traces/<run-id>.jsonl` in
the worldserver working directory. Offline tools in [../tools](../tools/README.md)
can inspect saved traces and logs; reading an old record does not establish that
its staged behavior is available in the portable profile.

For the inherited scenario schema, oracle thresholds and isolated harness
workflow, see the [imported upstream Lab guide](https://github.com/jrad7/mod-dungeon-clear/blob/7fb6ebdefb8038af2676c280214f8a15c843b347/lab/README.md).
Its provisioning and encounter shortcuts are developer tooling, not portable
installation or gameplay instructions. Follow [../AGENTS.md](../AGENTS.md) and
the assembly's [contribution rules](https://github.com/Syntax555/mod-playerbots-portable/blob/main/CONTRIBUTING.md)
for changes; preserve earned-play refusals. Native validation uses the matching
assembly sources and relevant production fixtures.

Copyright and licensing remain those of [Dungeon Clear](../LICENSE),
AGPL-3.0-or-later, by Jared Wright and contributors.
