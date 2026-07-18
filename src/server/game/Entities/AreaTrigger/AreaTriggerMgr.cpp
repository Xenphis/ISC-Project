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

#include "AreaTriggerMgr.h"
#include "AreaTrigger.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Timer.h"

AreaTriggerMgr* AreaTriggerMgr::instance()
{
    static AreaTriggerMgr instance;
    return &instance;
}

void AreaTriggerMgr::LoadAreaTriggerTemplates()
{
    uint32 oldMSTime = getMSTime();

    _templates.clear();

    //                                                 0   1          2           3           4           5           6
    QueryResult result = WorldDatabase.Query("SELECT Id, ShapeType, ShapeData0, ShapeData1, ShapeData2, ShapeData3, ScriptName FROM areatrigger_template");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 areatrigger templates. DB table `areatrigger_template` is empty.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        AreaTriggerTemplate atTemplate;
        atTemplate.Id = fields[0].GetUInt32();

        uint8 shapeType = fields[1].GetUInt8();
        if (shapeType >= uint8(AreaTriggerShapeType::Max))
        {
            TC_LOG_ERROR("sql.sql", "Table `areatrigger_template` has areatrigger (Id: {}) with invalid ShapeType {}, skipped.", atTemplate.Id, shapeType);
            continue;
        }

        atTemplate.Shape.Type = AreaTriggerShapeType(shapeType);
        for (uint8 i = 0; i < atTemplate.Shape.Data.size(); ++i)
            atTemplate.Shape.Data[i] = fields[2 + i].GetFloat();

        if (!atTemplate.Shape.IsValid())
        {
            TC_LOG_ERROR("sql.sql", "Table `areatrigger_template` has areatrigger (Id: {}) with invalid shape data, skipped.", atTemplate.Id);
            continue;
        }

        atTemplate.ScriptId = sObjectMgr->GetScriptId(fields[6].GetString());

        _templates[atTemplate.Id] = atTemplate;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} areatrigger templates in {} ms", _templates.size(), GetMSTimeDiffToNow(oldMSTime));
}

void AreaTriggerMgr::LoadSpellAreaTriggers()
{
    uint32 oldMSTime = getMSTime();

    _spellAreaTriggers.clear();

    //                                                    0              1         2
    QueryResult result = WorldDatabase.Query("SELECT SpellId, AreaTriggerId, Duration FROM spell_areatrigger");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 spell areatriggers. DB table `spell_areatrigger` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 spellId = fields[0].GetUInt32();
        SpellAreaTrigger spellAreaTrigger;
        spellAreaTrigger.AreaTriggerId = fields[1].GetUInt32();
        spellAreaTrigger.Duration = fields[2].GetInt32();

        if (!sSpellMgr->GetSpellInfo(spellId))
        {
            TC_LOG_ERROR("sql.sql", "Table `spell_areatrigger` has entry for non-existing spell (SpellId: {}), skipped.", spellId);
            continue;
        }

        if (!GetAreaTriggerTemplate(spellAreaTrigger.AreaTriggerId))
        {
            TC_LOG_ERROR("sql.sql", "Table `spell_areatrigger` (SpellId: {}) references non-existing areatrigger template (AreaTriggerId: {}), skipped.", spellId, spellAreaTrigger.AreaTriggerId);
            continue;
        }

        _spellAreaTriggers[spellId].push_back(spellAreaTrigger);
        ++count;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} spell areatriggers in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void AreaTriggerMgr::LoadAreaTriggerSpawns()
{
    uint32 oldMSTime = getMSTime();

    _spawns.clear();

    //                                                    0              1      2          3          4          5            6
    QueryResult result = WorldDatabase.Query("SELECT SpawnId, AreaTriggerId, MapId, PositionX, PositionY, PositionZ, Orientation FROM areatrigger");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 areatrigger spawns. DB table `areatrigger` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        AreaTriggerSpawn spawn;
        spawn.SpawnId = fields[0].GetUInt32();
        spawn.AreaTriggerId = fields[1].GetUInt32();
        uint32 mapId = fields[2].GetUInt16();
        spawn.Location = WorldLocation(mapId, fields[3].GetFloat(), fields[4].GetFloat(), fields[5].GetFloat(), fields[6].GetFloat());

        if (!GetAreaTriggerTemplate(spawn.AreaTriggerId))
        {
            TC_LOG_ERROR("sql.sql", "Table `areatrigger` (SpawnId: {}) references non-existing areatrigger template (AreaTriggerId: {}), skipped.", spawn.SpawnId, spawn.AreaTriggerId);
            continue;
        }

        if (!sMapStore.LookupEntry(mapId))
        {
            TC_LOG_ERROR("sql.sql", "Table `areatrigger` (SpawnId: {}) has invalid MapId {}, skipped.", spawn.SpawnId, mapId);
            continue;
        }

        _spawns[mapId].push_back(spawn);
        ++count;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} areatrigger spawns in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

AreaTriggerTemplate const* AreaTriggerMgr::GetAreaTriggerTemplate(uint32 id) const
{
    auto itr = _templates.find(id);
    return itr != _templates.end() ? &itr->second : nullptr;
}

std::vector<SpellAreaTrigger> const* AreaTriggerMgr::GetSpellAreaTriggers(uint32 spellId) const
{
    auto itr = _spellAreaTriggers.find(spellId);
    return itr != _spellAreaTriggers.end() ? &itr->second : nullptr;
}

void AreaTriggerMgr::SpawnStaticAreaTriggers(Map* map)
{
    auto itr = _spawns.find(map->GetId());
    if (itr == _spawns.end())
        return;

    for (AreaTriggerSpawn const& spawn : itr->second)
        AreaTrigger::CreateAreaTrigger(spawn.AreaTriggerId, map, spawn.Location, -1, nullptr, 0, /*staticSpawn*/ true);
}
