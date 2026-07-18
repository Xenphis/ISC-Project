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

#ifndef TRINITYCORE_AREATRIGGER_H
#define TRINITYCORE_AREATRIGGER_H

#include "Object.h"
#include "AreaTriggerTemplate.h"
#include "GridObject.h"
#include "MapObject.h"
#include <memory>

class AreaTriggerAI;
class Unit;

// Server-side only entity: the 3.3.5 client has no AreaTrigger object type,
// so this object is never sent to clients (see IsNeverVisible).
class TC_GAME_API AreaTrigger final : public WorldObject, public GridObject<AreaTrigger>, public MapObject
{
    public:
        AreaTrigger();
        ~AreaTrigger();

        void AddToWorld() override;
        void RemoveFromWorld() override;

        // caster may be nullptr (static spawns); returns nullptr on failure
        static AreaTrigger* CreateAreaTrigger(uint32 entry, Map* map, Position const& pos, int32 duration, Unit* caster = nullptr, uint32 spellId = 0, bool staticSpawn = false);

        void Update(uint32 diff) override;
        void Remove();

        bool IsNeverVisible(bool /*allowServersideObjects*/) const override { return true; }

        AreaTriggerTemplate const* GetTemplate() const { return _template; }
        uint32 GetScriptId() const { return _template->ScriptId; }
        uint32 GetSpellId() const { return _spellId; }
        int32 GetDuration() const { return _duration; }
        void SetDuration(int32 newDuration) { _duration = newDuration; }
        Unit* GetCaster() const { return _caster; }
        ObjectGuid GetCasterGUID() const { return _casterGuid; }
        ObjectGuid GetOwnerGUID() const override { return _casterGuid; }
        uint32 GetFaction() const override;
        GuidUnorderedSet const& GetInsideUnits() const { return _insideUnits; }
        AreaTriggerAI* AI() { return _ai.get(); }

    private:
        bool Create(uint32 entry, Map* map, Position const& pos, int32 duration, Unit* caster, uint32 spellId, bool staticSpawn);
        void UpdateTargetList();
        void HandleUnitEnterExit(std::vector<Unit*> const& targets);
        bool CheckIsInBox(Unit const* unit) const;
        void BindToCaster();
        void UnbindFromCaster();

        AreaTriggerTemplate const* _template;
        Unit* _caster;
        ObjectGuid _casterGuid;
        uint32 _spellId;
        int32 _duration;                                    // ms; -1 = permanent
        uint32 _checkTimer;
        bool _isRemoved;
        GuidUnorderedSet _insideUnits;
        std::unique_ptr<AreaTriggerAI> _ai;
};

#endif // TRINITYCORE_AREATRIGGER_H
