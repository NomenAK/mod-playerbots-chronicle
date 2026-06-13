-- #########################################################
-- Chronicle D027 (amendement 2026-06-13, Vague 4) — companion
-- activation seam.
--
-- The NarrativeBridge discovery scan emits an event whenever a
-- bot gains or loses a REAL-player master (HasRealPlayerMaster —
-- the ownership signal now that the companion extends to ALL
-- player-controlled bots). The worldserver WRITES; narrative_service
-- READS + DELETES (at-most-once drain): on "gained" it resolves the
-- bot's deterministic persona and flags it (chronicle_narrative_bots)
-- BEFORE the master's first whisper (Tier-2); on "released" it
-- unflags it. master_guid is 0 for "released" (the bot/master may be
-- gone). class/race/gender/level/name feed the persona draw + render.
--
-- Same DB-as-control-plane model as the other chronicle_narrative_*
-- seam tables (Chronicle invariant 3). No core patch.
-- #########################################################

CREATE TABLE IF NOT EXISTS `chronicle_narrative_bot_activations` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'companion guid (low) that gained/lost a master',
  `master_guid` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'real-player master guid (low); 0 when released',
  `event` VARCHAR(16) NOT NULL COMMENT 'gained | released',
  `class` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '3.3.5a class id (persona draw)',
  `race` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '3.3.5a race id (persona draw)',
  `gender` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '3.3.5a gender id (0 male / 1 female)',
  `level` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bot level at activation (render hint)',
  `bot_name` VARCHAR(48) NOT NULL DEFAULT '' COMMENT 'bot character name',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;
