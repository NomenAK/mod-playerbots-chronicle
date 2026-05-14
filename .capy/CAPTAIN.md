# Captain context — mod-playerbots-chronicle (fork)

## What this is

Fork of `mod-playerbots/mod-playerbots` — the AI bot framework for AzerothCore. Provides PvE/PvP/random-bot intelligence consumed by the Chronicle WoW 3.3.5a narrative private server.

This fork is a **sub-submodule**: it lives under `azerothcore-wotlk-chronicle/modules/mod-playerbots` and is loaded by Chronicle transitively through Chronicle's `azerothcore/` submodule (pinned to `azerothcore-wotlk-chronicle@chronicle/main`).

Fork policy: keep minimal divergence — Chronicle-specific bot patches only when upstream cannot accept them. Upstream is active (PRs landing weekly), so cherry-picks are preferred over forking long-lived diverging branches.

## Branches

| Branch | Role |
|---|---|
| `master` | upstream sync (default) — mirrors `mod-playerbots/mod-playerbots@master` |
| Feature branches if needed | Chronicle-specific patches that cannot be upstreamed |

## Consumed by

- Transitively via [`NomenAK/azerothcore-wotlk-chronicle`](https://github.com/NomenAK/azerothcore-wotlk-chronicle) at path `modules/mod-playerbots`
- Ultimate consumer: [`NomenAK/Chronicle`](https://github.com/NomenAK/Chronicle) via its `azerothcore/` submodule

## Upstream sync strategy

- Tracked via Chronicle's `playerbots-upstream-watch` automation (`Chronicle/scripts/playerbots-upstream-watch.js`)
- Notable: [OMG-169](https://linear.app/neuromantes/issue/OMG-169) — adopt upstream PR #168 (PlayerbotsDatabase as core) when ready
- Pull cadence: opportunistic, post-validation in a separate worktree before bumping Chronicle submodule pin
- Validate upstream pulls in isolation before updating the downstream AzerothCore module pointer
- Keep upstream merge/cherry-pick commits separate from Chronicle-specific patch commits

## Hard rules

1. **DO NOT modify upstream code** unless the patch is Chronicle-specific AND cannot live in a sibling config / data file. Prefer cherry-pick or contribution upstream.
2. **DO NOT create a `CLAUDE.md`** at the root — upstream may add one, and we don't want merge conflicts.
3. **Commit messages** follow upstream convention. Look at `setup_git_commit_template.sh` for the project template.
4. **Never write secrets to `.env` files** — Capy harness hydrates env vars in memory. `.env` is gitignored to defend against accidental commits.
5. **Keep generated artifacts out of the fork** unless upstream already tracks the same artifact class.
6. **Prefer downstream configuration** over fork patches when Chronicle can express behavior through config, SQL data, or AzerothCore module wiring.

## CI

Upstream CI runs on `master` (build matrix). Chronicle integration is validated downstream via Chronicle's docker-compose smoke.

When a patch changes bot behavior, expect validation in the parent AzerothCore fork before Chronicle bumps any pinned reference.

## Linear / coordination

Workspace: `neuromantes`. Team: `OMG`. Project: `Omega · Chronicle · PlayerBots`. Use `linear` CLI fallback. Never persist API keys to repo files.

Use Linear references for coordination only; do not encode API tokens, workspace tokens, or private incident details in repository files.

## Related repos

- Parent fork (consumer): https://github.com/NomenAK/azerothcore-wotlk-chronicle
- Ultimate consumer: https://github.com/NomenAK/Chronicle
- Upstream: https://github.com/mod-playerbots/mod-playerbots

## Local agent note

This file is fork-local Captain context. It intentionally lives under `.capy/` instead of root-level agent instruction files to avoid future upstream merge conflicts.
