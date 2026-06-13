-- #########################################################
-- Chronicle D027 (amendement 2026-06-13) — companion seam,
-- inbound whispers + outbound replies (Hybride architecture).
--
-- Completes the bidirectional companion seam started by
-- 2026_06_12_00_chronicle_narrative_seam.sql:
--
--   * chronicle_narrative_bot_whispers — INBOUND. The worldserver
--     WRITES (NarrativeCompanion forward sink, on a master→companion
--     whisper); narrative_service READS + DELETES (at-most-once
--     drain). Carries the master's raw NL text to the LLM.
--   * chronicle_narrative_bot_replies — OUTBOUND say-back.
--     narrative_service WRITES the in-character reply; the worldserver
--     READS + DELETES (NarrativeBridge drain) and delivers it via
--     botAI->TellMaster. Symmetric to chronicle_narrative_bot_commands.
--
-- Same DB-as-control-plane model as the canonical ai_playerbot_texts
-- seam (Chronicle invariant 3). No core patch.
-- #########################################################

CREATE TABLE IF NOT EXISTS `chronicle_narrative_bot_whispers` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'companion guid (low) the master whispered',
  `master_guid` INT UNSIGNED NOT NULL COMMENT 'master guid (low) who sent the whisper',
  `chat_type` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'raw CHAT_MSG_* enum',
  `text` VARCHAR(512) NOT NULL COMMENT 'master raw whisper text (NL)',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS `chronicle_narrative_bot_replies` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'companion guid (low) that speaks',
  `master_guid` INT UNSIGNED NOT NULL COMMENT 'master guid (low) the reply is whispered to',
  `text` VARCHAR(512) NOT NULL COMMENT 'in-character reply text (chat-safe / accent-free)',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;
