/*
 * Chronicle D027 P2.1d — narrative companion command routing facade.
 *
 * This file is part of the Chronicle fork of mod-playerbots, released under
 * GNU AGPL v3 (inherits the module license).
 *
 * ============================================================================
 *  PURPOSE — typed facade isolating the fork coupling (OQ-3 pre-flight §5)
 * ============================================================================
 * The player-owned narrative companion (D027 narrative_owned_bot) translates a
 * master's natural language into EXISTING playerbot verbs. The translation +
 * whitelist + confirm-gating all live in narrative_service (Elixir). This C++
 * facade is the thin server-side seam that:
 *
 *   1. decides whether an inbound whisper belongs to a narrative-flagged owned
 *      bot (so it should be FORWARDED to narrative_service instead of routed to
 *      the gameplay command parser), and
 *   2. wraps the fork's `Event`/`Player*` coupling behind a typed boundary so the
 *      rest of the narrative path never touches fork internals directly.
 *
 * It builds NO bot actions. When an action is cleared by narrative_service it
 * comes back over the existing decision-poll path as a WHITELISTED native command
 * string and is handed to the fork's own `HandleCommand`/`chatCommands` queue —
 * the same mechanism the bot already uses. See the integration contract:
 *   docs/superpowers/specs/2026-06-09-d027-p2.1d-cpp-integration-contract.md
 *
 * ============================================================================
 *  GUARDRAILS (G011-G014 + D027 §Sécurité)
 * ============================================================================
 *  - NO detached thread captures a Player*. The forward is delegated to a
 *    registered sink (the bridge) that MUST enqueue onto a thread-safe queue and
 *    do the HTTP off the world thread; this facade only passes scalars/strings.
 *  - NO LLM call in-game (D014). This facade never calls a model; it routes.
 *  - Whitelist re-checked here (defense in depth): an inbound cleared command is
 *    matched against the mirrored verb whitelist before it reaches HandleCommand,
 *    so a forged decision on the wire cannot smuggle a non-whitelisted command.
 *  - Master only (G-LOOP-2): forwarding requires HasRealPlayerMaster() AND the
 *    whisper's sender == the bot's master.
 */

#ifndef _CHRONICLE_NARRATIVE_COMPANION_H
#define _CHRONICLE_NARRATIVE_COMPANION_H

#include <functional>
#include <string>

#include "Define.h"

class Player;
class PlayerbotAI;

namespace Chronicle
{
    // The typed payload the facade forwards to narrative_service (via the sink).
    // Pure value type — no Player*/Event leaks past this boundary.
    struct CompanionWhisper
    {
        uint32 botGuid = 0;     // the companion's guid (low)
        uint32 masterGuid = 0;  // the resolved real-player master's guid (low)
        uint32 chatType = 0;    // CHAT_MSG_WHISPER, ...
        std::string text;       // the master's raw whisper text
    };

    // The sink the bridge registers to actually forward a CompanionWhisper off the
    // world thread (thread-safe queue → HTTP POST to narrative_service). Returns
    // true when the whisper was consumed by the narrative path (so the caller must
    // NOT route it to the gameplay command parser). The default sink is a no-op
    // returning false — without a registered bridge the companion path is inert
    // and the bot behaves exactly as before (safe fallback).
    using CompanionForwardSink = std::function<bool(CompanionWhisper const&)>;

    class NarrativeCompanion
    {
    public:
        // Registered once at bridge init. Not thread-safe by design: call before
        // the world thread is processing chat.
        static void SetForwardSink(CompanionForwardSink sink);

        // TRUE iff `botAI` is a narrative-flagged player-owned companion: it has a
        // real-player master AND carries the narrative flag (registry/config —
        // see IsNarrativeFlagged). RNDBots / gameplay bots are never narrative.
        static bool IsNarrativeOwnedBot(PlayerbotAI* botAI);

        // The single routing decision used by OnPlayerCanUseChat. Returns TRUE
        // when the whisper was forwarded to narrative_service (caller should NOT
        // call HandleCommand for it). Returns FALSE to fall through to the native
        // gameplay path unchanged. Master-only (G-LOOP-2) is enforced here.
        static bool TryForwardWhisper(Player* fromPlayer, uint32 type, std::string const& msg,
                                      Player* receiver);

        // Defense-in-depth whitelist re-check for an inbound cleared command (the
        // verb arriving on the decision-poll path). The canonical list lives in
        // narrative_service (NarrativeService.Toolkit); this mirrors it so a forged
        // decision cannot inject a non-whitelisted command. TRUE = allowed.
        static bool IsWhitelistedVerb(std::string const& verb);

    private:
        static bool IsNarrativeFlagged(PlayerbotAI* botAI);
        static CompanionForwardSink s_sink;
    };
}  // namespace Chronicle

#endif  // _CHRONICLE_NARRATIVE_COMPANION_H
