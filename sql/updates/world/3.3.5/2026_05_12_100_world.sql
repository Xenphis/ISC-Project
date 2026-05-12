--
-- Add phaseMask column to `transports` table to allow spawning continent
-- transports in a specific phase (cherry-picked from TC master 2d7af28e).
--
ALTER TABLE `transports`
    ADD COLUMN `phaseMask` INT UNSIGNED NOT NULL DEFAULT 1 AFTER `entry`;
