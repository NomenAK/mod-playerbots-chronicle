# Build instructions — mod-playerbots-chronicle (Capy)

## Git & PR workflow

These rules keep PRs linked to their Capy task/thread. A PR opened with a raw
`gh pr create` on a non-`capy/*` branch is **not** attached to the thread.

1. **Always work on a `capy/<slug>` branch.** Step 1 of any task:
   `git fetch origin master && git checkout -B capy/<slug> origin/master`.
2. **Never run `gh pr create` yourself.** Commit, push the `capy/*` branch, and let
   the platform's managed PR path open and link the PR. Stop after push.
3. Before pushing a parallel branch: `git fetch origin master && git rebase origin/master`,
   then push with `--force-with-lease`.
4. Never `git add .` blindly — stage explicitly. `.env` is gitignored; secrets come
   from the Capy environment, never written to files.
