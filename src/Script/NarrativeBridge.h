/*
 * Chronicle D027 — narrative companion seam bridge (runtime activation).
 *
 * This file is part of the Chronicle fork of mod-playerbots, released under
 * GNU AGPL v3 (inherits the module license).
 *
 * ============================================================================
 *  PURPOSE — the data-driven half of the D027 companion seam
 * ============================================================================
 * NarrativeCompanion (the facade) owns the routing/whitelist decisions but is
 * inert by itself: nothing in the committed seam could flag a bot as narrative
 * at runtime (D027 ADR, amendement 2026-06-12 — "Suite nécessaire"). This
 * bridge closes that gap, driven purely by seam DATA in the playerbots
 * database — the same DB-as-control-plane model as the canonical
 * ai_playerbot_texts seam (Chronicle invariant 3). narrative_service WRITES,
 * the worldserver READS:
 *
 *   chronicle_narrative_bots          guid-low set of narrative-flagged
 *                                     player-owned companions. Reconciled by
 *                                     diff into the facade registry
 *                                     (FlagNarrativeBot / UnflagNarrativeBot);
 *                                     removing a row unflags the bot.
 *
 *   chronicle_narrative_bot_commands  cleared bot_action decisions (whitelisted
 *                                     verb + native command string), the last
 *                                     hop of the decision-poll path. Drained
 *                                     at-most-once into
 *                                     NarrativeCompanion::DispatchClearedCommand,
 *                                     which whitelist-gates (with refusal log)
 *                                     before the bot's own HandleCommand.
 *
 * ============================================================================
 *  THREADING (G011/G012)
 * ============================================================================
 * No DB work per tick: polls run at a configurable interval (default 5 s) as
 * ASYNC queries (DB worker thread), and the completions are applied on the
 * world thread via QueryCallbackProcessor — so registry reconcile and command
 * dispatch (which resolves live Player*) always execute on the world thread.
 * No detached threads, no blocking I/O on the world thread.
 *
 * Config (worldserver .conf):
 *   Chronicle.NarrativeBridge.Enable               (default 1)
 *   Chronicle.NarrativeBridge.PollIntervalSeconds  (default 5, min 1)
 * With empty seam tables, behavior is identical to stock.
 */

#ifndef _CHRONICLE_NARRATIVE_BRIDGE_H
#define _CHRONICLE_NARRATIVE_BRIDGE_H

#include "Define.h"

namespace Chronicle
{
    class NarrativeBridge
    {
    public:
        // Reads config. Call once before the world starts updating
        // (PlayerbotsWorldScript::OnBeforeWorldInitialized).
        static void Initialize();

        // World-thread pump (PlayerbotsWorldScript::OnUpdate): applies ready
        // query completions and schedules the next seam poll when the interval
        // elapsed. Cheap when idle — no DB work outside the poll cadence.
        static void Update(uint32 diff);
    };
}  // namespace Chronicle

#endif  // _CHRONICLE_NARRATIVE_BRIDGE_H
