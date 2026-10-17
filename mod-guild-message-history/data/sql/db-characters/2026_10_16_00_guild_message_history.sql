-- Guild Message History module
-- This file lives in the CHARACTERS database update path.
-- AzerothCore applies it automatically on startup (modules/*/data/sql/db-characters/**).
-- If you installed a previous, incompatible version of this module, drop both tables first.

CREATE TABLE IF NOT EXISTS `guild_message_history` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guild_id` INT UNSIGNED NOT NULL,
    `sender_guid` INT UNSIGNED NOT NULL,
    `sender_name` VARCHAR(12) NOT NULL,
    `message` VARCHAR(255) NOT NULL,
    `sent_at` BIGINT UNSIGNED NOT NULL COMMENT 'Unix timestamp (UTC)',
    PRIMARY KEY (`id`),
    KEY `idx_guild_sent_at` (`guild_id`, `sent_at`),
    KEY `idx_sent_at` (`sent_at`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS `guild_message_history_state` (
    `guid` INT UNSIGNED NOT NULL,
    `last_logout` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Unix timestamp (UTC) of the last replay checkpoint',
    PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
