/*
 * Chronicle D027 P2.1d — narrative companion command routing facade (impl).
 * See NarrativeCompanion.h for the design + guardrail notes. AGPL v3.
 */

#include "NarrativeCompanion.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#include "Creature.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"

namespace Chronicle
{
    CompanionForwardSink NarrativeCompanion::s_sink = nullptr;
    ChannelCaptureSink NarrativeCompanion::s_channelSink = nullptr;

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

    void NarrativeCompanion::SetChannelSink(ChannelCaptureSink sink) { s_channelSink = std::move(sink); }

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

    std::vector<CompanionPresence> NarrativeCompanion::CollectActiveCompanions()
    {
        std::vector<CompanionPresence> out;

        // Iterate the live player map under its read lock (same idiom as
        // TravelMgr). Bounded: a 10-50 player realm + its bots. World thread only.
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();

        for (auto const& itr : players)
        {
            Player* const bot = itr.second;
            if (!bot)
                continue;

            PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            // Companion = a bot with a REAL-player master. RNDBots (no master) and
            // bot-mastered bots are excluded: the master must be human. This is the
            // discovery-side half of the strict bot↔bot barrier.
            if (!botAI || !botAI->HasRealPlayerMaster())
                continue;

            Player* const master = botAI->GetMaster();
            if (!master)
                continue;

            CompanionPresence c;
            c.botGuid = bot->GetGUID().GetCounter();
            c.masterGuid = master->GetGUID().GetCounter();
            c.cls = bot->getClass();
            c.race = bot->getRace();
            c.gender = bot->getGender();
            c.level = bot->GetLevel();
            c.name = bot->GetName();
            out.push_back(std::move(c));
        }

        return out;
    }

    bool NarrativeCompanion::SuppressStockGreeting(PlayerbotAI* botAI)
    {
        // narrative_service owns companion speech: suppress stock chatter for any
        // bot with a real-player master. Non-companion bots keep stock behavior.
        return botAI && botAI->HasRealPlayerMaster();
    }

    bool NarrativeCompanion::TryForwardWhisper(Player* fromPlayer, uint32 type, std::string const& msg,
                                               Player* receiver)
    {
        if (!fromPlayer || !receiver)
            return false;

        // Strict bot↔bot barrier (Vague 4): only a REAL player's whisper ever
        // enters the narrative path. A bot whispering anything can never trigger
        // an LLM interaction (anti-loop / anti-cost). Defense in depth — the
        // master-pairing check below already implies a human sender, but we refuse
        // a bot sender explicitly and early.
        PlayerbotAI* const fromAI = PlayerbotsMgr::instance().GetPlayerbotAI(fromPlayer);
        if (fromAI && !fromAI->IsRealPlayer())
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

    bool NarrativeCompanion::TryForwardGroupChat(Player* fromPlayer, uint32 type, std::string const& msg,
                                                 Player* member)
    {
        // Party/raid chat from a master to their narrative companion is the SAME
        // routing decision as a whisper — only the chat_type differs (it rides
        // `type` into the seam). Reuse the whisper path verbatim: real-player
        // sender + bot↔bot barrier, narrative-owned receiver, and the G-LOOP-2
        // master pairing are all enforced there.
        return TryForwardWhisper(fromPlayer, type, msg, member);
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

    bool NarrativeCompanion::DeliverEmote(uint32 botGuidLow, uint32 masterGuidLow, uint32 emoteId)
    {
        // World thread only from here on: live Player* resolution.
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuidLow));
        if (!bot)
        {
            LOG_DEBUG("playerbots", "Chronicle narrative: dropped emote — bot {} is not in world", botGuidLow);
            return false;
        }

        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!IsNarrativeOwnedBot(botAI))
        {
            LOG_WARN("playerbots",
                     "Chronicle narrative: refused emote — bot {} is not a narrative-flagged owned companion",
                     botGuidLow);
            return false;
        }

        // G-LOOP-2, mirrored from DeliverReply: the emote is a companion action for
        // its own master's context only.
        Player* master = botAI->GetMaster();
        if (!master || master->GetGUID().GetCounter() != masterGuidLow)
        {
            LOG_WARN("playerbots",
                     "Chronicle narrative: refused emote for bot {} — master mismatch "
                     "(addressed {}, bot serves {})",
                     botGuidLow, masterGuidLow, master ? master->GetGUID().GetCounter() : 0);
            return false;
        }

        // Native one-shot emote — the same EMOTE_ONESHOT_* mechanism the fork
        // already uses for bot emotes (a new trigger, not a new capability).
        bot->HandleEmoteCommand(emoteId);
        return true;
    }

    bool NarrativeCompanion::DeliverCreatureReply(uint32 creatureEntry, uint32 playerGuidLow,
                                                  std::string const& text, uint32 emoteId)
    {
        if (text.empty())
            return false;

        // World thread only from here on: live Player*/Creature* resolution.
        //
        // A voiced-NPC say-back is always a reply to a real player who just spoke
        // to the creature, so the asker is the natural anchor for resolving the
        // live creature: FindNearestCreature does a bounded grid search around the
        // player (cheap) instead of scanning a whole continent's creature store by
        // entry (what the retired O(registry) Lua poll did). The producer always
        // sends the asker; a 0 guid leaves us no anchor, so we drop the line
        // (logged) rather than guess where to place it.
        Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(playerGuidLow));
        if (!player)
        {
            // Timing, not an attack (asker changed zone / logged out between the
            // service write and this drain).
            LOG_DEBUG("playerbots",
                      "Chronicle narrative: dropped creature reply — asker {} not in world (entry {})",
                      playerGuidLow, creatureEntry);
            return false;
        }

        // Resolve the live creature near the asker. 100y covers a player drifting
        // between speaking and the (<=5s) drain — say range is ~25y — while a
        // stationary voiced NPC the player just addressed is virtually always in.
        Creature* creature = player->FindNearestCreature(creatureEntry, 100.0f);
        if (!creature)
        {
            LOG_DEBUG("playerbots",
                      "Chronicle narrative: dropped creature reply — entry {} not found near asker {}",
                      creatureEntry, playerGuidLow);
            return false;
        }

        // Best-effort: turn the creature to face the asker before it speaks.
        creature->SetFacingToObject(player);

        // Native creature speech — the same Unit::Say the fork already uses for bot
        // chatter (a new trigger, not a new capability). LANG_UNIVERSAL: voiced
        // NPCs are faction-neutral (no Common/Orcish split). The text keeps its
        // accented French + the ✦ glyph (utf8mb4 seam → raw bytes → Say).
        creature->Say(text, LANG_UNIVERSAL);

        // Optional one-shot emote alongside the line (EMOTE_ONESHOT_* anim id,
        // validated service-side against NarrativeService.Capabilities).
        if (emoteId > 0)
            creature->HandleEmoteCommand(emoteId);

        return true;
    }

    void NarrativeCompanion::CaptureChannelMessage(Player* sender, std::string const& channelName,
                                                   std::string const& msg)
    {
        if (!sender || msg.empty() || !s_channelSink)
            return;

        // Only REAL players are ever captured — a bot's channel chatter must never
        // enter the narrative path (anti bot↔bot / anti-cost).
        PlayerbotAI* const fromAI = PlayerbotsMgr::instance().GetPlayerbotAI(sender);
        if (fromAI && !fromAI->IsRealPlayer())
            return;

        ChannelMessage m;
        m.senderGuid = sender->GetGUID().GetCounter();
        m.channel = channelName;
        m.text = msg;

        // Fire-and-forget: the sink hands off to a thread-safe queue (async INSERT)
        // and returns promptly. Never suppresses the channel line.
        s_channelSink(m);
    }

    bool NarrativeCompanion::DeliverChannelResponse(uint32 botGuidLow, uint32 targetGuidLow,
                                                    std::string const& text)
    {
        if (text.empty())
            return false;

        // World thread only from here on: live Player* resolution.
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuidLow));
        if (!bot)
        {
            LOG_DEBUG("playerbots", "Chronicle narrative: dropped channel reply — bot {} not in world", botGuidLow);
            return false;
        }

        // The responder must be an actual bot (matchmaking elects from the bot pool).
        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI || botAI->IsRealPlayer())
        {
            LOG_WARN("playerbots", "Chronicle narrative: refused channel reply — {} is not a bot", botGuidLow);
            return false;
        }

        Player* target = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(targetGuidLow));
        if (!target)
        {
            LOG_DEBUG("playerbots", "Chronicle narrative: dropped channel reply — target {} not online",
                      targetGuidLow);
            return false;
        }

        // Anti bot↔bot: never whisper another bot.
        PlayerbotAI* const targetAI = PlayerbotsMgr::instance().GetPlayerbotAI(target);
        if (targetAI && !targetAI->IsRealPlayer())
        {
            LOG_WARN("playerbots", "Chronicle narrative: refused channel reply — target {} is a bot",
                     targetGuidLow);
            return false;
        }

        // Native bot→player whisper — the matchmaking response (Player::Whisper).
        bot->Whisper(text, LANG_UNIVERSAL, target);
        return true;
    }

    bool NarrativeCompanion::IsWhitelistedVerb(std::string const& verb)
    {
        return std::any_of(kWhitelistedVerbs.begin(), kWhitelistedVerbs.end(),
                           [&](char const* v) { return verb == v; });
    }
}  // namespace Chronicle
