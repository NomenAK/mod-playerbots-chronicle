-- #########################################################
-- Chronicle D027 — narrative companion seam tables (bridge)
--
-- Control plane for the bot-as-narrative-agent category
-- (D027 ADR, amendement 2026-06-12 — seam bridge):
-- narrative_service WRITES these tables, the worldserver
-- READS them (NarrativeBridge async poll, applied on the
-- world thread). Same DB-as-control-plane model as the
-- canonical ai_playerbot_texts seam (Chronicle invariant 3).
--
--   * chronicle_narrative_bots — guid-low registry of
--     narrative-flagged player-owned companions. Reconciled
--     by diff into the in-memory facade registry; deleting a
--     row unflags the bot on the next poll.
--   * chronicle_narrative_bot_commands — cleared bot_action
--     decisions (whitelisted verb + native command string),
--     last hop of the decision-poll path. Drained
--     at-most-once; the verb whitelist is re-checked
--     C++-side (defense in depth) before HandleCommand.
-- #########################################################

CREATE TABLE IF NOT EXISTS `chronicle_narrative_bots` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'bot character guid (low)',
  `note` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'free-form provenance (persona/master)',
  `flagged_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS `chronicle_narrative_bot_commands` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'companion character guid (low)',
  `master_guid` INT UNSIGNED NOT NULL COMMENT 'master guid (low) the command was cleared for',
  `verb` VARCHAR(32) NOT NULL COMMENT 'whitelisted toolkit verb (NarrativeService.Toolkit)',
  `command` VARCHAR(255) NOT NULL COMMENT 'native playerbot command string',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;
