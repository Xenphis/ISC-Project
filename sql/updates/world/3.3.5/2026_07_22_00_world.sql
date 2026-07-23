-- ISC companion system
CREATE TABLE `companion_template` (
  `CreatureEntry` int unsigned NOT NULL COMMENT 'creature_template entry',
  `Comment` varchar(255) DEFAULT NULL,
  PRIMARY KEY (`CreatureEntry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='ISC Companion System';

-- Example: make an existing NPC recruitable
-- UPDATE `creature_template` SET `AIName` = 'CompanionAI', `npcflag` = `npcflag` | 1 WHERE `entry` = @ENTRY;
-- INSERT INTO `companion_template` (`CreatureEntry`, `Comment`) VALUES (@ENTRY, 'My first companion');
