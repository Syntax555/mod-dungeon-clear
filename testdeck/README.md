# DC Test Deck

Test Deck is the inherited web interface for Dungeon Clear's provisioned test
runs, saved rosters, continuous test plans and recorded run logs. It is
developer tooling, installed separately from the portable server.

The assembly's `Playerbots.NaturalProgression = 1` refuses provisioned `.dc test`
starts/plans and Pull Lab launches. Launch, roster and continuous-run controls
therefore cannot create earned-play parties. Test Deck does not enable the
supported autonomous coordinator; use the
[normal dungeon guide](https://github.com/Syntax555/mod-playerbots-portable/blob/main/docs/bot-dungeons.md)
for that feature. Do not treat staged test success as a verified natural run.

## Development reference

The backend is under `testdeck/`, the UI source under `web/` and its compiled
assets under `dist/`. `testdeck.example.toml` describes configuration.
The backend can inspect existing test records and logs, but needs its own host
setup and database access; it is not bundled as a portable-server service.

The [imported upstream Test Deck guide](https://github.com/jrad7/mod-dungeon-clear/blob/7fb6ebdefb8038af2676c280214f8a15c843b347/testdeck/README.md)
covers the isolated harness, transports, configuration and UI development.
[docs/api.md](docs/api.md) describes the API. Those harness instructions include
provisioning and relocation, which the portable earned-play profile refuses.
Do not change production progression settings to make the test frontend launch.

Read [../AGENTS.md](../AGENTS.md) and the assembly's
[contribution workflow](https://github.com/Syntax555/mod-playerbots-portable/blob/main/CONTRIBUTING.md)
before changing this tooling. Run the checks relevant to changed backend or UI
files; native gameplay validation uses the assembly's matching dependencies
and production fixtures.

Copyright and licensing remain those of [Dungeon Clear](../LICENSE),
AGPL-3.0-or-later, by Jared Wright and contributors.
