/*
 * Chronicle D027 — narrative companion seam bridge (impl).
 * See NarrativeBridge.h for the design + threading notes. AGPL v3.
 */

#include "NarrativeBridge.h"

#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "AsyncCallbackProcessor.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "NarrativeCompanion.h"
#include "QueryResult.h"

namespace Chronicle
{
    namespace
    {
        // All of this state is world-thread-only: Initialize/Update run on the
        // world thread, and QueryCallbackProcessor invokes the completions there
        // too. The async part (the actual SQL) happens on the DB worker thread
        // and only ever touches the value-typed QueryResult handed back.
        bool g_enabled = false;
        uint32 g_pollIntervalMs = 5 * IN_MILLISECONDS;
        uint32 g_timeSinceLastPoll = 0;
        bool g_flagsQueryInFlight = false;
        bool g_commandsQueryInFlight = false;
        bool g_repliesQueryInFlight = false;
        bool g_creatureRepliesQueryInFlight = false;
        bool g_emotesQueryInFlight = false;
        bool g_channelsEnabled = false;
        bool g_channelRepliesQueryInFlight = false;

        QueryCallbackProcessor g_queryProcessor;

        // The flag set this bridge last applied to the facade registry, used to
        // reconcile by diff: a row removed service-side unflags the bot on the
        // next poll without tombstones.
        std::unordered_set<uint32> g_appliedFlags;

        // Companions (player-controlled bots) this bridge has already emitted a
        // "gained" activation for. Diffed each scan so gained/released fire exactly
        // once per transition (Vague 4 — extend the companion to ALL bots a real
        // player controls).
        std::unordered_set<uint32> g_seenCompanions;

        // A cleared command the master triggered but that never reached an
        // online bot must not fire much later (a "sell" the master walked away
        // from never executes behind his back). Mirrors the service-side
        // confirm-flow TTL default (contract §4, pending_ttl_seconds = 120).
        constexpr int64 kCommandTtlSeconds = 120;

        // Per-poll drain cap. The companion flow is whisper-paced (one master,
        // one confirm step); anything beyond a handful per cycle is a flood and
        // can wait for the next poll or expire via the TTL above.
        constexpr uint32 kCommandDrainBatch = 16;

        void ReconcileFlags(QueryResult result)
        {
            // Full snapshot on purpose: the table holds the few player-owned
            // companions of a 10-50 player realm. A null result (empty table OR
            // query error) unflags everything — fail-closed: narrative degrades,
            // whisper routing reverts to stock gameplay parsing (the expected
            // Chronicle narrative failure mode).
            std::unordered_set<uint32> fresh;
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    fresh.insert(fields[0].Get<uint32>());
                } while (result->NextRow());
            }

            for (uint32 guidLow : fresh)
            {
                if (g_appliedFlags.find(guidLow) == g_appliedFlags.end())
                {
                    NarrativeCompanion::FlagNarrativeBot(guidLow);
                    LOG_INFO("playerbots",
                             "Chronicle narrative bridge: flagged bot guid {} as narrative companion", guidLow);
                }
            }

            for (uint32 guidLow : g_appliedFlags)
            {
                if (fresh.find(guidLow) == fresh.end())
                {
                    NarrativeCompanion::UnflagNarrativeBot(guidLow);
                    LOG_INFO("playerbots", "Chronicle narrative bridge: unflagged bot guid {}", guidLow);
                }
            }

            g_appliedFlags = std::move(fresh);
        }

        void DrainCommands(QueryResult result)
        {
            if (!result)
                return;

            std::vector<uint32> consumedIds;
            consumedIds.reserve(kCommandDrainBatch);

            do
            {
                Field* fields = result->Fetch();
                uint32 const id = fields[0].Get<uint32>();
                uint32 const botGuid = fields[1].Get<uint32>();
                uint32 const masterGuid = fields[2].Get<uint32>();
                std::string const verb = fields[3].Get<std::string>();
                std::string const command = fields[4].Get<std::string>();
                int64 const ageSeconds = fields[5].Get<int64>();

                consumedIds.push_back(id);

                if (ageSeconds > kCommandTtlSeconds)
                {
                    LOG_WARN("playerbots",
                             "Chronicle narrative bridge: dropped stale cleared command {} for bot {} "
                             "(verb '{}', age {}s > {}s TTL)",
                             id, botGuid, verb, ageSeconds, kCommandTtlSeconds);
                    continue;
                }

                // The facade is the gate: whitelist re-check (refusal logged),
                // narrative flag, (bot, master) pairing — then HandleCommand.
                NarrativeCompanion::DispatchClearedCommand(botGuid, masterGuid, verb, command);
            } while (result->NextRow());

            if (consumedIds.empty())
                return;

            // Consume drained rows whether they executed, were refused or were
            // stale: the seam is at-most-once by design. Retries/proposals are
            // owned service-side; a refused row must never retry forever here.
            std::ostringstream del;
            del << "DELETE FROM chronicle_narrative_bot_commands WHERE id IN (";
            for (std::size_t i = 0; i < consumedIds.size(); ++i)
            {
                if (i)
                    del << ',';
                del << consumedIds[i];
            }
            del << ')';
            PlayerbotsDatabase.Execute(del.str());
        }

        // Forward sink (D027 amendement 2026-06-13): a master→companion whisper
        // captured by NarrativeCompanion::TryForwardWhisper is written to the
        // inbound seam for narrative_service to drain. Runs on the world thread;
        // the INSERT is async (PlayerbotsDatabase.Execute), so this returns
        // promptly (G011/G012) and never blocks chat. Returns true to consume the
        // whisper (suppress native routing). The free-text is escaped — it is the
        // only user-controlled string written on this seam.
        bool ForwardWhisperToSeam(CompanionWhisper const& whisper)
        {
            std::string text = whisper.text;
            PlayerbotsDatabase.EscapeString(text);

            std::ostringstream ins;
            ins << "INSERT INTO chronicle_narrative_bot_whispers "
                   "(bot_guid, master_guid, chat_type, text, created_at) VALUES ("
                << whisper.botGuid << ',' << whisper.masterGuid << ',' << whisper.chatType << ",'"
                << text << "', NOW())";
            PlayerbotsDatabase.Execute(ins.str());
            return true;
        }

        void DrainReplies(QueryResult result)
        {
            if (!result)
                return;

            std::vector<uint32> consumedIds;
            consumedIds.reserve(kCommandDrainBatch);

            do
            {
                Field* fields = result->Fetch();
                uint32 const id = fields[0].Get<uint32>();
                uint32 const botGuid = fields[1].Get<uint32>();
                uint32 const masterGuid = fields[2].Get<uint32>();
                std::string const text = fields[3].Get<std::string>();
                int64 const ageSeconds = fields[4].Get<int64>();

                consumedIds.push_back(id);

                if (ageSeconds > kCommandTtlSeconds)
                {
                    LOG_WARN("playerbots",
                             "Chronicle narrative bridge: dropped stale reply {} for bot {} "
                             "(age {}s > {}s TTL)",
                             id, botGuid, ageSeconds, kCommandTtlSeconds);
                    continue;
                }

                // The facade gates: narrative flag + (bot, master) pairing, then
                // the bot whispers its master via TellMaster.
                NarrativeCompanion::DeliverReply(botGuid, masterGuid, text);
            } while (result->NextRow());

            if (consumedIds.empty())
                return;

            // At-most-once, like commands: consume whether delivered, refused or
            // stale. Any retry is owned service-side.
            std::ostringstream del;
            del << "DELETE FROM chronicle_narrative_bot_replies WHERE id IN (";
            for (std::size_t i = 0; i < consumedIds.size(); ++i)
            {
                if (i)
                    del << ',';
                del << consumedIds[i];
            }
            del << ')';
            PlayerbotsDatabase.Execute(del.str());
        }

        // D030 Phase 1 (creature reply seam): drain narrative_service-written
        // voiced-NPC say-back rows and speak them via the facade
        // (DeliverCreatureReply → Unit::Say + face + emote). Symmetric to
        // DrainReplies; at-most-once (consume whether spoken, dropped or stale).
        // This REPLACES the retired chronicle_apply_decision.lua HTTP poll, moving
        // creature speech onto the same async DB seam as the companion — 0 HTTP in
        // the world thread, cost independent of the voiced-NPC roster size.
        void DrainCreatureReplies(QueryResult result)
        {
            if (!result)
                return;

            std::vector<uint32> consumedIds;
            consumedIds.reserve(kCommandDrainBatch);

            do
            {
                Field* fields = result->Fetch();
                uint32 const id = fields[0].Get<uint32>();
                uint32 const creatureEntry = fields[1].Get<uint32>();
                uint32 const playerGuid = fields[2].Get<uint32>();
                std::string const text = fields[3].Get<std::string>();
                uint32 const emoteId = fields[4].Get<uint32>();
                int64 const ageSeconds = fields[5].Get<int64>();

                consumedIds.push_back(id);

                if (ageSeconds > kCommandTtlSeconds)
                {
                    LOG_WARN("playerbots",
                             "Chronicle narrative bridge: dropped stale creature reply {} for entry {} "
                             "(age {}s > {}s TTL)",
                             id, creatureEntry, ageSeconds, kCommandTtlSeconds);
                    continue;
                }

                // The facade resolves the live creature near the asker, then faces
                // + speaks + emotes. No master gate (a creature is not a companion).
                NarrativeCompanion::DeliverCreatureReply(creatureEntry, playerGuid, text, emoteId);
            } while (result->NextRow());

            if (consumedIds.empty())
                return;

            std::ostringstream del;
            del << "DELETE FROM chronicle_narrative_creature_replies WHERE id IN (";
            for (std::size_t i = 0; i < consumedIds.size(); ++i)
            {
                if (i)
                    del << ',';
                del << consumedIds[i];
            }
            del << ')';
            PlayerbotsDatabase.Execute(del.str());
        }

        // Beta Spec 09 (social): forward a captured global-channel message to the
        // matchmaking seam. The two free-text fields are escaped. Async INSERT
        // (world thread, returns promptly). Inert unless the channel sink is wired.
        void ForwardChannelToSeam(ChannelMessage const& m)
        {
            std::string channel = m.channel;
            std::string text = m.text;
            PlayerbotsDatabase.EscapeString(channel);
            PlayerbotsDatabase.EscapeString(text);

            std::ostringstream ins;
            ins << "INSERT INTO chronicle_narrative_channel_messages "
                   "(sender_guid, channel, text, created_at) VALUES ("
                << m.senderGuid << ",'" << channel << "','" << text << "', NOW())";
            PlayerbotsDatabase.Execute(ins.str());
        }

        // Beta Spec 09 (social): drain matchmaking responses (the elected bot
        // whispers the asker). Symmetric to DrainReplies; at-most-once + TTL.
        void DrainChannelResponses(QueryResult result)
        {
            if (!result)
                return;

            std::vector<uint32> consumedIds;
            consumedIds.reserve(kCommandDrainBatch);

            do
            {
                Field* fields = result->Fetch();
                uint32 const id = fields[0].Get<uint32>();
                uint32 const botGuid = fields[1].Get<uint32>();
                uint32 const targetGuid = fields[2].Get<uint32>();
                std::string const text = fields[3].Get<std::string>();
                int64 const ageSeconds = fields[4].Get<int64>();

                consumedIds.push_back(id);

                if (ageSeconds > kCommandTtlSeconds)
                {
                    LOG_WARN("playerbots",
                             "Chronicle narrative bridge: dropped stale channel reply {} (age {}s > {}s TTL)",
                             id, ageSeconds, kCommandTtlSeconds);
                    continue;
                }

                NarrativeCompanion::DeliverChannelResponse(botGuid, targetGuid, text);
            } while (result->NextRow());

            if (consumedIds.empty())
                return;

            std::ostringstream del;
            del << "DELETE FROM chronicle_narrative_channel_responses WHERE id IN (";
            for (std::size_t i = 0; i < consumedIds.size(); ++i)
            {
                if (i)
                    del << ',';
                del << consumedIds[i];
            }
            del << ')';
            PlayerbotsDatabase.Execute(del.str());
        }

        // Beta Spec 09 (emote parity): drain narrative_service-written emote rows
        // and play them via the facade (DeliverEmote → Unit::HandleEmoteCommand).
        // Symmetric to DrainReplies; at-most-once (consume whether played, refused
        // or stale). emote_id is a plain uint (no free-text → no escaping).
        void DrainEmotes(QueryResult result)
        {
            if (!result)
                return;

            std::vector<uint32> consumedIds;
            consumedIds.reserve(kCommandDrainBatch);

            do
            {
                Field* fields = result->Fetch();
                uint32 const id = fields[0].Get<uint32>();
                uint32 const botGuid = fields[1].Get<uint32>();
                uint32 const masterGuid = fields[2].Get<uint32>();
                uint32 const emoteId = fields[3].Get<uint32>();
                int64 const ageSeconds = fields[4].Get<int64>();

                consumedIds.push_back(id);

                if (ageSeconds > kCommandTtlSeconds)
                {
                    LOG_WARN("playerbots",
                             "Chronicle narrative bridge: dropped stale emote {} for bot {} "
                             "(age {}s > {}s TTL)",
                             id, botGuid, ageSeconds, kCommandTtlSeconds);
                    continue;
                }

                NarrativeCompanion::DeliverEmote(botGuid, masterGuid, emoteId);
            } while (result->NextRow());

            if (consumedIds.empty())
                return;

            std::ostringstream del;
            del << "DELETE FROM chronicle_narrative_bot_emotes WHERE id IN (";
            for (std::size_t i = 0; i < consumedIds.size(); ++i)
            {
                if (i)
                    del << ',';
                del << consumedIds[i];
            }
            del << ')';
            PlayerbotsDatabase.Execute(del.str());
        }

        // --- companion discovery → activation seam (Vague 4) ---

        // Emit a "gained" activation: the bot is now a player-controlled
        // companion. narrative_service resolves its deterministic persona and
        // flags it BEFORE the master's first whisper. Async INSERT (world thread,
        // returns promptly). The name is the only free-text field → escaped.
        void EmitActivationGained(CompanionPresence const& c)
        {
            std::string name = c.name;
            PlayerbotsDatabase.EscapeString(name);

            std::ostringstream ins;
            ins << "INSERT INTO chronicle_narrative_bot_activations "
                   "(bot_guid, master_guid, event, class, race, gender, level, bot_name, created_at) "
                   "VALUES ("
                << c.botGuid << ',' << c.masterGuid << ",'gained'," << uint32(c.cls) << ','
                << uint32(c.race) << ',' << uint32(c.gender) << ',' << c.level << ",'" << name
                << "', NOW())";
            PlayerbotsDatabase.Execute(ins.str());
        }

        // Emit a "released" activation: the bot lost its real-player master
        // (logout / dismiss / BG). narrative_service unflags it. Only the bot guid
        // is known (the master may be gone) → master_guid 0.
        void EmitActivationReleased(uint32 botGuidLow)
        {
            std::ostringstream ins;
            ins << "INSERT INTO chronicle_narrative_bot_activations "
                   "(bot_guid, master_guid, event, created_at) VALUES ("
                << botGuidLow << ",0,'released', NOW())";
            PlayerbotsDatabase.Execute(ins.str());
        }

        // Diff the currently player-controlled companions against the last scan: a
        // new one emits "gained", a vanished one emits "released". Synchronous but
        // bounded (CollectActiveCompanions scans the player map under its read
        // lock); the INSERTs are async. Re-login re-emits "gained" (self-heal if a
        // prior emit was lost). World thread (called from Update at poll cadence).
        void DiscoverCompanions()
        {
            std::vector<CompanionPresence> present =
                NarrativeCompanion::CollectActiveCompanions();

            std::unordered_set<uint32> current;
            current.reserve(present.size());

            for (CompanionPresence const& c : present)
            {
                current.insert(c.botGuid);
                if (g_seenCompanions.find(c.botGuid) == g_seenCompanions.end())
                {
                    EmitActivationGained(c);
                    LOG_INFO("playerbots",
                             "Chronicle narrative bridge: companion gained — bot {} master {}",
                             c.botGuid, c.masterGuid);
                }
            }

            for (uint32 botGuidLow : g_seenCompanions)
            {
                if (current.find(botGuidLow) == current.end())
                {
                    EmitActivationReleased(botGuidLow);
                    LOG_INFO("playerbots",
                             "Chronicle narrative bridge: companion released — bot {}", botGuidLow);
                }
            }

            g_seenCompanions = std::move(current);
        }
    }  // namespace

    void NarrativeBridge::Initialize()
    {
        g_enabled = sConfigMgr->GetOption<bool>("Chronicle.NarrativeBridge.Enable", true);

        uint32 seconds = sConfigMgr->GetOption<uint32>("Chronicle.NarrativeBridge.PollIntervalSeconds", 5);
        if (seconds < 1)
            seconds = 1;
        g_pollIntervalMs = seconds * IN_MILLISECONDS;

        // Start "due" so the first reconcile happens right after world start
        // instead of one full interval later.
        g_timeSinceLastPoll = g_pollIntervalMs;

        // Wire the companion forward sink so master→companion whispers land on the
        // inbound seam. Only when enabled — otherwise TryForwardWhisper finds no
        // sink and the path stays inert (stock gameplay parsing).
        if (g_enabled)
            NarrativeCompanion::SetForwardSink(ForwardWhisperToSeam);

        // Beta Spec 09 (social): channel capture is OFF by default — opt-in via
        // worldserver.conf (Chronicle.Channels.Capture). When on, real-player
        // channel messages are captured to the matchmaking seam.
        g_channelsEnabled = sConfigMgr->GetOption<bool>("Chronicle.Channels.Capture", false);
        if (g_enabled && g_channelsEnabled)
            NarrativeCompanion::SetChannelSink(ForwardChannelToSeam);

        LOG_INFO("playerbots", "Chronicle narrative bridge {} (seam poll every {}s, channels {})",
                 g_enabled ? "enabled" : "disabled", seconds, g_channelsEnabled ? "on" : "off");
    }

    void NarrativeBridge::Update(uint32 diff)
    {
        if (!g_enabled)
            return;

        // Apply completed async queries on the world thread (registry reconcile
        // + command dispatch both require it).
        g_queryProcessor.ProcessReadyCallbacks();

        g_timeSinceLastPoll += diff;
        if (g_timeSinceLastPoll < g_pollIntervalMs)
            return;

        g_timeSinceLastPoll = 0;

        // In-flight guards: if the DB is slow, do not stack a second identical
        // query — skip this cadence and let the pending one land first.
        if (!g_flagsQueryInFlight)
        {
            g_flagsQueryInFlight = true;
            g_queryProcessor.AddCallback(
                PlayerbotsDatabase.AsyncQuery("SELECT guid FROM chronicle_narrative_bots")
                    .WithCallback([](QueryResult result)
                    {
                        g_flagsQueryInFlight = false;
                        ReconcileFlags(std::move(result));
                    }));
        }

        if (!g_commandsQueryInFlight)
        {
            g_commandsQueryInFlight = true;
            std::ostringstream sel;
            sel << "SELECT id, bot_guid, master_guid, verb, command, "
                   "TIMESTAMPDIFF(SECOND, created_at, NOW()) "
                   "FROM chronicle_narrative_bot_commands ORDER BY id ASC LIMIT "
                << kCommandDrainBatch;
            g_queryProcessor.AddCallback(
                PlayerbotsDatabase.AsyncQuery(sel.str())
                    .WithCallback([](QueryResult result)
                    {
                        g_commandsQueryInFlight = false;
                        DrainCommands(std::move(result));
                    }));
        }

        if (!g_repliesQueryInFlight)
        {
            g_repliesQueryInFlight = true;
            std::ostringstream sel;
            sel << "SELECT id, bot_guid, master_guid, text, "
                   "TIMESTAMPDIFF(SECOND, created_at, NOW()) "
                   "FROM chronicle_narrative_bot_replies ORDER BY id ASC LIMIT "
                << kCommandDrainBatch;
            g_queryProcessor.AddCallback(
                PlayerbotsDatabase.AsyncQuery(sel.str())
                    .WithCallback([](QueryResult result)
                    {
                        g_repliesQueryInFlight = false;
                        DrainReplies(std::move(result));
                    }));
        }

        if (!g_creatureRepliesQueryInFlight)
        {
            g_creatureRepliesQueryInFlight = true;
            std::ostringstream sel;
            sel << "SELECT id, creature_entry, player_guid, text, emote_id, "
                   "TIMESTAMPDIFF(SECOND, created_at, NOW()) "
                   "FROM chronicle_narrative_creature_replies ORDER BY id ASC LIMIT "
                << kCommandDrainBatch;
            g_queryProcessor.AddCallback(
                PlayerbotsDatabase.AsyncQuery(sel.str())
                    .WithCallback([](QueryResult result)
                    {
                        g_creatureRepliesQueryInFlight = false;
                        DrainCreatureReplies(std::move(result));
                    }));
        }

        if (!g_emotesQueryInFlight)
        {
            g_emotesQueryInFlight = true;
            std::ostringstream sel;
            sel << "SELECT id, bot_guid, master_guid, emote_id, "
                   "TIMESTAMPDIFF(SECOND, created_at, NOW()) "
                   "FROM chronicle_narrative_bot_emotes ORDER BY id ASC LIMIT "
                << kCommandDrainBatch;
            g_queryProcessor.AddCallback(
                PlayerbotsDatabase.AsyncQuery(sel.str())
                    .WithCallback([](QueryResult result)
                    {
                        g_emotesQueryInFlight = false;
                        DrainEmotes(std::move(result));
                    }));
        }

        if (g_channelsEnabled && !g_channelRepliesQueryInFlight)
        {
            g_channelRepliesQueryInFlight = true;
            std::ostringstream sel;
            sel << "SELECT id, bot_guid, target_guid, text, "
                   "TIMESTAMPDIFF(SECOND, created_at, NOW()) "
                   "FROM chronicle_narrative_channel_responses ORDER BY id ASC LIMIT "
                << kCommandDrainBatch;
            g_queryProcessor.AddCallback(
                PlayerbotsDatabase.AsyncQuery(sel.str())
                    .WithCallback([](QueryResult result)
                    {
                        g_channelRepliesQueryInFlight = false;
                        DrainChannelResponses(std::move(result));
                    }));
        }

        // Companion discovery (Vague 4): a synchronous player-map diff at the poll
        // cadence — emits gained/released activation events to the seam so the
        // service flags/unflags player-controlled companions.
        DiscoverCompanions();
    }
}  // namespace Chronicle
