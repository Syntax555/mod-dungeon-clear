# Repository guidance

This fork belongs to [AzerothCore Playerbots Portable](https://github.com/Syntax555/mod-playerbots-portable/blob/main/README.md).
Before editing, read its [repository rules](https://github.com/Syntax555/mod-playerbots-portable/blob/main/AGENTS.md), the shared
skills those rules require, [README](https://github.com/Syntax555/mod-playerbots-portable/blob/main/README.md),
[contributing guide](https://github.com/Syntax555/mod-playerbots-portable/blob/main/CONTRIBUTING.md) and
[Bot dungeons and raids](https://github.com/Syntax555/mod-playerbots-portable/blob/main/docs/bot-dungeons.md). Those rules also apply when working in this
fork alone and take precedence over imported upstream contribution instructions.

Commit validated changes on `main`; pin the commit in the assembly's
`versions.lock.json` and record fixture-covered behavior commits in `forkChanges`.
Keep original upstream history, licenses and file notices. Never rewrite a
commit pinned by the assembly. Validate native changes with the assembly's
prepared core and relevant production fixtures before committing.
