# Capy platform — workflow & conventions (mutualisé)

> **Ce fichier est mutualisé** entre tous les repos NomenAK gérés par Capy. Si tu modifies une règle ici, propage-la aux autres repos. Le contenu projet-spécifique vit dans `CAPTAIN.md` (architecture, ADRs, phase) et `BUILD.md` (stack, commandes, conventions code).
>
> Les Captain et Build agents Capy DOIVENT lire ce fichier avant action. Si une règle métier (CAPTAIN.md / BUILD.md) entre en conflit avec une règle Capy ici, la règle métier gagne — et tu déclenches un patch sur ce fichier en parallèle.

---

## 1. VM Capy & secrets

**VM éphémère.** Toute modification non-commitée meurt entre deux turns. Réflexes :
- Commit dès qu'une unité cohérente est produite (G001).
- `git status` doit retourner « working tree clean » en fin de turn.
- Si commit explicitement bloqué (attente validation user), résumer dans le message ce qui serait perdu.

**Secrets.** Capy hydrate les env vars en mémoire. JAMAIS écrire un secret dans un fichier (G017). Règles strictes :
- Ne créer / modifier QUE `.env.example` (template safe). Tout autre `.env*` interdit.
- Pas de `git add .` aveugle — toujours add explicite ou `git add -p`. Le leak de `.env` 2026-05-12 a été causé par un add aveugle.
- Tokens (`LINEAR_API_KEY`, `MISTRAL_API_KEY`, `GITHUB_API_KEY`, `TAILSCALE_*`, `OMNIROUTE_*`) viennent uniquement de l'env Capy.

---

## 2. Captain ↔ Build workflow (G025)

Quand Captain délègue à un Build agent (via `create_tasks`), le Build tourne sur une VM isolée. Les frictions répertoriées :

### G025.a — Branche héritée stale
Le Build hérite de la branche thread-locale du Captain (`repos: [{branch: ...}]`). Si cette branche a été mergée pendant le run, le Build push sur du stale et contamine sa PR avec des fichiers déjà sur main.

**Règle Captain** : passer `repos: [{branch: "main"}]` à `create_tasks` quand pas de stacking explicite voulu.

**Règle Build (à inclure dans les prompts)** :
> Step 1 obligatoire : `git fetch origin main && git checkout -B capy/<feature>-task<N>-<slug> origin/main`. Jamais push sur la branche héritée si nom partagé.

### G025.b — Build ouvre la PR sans qu'on lui demande
Le Build a son tool `create_pr` natif et l'utilise spontanément. Si Captain crée aussi la PR → doublon avec mauvaise base.

**Règle Captain** : avant chaque `create_pr` côté Captain, vérifier `gh pr list --head <branch>`. Soit accepter que le Build ouvre la PR (et juste valider scope post), soit prompt explicite « STOP avant create_pr, exit après push uniquement ».

### G025.c — Branches parallèles forkées du même main pré-merge
Wave A lancée parallèle depuis le même `main` snapshot. La 1re PR mergée avance main ; les autres branches sont en retard et leurs diffs montrent de **faux deletes** sur les fichiers déjà mergés ailleurs.

**Règle Build (à inclure dans les prompts)** : avant `git push`, faire `git fetch origin main && git rebase origin/main` puis `--force-with-lease` push.

**Règle Captain** : préférer le pattern « Wave A merge complète → Wave B lancée depuis main+1 » plutôt que « Wave A+B simultanés, merge au fil de l'eau ».

### Réflexe systématique Captain post-Build
Pour chaque Build qui annonce « push done » :
1. `git fetch origin <branch>`
2. `git diff --stat origin/main origin/<branch>` — valider fileCount + sens des changes (pas de faux deletes, pas de fichiers hors scope).
3. Si PR déjà créée par Build : `gh pr view <n> --json files,baseRefName,headRefName` — valider scope.
4. Si OK : `gh pr merge <n> --squash --admin --delete-branch`.
5. Si KO : `gh pr close <n>` + `send_task_messages` pour rebase ou repush ciblé.

---

## 3. Native Capy tools (rappel surface)

Les agents Captain ont accès à un set de tools natifs. Préférer ces tools au shell quand applicable :

| Besoin | Tool natif Capy | Alternative shell |
|---|---|---|
| Créer une PR pour une task Build | `create_pr(index: N, repoFullName: ...)` | `gh pr create` (uniquement pour edits inline Captain) |
| Push une branche Build sans PR | `git_push(index: N)` | `git push` (uniquement pour edits inline Captain) |
| Inspecter PR (mergeable, review state) | `get_pr_overview(prNumber: ..., repoFullName: ...)` | `gh pr view --json` |
| Inspecter CI checks | `get_pr_checks(prNumber: ..., onlyFailing: true)` | `gh run view` |
| Trigger PR review automatique | `run_pr_review(prNumber: ...)` | n/a |
| Lire findings PR review | `get_pr_review_findings(prNumber: ...)` | n/a |
| Envoyer message à une task | `send_task_messages([{index, message}])` | n/a |
| Stop une task in-progress | `stop_tasks(indices: [...])` | n/a |
| Lister tasks du thread | `list_tasks()` | n/a |
| View task transcript | `view_task(index: N)` | n/a |

`gh CLI` reste utile pour : merger admin (`gh pr merge --admin`), close PRs en doublon, bulk operations, lister PRs ouvertes cross-thread.

---

## 4. Linear coordination

Workspace : `neuromantes`. Team principale : `OMG` (Omega + Chronicle).

**Quand Linear MCP est instable** : utiliser le CLI fallback installé par `setup-cli-tools.sh` :
- `LINEAR_API_KEY=$LINEAR_API_KEY linear issue view OMG-NN`
- `LINEAR_API_KEY=$LINEAR_API_KEY linear team list`
- Pour descriptions / comments multi-lignes : préférer `--body-file <path>` au shell-embedded (G003 — checkboxes truncate sur certains parsers).

**Référencer Linear dans commits** : `OMG-NN: short description`. Dans PR body : citer le ticket/initiative explicitement.

### Canonical labels (overhaul 2026-05-14)

- **`type:*`** — `type:bug`, `type:feature`, `type:refactor`, `type:doc`, `type:infra`, `type:adr-followup`, `type:tension-resolution`
- **`app:*`** — `app:chronicle`, `app:chronicle-launcher`, `app:kernel`, `app:omniroute`, `app:paperclip`, `app:meta`, `app:cross-app`, `app:wow335a`, `app:fork-azerothcore`, `app:fork-mod-playerbots`
- **`tech:*`** — `tech:azerothcore`, `tech:elixir`, `tech:bots`, `tech:voice`, `tech:ai-stack`, `tech:paperclip`
- **`handoff`** — sessions handoff cross-context
- **`infra`** — workspace-wide infra concern

### Ticket convention

```
Title: [<scope>] short description (ou T-CHR-XX-NN — title pour Chronicle work tickets)
Body sections:
- Status / Context
- Goal
- Scope (bullets)
- Constraints (hard rules)
- DoD (checkable items)
- Refs (ADRs, sibling tickets, PRs)
```

---

## 5. PR & merge policy

- Captain ne merge PAS sur `main`/`develop`/`master`/`release/*` sauf si user demande explicitement le merge dans la conversation actuelle.
- Stacked PRs (capy/feature → capy/parent) : OK de planifier, créer, updater, reporter ready ; merger uniquement si demande explicite.
- Auto-merge interdit sauf demande explicite.
- Après CI / review verts, reporter via `message_user` que la PR est ready, ne pas merger.

---

## 6. Gotchas process (cross-repo)

Tableau cross-repo des gotchas process Capy. Les gotchas projet-spécifiques (G024 ALE userdata, etc.) restent dans le `gotchas.md` de chaque repo.

| ID | Symptôme | Règle |
|---|---|---|
| G001 | Travail non-committé perdu au reset VM | Commit unité cohérente, working tree clean fin de turn |
| G002 | Pollution working tree Windows NTFS mode-flips | `git config core.filemode false` local au repo |
| G003 | Linear MCP truncate checkbox lists | Bullets numérotés ou comments ; toujours verify post-update |
| G017 | Build agent leak `.env` via add aveugle | Add explicite uniquement, `.env` gitignored, env vars en mémoire |
| G018 | Diff trompeur dans notification Capy | Toujours `git fetch + git diff origin/main origin/branch` post-Build |
| G019 | Self-hosted runner state pollution | Ajouter cleanup hooks dans CI scripts |
| G020 | Build commit local jamais pushé | Réflexe `git log origin/branch` post-Build, `git_push(index: N)` si manquant |
| G022 | Self-hosted runner stale submodule dirs | `git submodule foreach 'git clean -ffdx'` après checkout |
| G025 | Build agent + Captain PR workflow frictions | Voir section 2 ci-dessus (G025.a/b/c) |

---

## 7. Cross-thread persistence

- Les threads Captain sont persistants côté plateforme. Threads codes `OMEGA-NNNN` etc.
- `view_thread(threadId)` pour rappeler contexte d'un thread précédent.
- `archive_thread(threadId)` pour fermer un thread quand son sujet est livré (la PR liée n'est PAS auto-archivée).
- Ne pas confondre Task index (thread-local : Task 1, 2, ...) avec Linear issues (`OMG-NN`).

---

## 8. Updates à ce fichier

Ce fichier vit dans `.capy/CAPY-PLATFORM.md` de **chaque repo NomenAK** géré par Capy. Quand une règle change ici :

1. Update dans le repo où le besoin émerge.
2. Propage aux 5 autres repos via `send_task_messages` ou inline edit + 6 PRs distincts.
3. Note la propagation dans le commit message : `docs(capy): propagate <rule X> across repos NomenAK`.

Repos couverts (au 2026-05-15) :
- `NomenAK/Chronicle`
- `NomenAK/chronicle-launcher`
- `NomenAK/Omega`
- `NomenAK/wow335a-ac-portable`
- `NomenAK/azerothcore-wotlk-chronicle` (fork — `.capy/` fork-local pour éviter conflits upstream)
- `NomenAK/mod-playerbots-chronicle` (fork — `.capy/` fork-local idem)
