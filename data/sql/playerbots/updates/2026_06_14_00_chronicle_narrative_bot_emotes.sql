-- #########################################################
-- Chronicle Beta Spec 09 (emote parity) — companion emote seam.
--
--   * chronicle_narrative_bot_emotes — OUTBOUND. narrative_service WRITES a
--     one-shot emote (EMOTE_ONESHOT_* anim id) for a companion to play; the
--     worldserver READS + DELETES (NarrativeBridge::DrainEmotes) and plays it via
--     NarrativeCompanion::DeliverEmote → Unit::HandleEmoteCommand. Symmetric to
--     chronicle_narrative_bot_replies; same at-most-once drain + TTL.
--
-- Same DB-as-control-plane model as the canonical ai_playerbot_texts seam
-- (Chronicle invariant 3). No core patch.
-- #########################################################

CREATE TABLE IF NOT EXISTS `chronicle_narrative_bot_emotes` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'companion guid (low) that emotes',
  `master_guid` INT UNSIGNED NOT NULL COMMENT 'master guid (low) the emote is addressed for',
  `emote_id` INT UNSIGNED NOT NULL COMMENT 'EMOTE_ONESHOT_* anim id (PerformEmote space)',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;
