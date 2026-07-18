/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITYCORE_AREATRIGGER_MGR_H
#define TRINITYCORE_AREATRIGGER_MGR_H

#include "AreaTriggerTemplate.h"
#include <unordered_map>
#include <vector>

class Map;

class TC_GAME_API AreaTriggerMgr
{
    private:
        AreaTriggerMgr() = default;
        ~AreaTriggerMgr() = default;

    public:
        static AreaTriggerMgr* instance();

        void LoadAreaTriggerTemplates();
        void LoadSpellAreaTriggers();
        void LoadAreaTriggerSpawns();

        AreaTriggerTemplate const* GetAreaTriggerTemplate(uint32 id) const;
        std::vector<SpellAreaTrigger> const* GetSpellAreaTriggers(uint32 spellId) const;

        // spawns the static (DB-placed) areatriggers of a freshly created map
        void SpawnStaticAreaTriggers(Map* map);

    private:
        std::unordered_map<uint32, AreaTriggerTemplate> _templates;
        std::unordered_map<uint32, std::vector<SpellAreaTrigger>> _spellAreaTriggers;
        std::unordered_map<uint32 /*mapId*/, std::vector<AreaTriggerSpawn>> _spawns;
};

#define sAreaTriggerMgr AreaTriggerMgr::instance()

#endif // TRINITYCORE_AREATRIGGER_MGR_H
