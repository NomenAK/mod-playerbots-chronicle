-- #########################################################
-- Chronicle D030 Phase 1 — creature reply seam (voiced-NPC say-back).
--
-- Unifies the voiced-creature speech path onto the SAME async DB seam
-- the companion already uses (chronicle_narrative_bot_replies), retiring
-- the legacy HTTP decision-poll (chronicle_apply_decision.lua's
-- O(registry) /api/decisions/poll loop that crash-looped the world
-- thread at the 81-NPC roster cold boot — see
-- docs/handoffs/2026-06-15-roster-extend-boot-hang-revert.md).
--
--   chronicle_narrative_creature_replies — OUTBOUND voiced-NPC speech.
--     narrative_service WRITES the in-character line; the worldserver
--     READS + DELETES (NarrativeBridge::DrainCreatureReplies) and
--     delivers it via the creature's native Unit::Say (LANG_UNIVERSAL),
--     optionally facing the asking player + playing a one-shot emote.
--     Symmetric to chronicle_narrative_bot_replies, but keyed by
--     creature_entry/player_guid (a creature is not a Player*).
--
-- Charset: utf8mb4 (NOT utf8mb3 like the companion sibling tables).
-- Companion replies are chat-safe'd to ASCII before the seam; voiced-NPC
-- lines keep their accented French + the ✦ identity glyph (U+2726), so
-- the column must carry the full UTF-8 range losslessly. The Elixir
-- writer connects with charset=utf8mb4 to match (see runtime.exs).
--
-- Same DB-as-control-plane model as the canonical ai_playerbot_texts
-- seam (Chronicle invariant 3). No core patch.
-- #########################################################

CREATE TABLE IF NOT EXISTS `chronicle_narrative_creature_replies` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `creature_entry` INT UNSIGNED NOT NULL COMMENT 'voiced creature template entry that speaks',
  `player_guid` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'asking player guid (low); 0 = no facing',
  `text` VARCHAR(512) NOT NULL COMMENT 'in-character line (accented FR + ✦ preserved)',
  `emote_id` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'one-shot EMOTE_ONESHOT_* anim id; 0 = none',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
