# Captain context — mod-playerbots-chronicle (fork)

> Voir aussi [.capy/CAPY-PLATFORM.md](./CAPY-PLATFORM.md).

## What this is

Fork of `mod-playerbots/mod-playerbots` — the AI bot framework for AzerothCore. It provides PvE/PvP/random-bot intelligence consumed by the Chronicle WoW 3.3.5a narrative private server.

This fork is a **sub-submodule**: it lives under `azerothcore-wotlk-chronicle/modules/mod-playerbots` and is loaded by Chronicle transitively through Chronicle's AzerothCore fork.

Keep divergence minimal. Chronicle-specific bot patches belong here only when they cannot be expressed downstream through config, data, or module wiring and cannot be accepted upstream.

## Branches

| Branch | Role |
|---|---|
| `master` | Default branch; upstream-sync baseline mirroring `mod-playerbots/mod-playerbots@master` |
| Feature branches | Chronicle-specific patches or process/docs changes isolated from upstream sync commits |

## Consumed by

- Transitively via [`NomenAK/azerothcore-wotlk-chronicle`](https://github.com/NomenAK/azerothcore-wotlk-chronicle) at `modules/mod-playerbots`
- Ultimate consumer: [`NomenAK/Chronicle`](https://github.com/NomenAK/Chronicle) through its AzerothCore submodule

## Upstream sync strategy

- Track upstream movement via Chronicle's `playerbots-upstream-watch` automation (`Chronicle/scripts/playerbots-upstream-watch.js`).
- Notable coordination item: [OMG-169](https://linear.app/neuromantes/issue/OMG-169) — adopt upstream PR #168 (`PlayerbotsDatabase` as core) when ready.
- Pull opportunistically after validation in an isolation worktree before bumping downstream pointers.
- Keep upstream merge/cherry-pick commits separate from Chronicle-specific patch commits.
- Prefer contributing or cherry-picking broadly useful changes upstream instead of carrying private divergence.

## Hard rules

1. **DO NOT modify upstream code** unless the patch is Chronicle-specific and impossible elsewhere. Prefer downstream config/data/module wiring first, then upstream contribution or cherry-pick.
2. **DO NOT create a root `CLAUDE.md`** — upstream may add one later, and this fork avoids predictable merge conflicts.
3. **Commit messages** follow upstream convention; inspect `setup_git_commit_template.sh` for the project template.
4. **Secrets** follow [.capy/CAPY-PLATFORM.md §1](./CAPY-PLATFORM.md#1-secrets-and-environment). Never persist tokens or private config in this repo.
5. **Generated artifacts** stay out of the fork unless upstream already tracks the same artifact class.
6. **Prefer downstream configuration** over fork patches when Chronicle can express behavior through config, SQL/data, or AzerothCore module wiring.

## CI

- Upstream CI expectation applies on `master`.
- Chronicle integration validation happens downstream through `azerothcore-wotlk-chronicle` and ultimately `Chronicle`.
- For docs/process-only changes, verify changed files and skip expensive AzerothCore/PlayerBots builds unless requested.

## Linear / coordination

Team/project context: `Omega · Chronicle · PlayerBots`.

Keep generic Linear CLI/auth guidance in [.capy/CAPY-PLATFORM.md §4](./CAPY-PLATFORM.md#4-linear-coordination), not here.

## Related repos

- Parent fork / transitive consumer: https://github.com/NomenAK/azerothcore-wotlk-chronicle
- Ultimate consumer: https://github.com/NomenAK/Chronicle
- Upstream: https://github.com/mod-playerbots/mod-playerbots

## Local agent note

`.capy/` is fork-local agent context. It intentionally avoids root-level agent instruction files so upstream syncs have fewer predictable conflicts.
