/*
 * Chronicle D027 P2.1d — narrative companion command routing facade (impl).
 * See NarrativeCompanion.h for the design + guardrail notes. AGPL v3.
 */

#include "NarrativeCompanion.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_set>

#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"

namespace Chronicle
{
    CompanionForwardSink NarrativeCompanion::s_sink = nullptr;

    namespace
    {
        // Per-bot narrative flag registry. Populated at runtime by the seam bridge
        // (NarrativeBridge reconciles it against the chronicle_narrative_bots seam
        // table via FlagNarrativeBot/UnflagNarrativeBot); empty by default so the
        // companion path is INERT until the seam flags bots. A plain set guarded
        // by a mutex — touched only on (slow-cadence) seam reconcile and on
        // whisper routing, never on a hot per-frame path.
        std::mutex g_flagMutex;
        std::unordered_set<uint32> g_narrativeBots;

        // The verb whitelist, mirrored from narrative_service
        // (NarrativeService.Toolkit.verb_names/0). Kept in sync via the integration
        // contract. This is the C++ defense-in-depth copy: it never has to be the
        // authority, only refuse anything the service would also refuse.
        constexpr std::array<char const*, 13> kWhitelistedVerbs = {
            "follow", "stay", "guard", "come", "stop", "flee", "attack",
            "sell", "buy", "repair", "loot", "quest_start", "quest_end"};
    }  // namespace

    void NarrativeCompanion::SetForwardSink(CompanionForwardSink sink) { s_sink = std::move(sink); }

    bool NarrativeCompanion::IsNarrativeFlagged(PlayerbotAI* botAI)
    {
        if (!botAI)
            return false;

        Player* bot = botAI->GetBot();
        if (!bot)
            return false;

        std::lock_guard<std::mutex> lock(g_flagMutex);
        return g_narrativeBots.find(bot->GetGUID().GetCounter()) != g_narrativeBots.end();
    }

    void NarrativeCompanion::FlagNarrativeBot(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(g_flagMutex);
        g_narrativeBots.insert(guidLow);
    }

    void NarrativeCompanion::UnflagNarrativeBot(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(g_flagMutex);
        g_narrativeBots.erase(guidLow);
    }

    bool NarrativeCompanion::IsNarrativeOwnedBot(PlayerbotAI* botAI)
    {
        // Player-owned (real master) AND narrative-flagged. RNDBots fail the first
        // test; gameplay alts fail the second. Both must hold.
        return botAI && botAI->HasRealPlayerMaster() && IsNarrativeFlagged(botAI);
    }

    bool NarrativeCompanion::TryForwardWhisper(Player* fromPlayer, uint32 type, std::string const& msg,
                                               Player* receiver)
    {
        if (!fromPlayer || !receiver)
            return false;

        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);
        if (!IsNarrativeOwnedBot(botAI))
            return false;

        // G-LOOP-2: a companion only ever serves its own master. A whisper from
        // anyone else falls through to the native path (which itself ignores
        // non-master command attempts via the fork's security checks).
        if (botAI->GetMaster() != fromPlayer)
            return false;

        if (!s_sink)
            return false;  // no bridge registered → behave exactly as stock.

        CompanionWhisper whisper;
        whisper.botGuid = receiver->GetGUID().GetCounter();
        whisper.masterGuid = fromPlayer->GetGUID().GetCounter();
        whisper.chatType = type;
        whisper.text = msg;

        // The sink MUST hand this off to a thread-safe queue and return promptly
        // (no blocking HTTP on the world thread — G011/G012). It returns true when
        // it consumed the whisper, in which case we suppress native routing.
        return s_sink(whisper);
    }

    bool NarrativeCompanion::DispatchClearedCommand(uint32 botGuidLow, uint32 masterGuidLow,
                                                    std::string const& verb, std::string const& command)
    {
        // Defense in depth (D027 amendement 2026-06-12, item 2): narrative_service
        // is the whitelist authority, but a forged/corrupted seam row must be
        // refused HERE, before anything reaches HandleCommand — with a log.
        if (!IsWhitelistedVerb(verb))
        {
            LOG_WARN("playerbots",
                     "Chronicle narrative: refused cleared command for bot {} — verb '{}' is not whitelisted",
                     botGuidLow, verb);
            return false;
        }

        if (command.empty())
        {
            LOG_WARN("playerbots", "Chronicle narrative: refused empty cleared command for bot {} (verb '{}')",
                     botGuidLow, verb);
            return false;
        }

        // World thread only from here on: live Player* resolution.
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuidLow));
        if (!bot)
        {
            // Not an attack, just timing (bot logged out between clear and drain).
            LOG_DEBUG("playerbots", "Chronicle narrative: dropped cleared command '{}' — bot {} is not in world",
                      verb, botGuidLow);
            return false;
        }

        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!IsNarrativeOwnedBot(botAI))
        {
            LOG_WARN("playerbots",
                     "Chronicle narrative: refused cleared command '{}' — bot {} is not a narrative-flagged "
                     "owned companion",
                     verb, botGuidLow);
            return false;
        }

        // G-LOOP-2, mirrored from TryForwardWhisper: the command runs AS the bot's
        // current master, and only for the master the service cleared it for.
        Player* master = botAI->GetMaster();
        if (!master || master->GetGUID().GetCounter() != masterGuidLow)
        {
            LOG_WARN("playerbots",
                     "Chronicle narrative: refused cleared command '{}' for bot {} — master mismatch "
                     "(cleared for {}, bot serves {})",
                     verb, botGuidLow, masterGuidLow, master ? master->GetGUID().GetCounter() : 0);
            return false;
        }

        // Native mechanism, unchanged (contract §2.3): HandleCommand applies the
        // fork's own security checks for `master` and pushes a ChatCommandHolder
        // onto the bot's chatCommands queue. NOTE: if AiPlayerbot.CommandPrefix is
        // configured (default empty), the seam writer must prepend it.
        botAI->HandleCommand(CHAT_MSG_WHISPER, command, master);
        return true;
    }

    bool NarrativeCompanion::DeliverReply(uint32 botGuidLow, uint32 masterGuidLow,
                                          std::string const& text)
    {
        if (text.empty())
            return false;

        // World thread only from here on: live Player* resolution.
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuidLow));
        if (!bot)
        {
            // Timing, not an attack (bot logged out between reply write and drain).
            LOG_DEBUG("playerbots", "Chronicle narrative: dropped reply — bot {} is not in world", botGuidLow);
            return false;
        }

        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!IsNarrativeOwnedBot(botAI))
        {
            LOG_WARN("playerbots",
                     "Chronicle narrative: refused reply — bot {} is not a narrative-flagged owned companion",
                     botGuidLow);
            return false;
        }

        // G-LOOP-2, mirrored from DispatchClearedCommand: the say-back only ever
        // reaches the master the service addressed it to.
        Player* master = botAI->GetMaster();
        if (!master || master->GetGUID().GetCounter() != masterGuidLow)
        {
            LOG_WARN("playerbots",
                     "Chronicle narrative: refused reply for bot {} — master mismatch "
                     "(addressed {}, bot serves {})",
                     botGuidLow, masterGuidLow, master ? master->GetGUID().GetCounter() : 0);
            return false;
        }

        // Native say-back: the bot whispers its master — the same mechanism the
        // bot already uses for all its master-directed chatter. No new path.
        return botAI->TellMaster(text);
    }

    bool NarrativeCompanion::IsWhitelistedVerb(std::string const& verb)
    {
        return std::any_of(kWhitelistedVerbs.begin(), kWhitelistedVerbs.end(),
                           [&](char const* v) { return verb == v; });
    }
}  // namespace Chronicle
