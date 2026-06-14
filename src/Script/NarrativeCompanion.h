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
 *  - Whitelist re-checked here (defense in depth): every cleared command enters
 *    through DispatchClearedCommand, the single decision-poll → HandleCommand
 *    entry point, which refuses (with a refusal log) any verb outside the
 *    mirrored whitelist before it can reach HandleCommand. A forged seam row
 *    cannot smuggle a non-whitelisted command.
 *  - Master only (G-LOOP-2): forwarding requires HasRealPlayerMaster() AND the
 *    whisper's sender == the bot's master; DispatchClearedCommand mirrors the
 *    same check on the way back (the command only runs for the master the
 *    service cleared it for).
 */

#ifndef _CHRONICLE_NARRATIVE_COMPANION_H
#define _CHRONICLE_NARRATIVE_COMPANION_H

#include <functional>
#include <string>
#include <vector>

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

    // Beta Spec 09 (social): a global-channel message from a REAL player, captured
    // for the matchmaking service. Pure value type — no Player*/Channel leaks past
    // the facade. Only real-player senders are ever captured (anti bot↔bot/cost).
    struct ChannelMessage
    {
        uint32 senderGuid = 0;  // real-player sender guid (low)
        std::string channel;    // channel name (e.g. "LookingForGroup")
        std::string text;       // the message
    };

    // A snapshot of one currently player-controlled companion (a bot whose master
    // is a real player). Pure value type — the discovery scan (Vague 4) hands
    // these to the bridge so no Player*/PlayerbotAI leaks past the facade.
    struct CompanionPresence
    {
        uint32 botGuid = 0;     // companion guid (low)
        uint32 masterGuid = 0;  // real-player master guid (low)
        uint8 cls = 0;          // 3.3.5a class id
        uint8 race = 0;         // 3.3.5a race id
        uint8 gender = 0;       // 3.3.5a gender id (0 male / 1 female)
        uint32 level = 0;       // bot level
        std::string name;       // bot character name
    };

    // The sink the bridge registers to actually forward a CompanionWhisper off the
    // world thread (thread-safe queue → HTTP POST to narrative_service). Returns
    // true when the whisper was consumed by the narrative path (so the caller must
    // NOT route it to the gameplay command parser). The default sink is a no-op
    // returning false — without a registered bridge the companion path is inert
    // and the bot behaves exactly as before (safe fallback).
    using CompanionForwardSink = std::function<bool(CompanionWhisper const&)>;

    // Beta Spec 09: the sink the bridge registers to forward a captured channel
    // message to the matchmaking service (thread-safe queue → async INSERT). The
    // default sink is unset → channel capture is inert (safe fallback). Fire-and-
    // forget (void): capturing a channel line never suppresses it.
    using ChannelCaptureSink = std::function<void(ChannelMessage const&)>;

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

        // Discovery scan (Vague 4): snapshot every currently player-controlled
        // companion (HasRealPlayerMaster()) as pure value structs. The bridge
        // diffs this against what it last emitted to drive the activation seam
        // (persona resolve + flag on gain, unflag on loss). MUST run on the world
        // thread (iterates the live player map under the HashMapHolder read lock).
        static std::vector<CompanionPresence> CollectActiveCompanions();

        // TRUE when the stock playerbot greeting/chatter for `botAI` must be
        // suppressed because narrative_service owns this bot's speech: any bot with
        // a real-player master (the companion population). Non-companion bots keep
        // the fork's stock greeting.
        static bool SuppressStockGreeting(PlayerbotAI* botAI);

        // The single routing decision used by OnPlayerCanUseChat. Returns TRUE
        // when the whisper was forwarded to narrative_service (caller should NOT
        // call HandleCommand for it). Returns FALSE to fall through to the native
        // gameplay path unchanged. Master-only (G-LOOP-2) is enforced here.
        static bool TryForwardWhisper(Player* fromPlayer, uint32 type, std::string const& msg,
                                      Player* receiver);

        // Beta Spec 07: a master's PARTY/RAID message to one of their narrative
        // companions is the SAME routing decision as a whisper — only the chat_type
        // differs (carried through to the seam). Called per group member from the
        // Group OnPlayerCanUseChat override; returns TRUE when forwarded (caller
        // skips that member's native HandleCommand). Master-only (G-LOOP-2) enforced.
        static bool TryForwardGroupChat(Player* fromPlayer, uint32 type, std::string const& msg,
                                        Player* member);

        // Runtime mutators for the narrative-flag registry (D027 amendement
        // 2026-06-12, bridge item 1). The registry is the ONLY thing that turns
        // the (otherwise inert) companion routing on for a given bot. Populated
        // at runtime by the seam bridge (NarrativeBridge) from the
        // chronicle_narrative_bots seam table written by narrative_service.
        // Same mutex guarantees as the readers — callable from any thread,
        // idempotent.
        static void FlagNarrativeBot(uint32 guidLow);
        static void UnflagNarrativeBot(uint32 guidLow);

        // The decision-poll → HandleCommand entry point for cleared bot_action
        // decisions (contract §2.3; D027 amendement 2026-06-12, bridge item 2).
        // Re-checks the mirrored verb whitelist (refusing with a log — defense
        // in depth), the narrative flag, and the (bot, master) pairing
        // (G-LOOP-2), then queues the native command string via the bot's own
        // HandleCommand/chatCommands mechanism — no new execution path. MUST be
        // called on the world thread (resolves live Player*); the bridge calls
        // it from its world-thread drain. Returns TRUE when the command was
        // handed to HandleCommand.
        static bool DispatchClearedCommand(uint32 botGuidLow, uint32 masterGuidLow,
                                           std::string const& verb, std::string const& command);

        // The reply-drain → TellMaster entry point for the companion say-back path
        // (D027 amendement 2026-06-13, Hybride). Symmetric to
        // DispatchClearedCommand: resolves the live bot, re-checks the narrative
        // flag + (bot, master) pairing (G-LOOP-2), then delivers `text` as a
        // bot→master whisper via the fork's native TellMaster. MUST be called on
        // the world thread (resolves live Player*); the bridge calls it from its
        // world-thread reply drain. Returns TRUE when the reply was delivered.
        static bool DeliverReply(uint32 botGuidLow, uint32 masterGuidLow, std::string const& text);

        // Beta Spec 09 (emote parity): the emote-drain entry point. Symmetric to
        // DeliverReply — resolves the live bot, re-checks the narrative flag +
        // (bot, master) pairing (G-LOOP-2), then plays a one-shot EMOTE_ONESHOT_*
        // animation via the native Unit::HandleEmoteCommand (the same call the fork
        // already uses for bot emotes — a new trigger, not a new capability). MUST
        // run on the world thread. Returns TRUE when the emote was played.
        static bool DeliverEmote(uint32 botGuidLow, uint32 masterGuidLow, uint32 emoteId);

        // Beta Spec 09 (social) — channel capture. Registered once at bridge init
        // (only when channel capture is enabled in config).
        static void SetChannelSink(ChannelCaptureSink sink);

        // Capture a global-channel message for the matchmaking service: forwards to
        // the channel sink IFF the sender is a REAL player (never a bot — anti
        // bot↔bot/cost) and a sink is registered. Fire-and-forget; never suppresses
        // the channel line. Called from the Channel OnPlayerCanUseChat override.
        static void CaptureChannelMessage(Player* sender, std::string const& channelName,
                                          std::string const& msg);

        // Beta Spec 09 (social) — the elected bot whispers the asker (the matchmaking
        // response). Resolves the live bot (must be an actual bot) + the target (must
        // be a REAL online player — never another bot, anti bot↔bot), then sends a
        // bot→player whisper via the native Player::Whisper. MUST run on the world
        // thread. Returns TRUE when delivered.
        static bool DeliverChannelResponse(uint32 botGuidLow, uint32 targetGuidLow,
                                           std::string const& text);

        // Defense-in-depth whitelist re-check for an inbound cleared command (the
        // verb arriving on the decision-poll path). The canonical list lives in
        // narrative_service (NarrativeService.Toolkit); this mirrors it so a forged
        // decision cannot inject a non-whitelisted command. TRUE = allowed.
        // Called by DispatchClearedCommand before anything reaches HandleCommand.
        static bool IsWhitelistedVerb(std::string const& verb);

    private:
        static bool IsNarrativeFlagged(PlayerbotAI* botAI);
        static CompanionForwardSink s_sink;
        static ChannelCaptureSink s_channelSink;
    };
}  // namespace Chronicle

#endif  // _CHRONICLE_NARRATIVE_COMPANION_H
