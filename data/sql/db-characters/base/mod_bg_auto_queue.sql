--
-- Table tracking characters that opted out from the automatic battleground queue performed by mod-bg-auto-queue
--
CREATE TABLE IF NOT EXISTS `mod_bg_auto_queue_optout` (
    `guid` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;
