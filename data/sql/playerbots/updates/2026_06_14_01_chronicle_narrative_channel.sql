-- #########################################################
-- Chronicle Beta Spec 09 (social) — global-channel matchmaking seam.
--
--   * chronicle_narrative_channel_messages — INBOUND. The worldserver WRITES a
--     captured real-player channel message (NarrativeCompanion channel sink, only
--     when Chronicle.Channels.Capture is on); narrative_service READS + DELETES
--     (matchmaking drain). Carries the LFG/recruitment text for the pre-filter.
--   * chronicle_narrative_channel_responses — OUTBOUND. narrative_service WRITES
--     the elected bot's response; the worldserver READS + DELETES
--     (NarrativeBridge::DrainChannelResponses) and delivers it via
--     NarrativeCompanion::DeliverChannelResponse → Player::Whisper (bot → asker).
--
-- Same DB-as-control-plane model as the canonical ai_playerbot_texts seam
-- (Chronicle invariant 3). No core patch. Inert unless channel capture is enabled.
-- #########################################################

CREATE TABLE IF NOT EXISTS `chronicle_narrative_channel_messages` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `sender_guid` INT UNSIGNED NOT NULL COMMENT 'real-player sender guid (low)',
  `channel` VARCHAR(64) NOT NULL COMMENT 'channel name (e.g. LookingForGroup)',
  `text` VARCHAR(512) NOT NULL COMMENT 'channel message text',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS `chronicle_narrative_channel_responses` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid` INT UNSIGNED NOT NULL COMMENT 'elected bot guid (low) that responds',
  `target_guid` INT UNSIGNED NOT NULL COMMENT 'asker guid (low) the bot whispers',
  `text` VARCHAR(512) NOT NULL COMMENT 'response text (chat-safe / accent-free)',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;
