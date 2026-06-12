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

        QueryCallbackProcessor g_queryProcessor;

        // The flag set this bridge last applied to the facade registry, used to
        // reconcile by diff: a row removed service-side unflags the bot on the
        // next poll without tombstones.
        std::unordered_set<uint32> g_appliedFlags;

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

        LOG_INFO("playerbots", "Chronicle narrative bridge {} (seam poll every {}s)",
                 g_enabled ? "enabled" : "disabled", seconds);
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
    }
}  // namespace Chronicle
