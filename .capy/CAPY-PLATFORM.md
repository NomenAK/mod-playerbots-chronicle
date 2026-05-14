# Capy platform workflow

## 1. Secrets and environment

- Never commit secrets, tokens, credentials, private keys, `.env*` files, local config dumps, or generated credential material.
- Use Capy-provided environment variables in memory only. Do not print secret values, persist them to files, or copy them into issue/PR text.
- Safe templates such as `.env.example` may be edited only when they contain placeholders, never real values.
- If a secret is exposed, stop and report it; do not attempt ad-hoc rotation from the repository.

## 2. Git and PR discipline

- Keep changes narrow and reviewable. Prefer explicit path staging over broad `git add .`.
- Do not create or update PRs unless the task explicitly asks for it.
- Push only when explicitly requested. Never push directly to protected default branches unless the task names that branch and asks for it.
- Preserve upstream-owned files in forks unless the task specifically targets them.

## 3. Validation

- Run the most direct check that proves the requested change works.
- For docs-only changes, verify file paths, markdown readability, links when practical, and absence of unintended generated artifacts.
- If an expensive build is not relevant to the change, do not run it just to signal activity; state the scoped validation performed.

## 4. Linear coordination

- Use Linear only for coordination requested by the task or by repo-local guidance.
- Use the configured `LINEAR_API_KEY` from the environment or the available CLI/MCP integration; never store API keys in repository files.
- Prefer issue identifiers such as `OMG-123` in commits, PR descriptions, and notes when work maps to tracked planning.
- Keep repository docs focused on project-specific Linear context; generic CLI/auth instructions belong here, not in every repo-local Captain file.

## 5. Capy local docs

- `.capy/` files are repository-local agent guidance and should stay concise.
- `CAPY-PLATFORM.md` holds shared Capy workflow/security conventions.
- `CAPTAIN.md` holds repository-specific planning and ownership context.
- Avoid adding root-level agent files in forks when upstream may later add the same filenames.
