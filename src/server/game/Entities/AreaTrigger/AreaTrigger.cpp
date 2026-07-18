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

#include "AreaTrigger.h"
#include "AreaTriggerAI.h"
#include "AreaTriggerMgr.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Unit.h"

constexpr uint32 AREATRIGGER_CHECK_INTERVAL = 250;      // ms between two unit enter/exit scans

AreaTrigger::AreaTrigger() : WorldObject(false), _template(nullptr), _caster(nullptr), _spellId(0),
    _duration(-1), _checkTimer(0), _isRemoved(false)
{
    m_objectType |= TYPEMASK_AREATRIGGER;
    m_objectTypeId = TYPEID_AREATRIGGER;

    // never sent to clients, no update fields beyond the Object base block
    m_updateFlag = 0;
    m_valuesCount = OBJECT_END;
}

AreaTrigger::~AreaTrigger()
{
    ASSERT(!_caster);
}

void AreaTrigger::AddToWorld()
{
    ///- Register the areatrigger for guid lookup and for caster
    if (!IsInWorld())
    {
        GetMap()->GetObjectsStore().Insert<AreaTrigger>(GetGUID(), this);
        WorldObject::AddToWorld();
        if (!_casterGuid.IsEmpty())
            BindToCaster();
    }
}

void AreaTrigger::RemoveFromWorld()
{
    ///- Remove the areatrigger from the accessor and from all lists of objects in world
    if (IsInWorld())
    {
        _isRemoved = true;
        if (_caster)
            UnbindFromCaster();
        WorldObject::RemoveFromWorld();
        GetMap()->GetObjectsStore().Remove<AreaTrigger>(GetGUID());
    }
}

AreaTrigger* AreaTrigger::CreateAreaTrigger(uint32 entry, Map* map, Position const& pos, int32 duration, Unit* caster /*= nullptr*/, uint32 spellId /*= 0*/, bool staticSpawn /*= false*/)
{
    AreaTrigger* at = new AreaTrigger();
    if (!at->Create(entry, map, pos, duration, caster, spellId, staticSpawn))
    {
        delete at;
        return nullptr;
    }

    return at;
}

bool AreaTrigger::Create(uint32 entry, Map* map, Position const& pos, int32 duration, Unit* caster, uint32 spellId, bool staticSpawn)
{
    _template = sAreaTriggerMgr->GetAreaTriggerTemplate(entry);
    if (!_template)
    {
        TC_LOG_ERROR("entities.areatrigger", "AreaTrigger (entry {}) not created. Missing template.", entry);
        return false;
    }

    SetMap(map);
    Relocate(pos);
    if (!IsPositionValid())
    {
        TC_LOG_ERROR("entities.areatrigger", "AreaTrigger (entry {}) not created. Suggested coordinates aren't valid (X: {} Y: {})", entry, GetPositionX(), GetPositionY());
        return false;
    }

    WorldObject::_Create(ObjectGuid::Create<HighGuid::AreaTrigger>(map->GenerateLowGuid<HighGuid::AreaTrigger>()));
    SetPhaseMask(caster ? caster->GetPhaseMask() : uint32(PHASEMASK_NORMAL), false);

    SetEntry(entry);
    SetObjectScale(1.0f);

    if (caster)
    {
        _casterGuid = caster->GetGUID();
        _spellId = spellId;
    }
    _duration = duration;

    _ai = std::make_unique<NullAreaTriggerAI>(this);

    if (staticSpawn)
        setActive(true);    //must before add to map to keep the grid loaded

    if (!GetMap()->AddToMap(this))
        return false;

    _ai->OnCreate();

    return true;
}

void AreaTrigger::Update(uint32 diff)
{
    if (_duration >= 0)
    {
        if (_duration > int32(diff))
            _duration -= diff;
        else
        {
            Remove();
            return;
        }
    }

    if (_checkTimer <= diff)
    {
        _checkTimer = AREATRIGGER_CHECK_INTERVAL;
        UpdateTargetList();
        if (_isRemoved)
            return;
    }
    else
        _checkTimer -= diff;

    _ai->OnUpdate(diff);
}

void AreaTrigger::Remove()
{
    if (!IsInWorld() || _isRemoved)
        return;

    _isRemoved = true;
    _ai->OnRemove();
    RemoveFromWorld();
    AddObjectToRemoveList();
}

void AreaTrigger::UpdateTargetList()
{
    std::vector<Unit*> targets;
    float radius = _template->Shape.GetMaxSearchRadius();
    Trinity::AnyUnitInObjectRangeCheck check(this, radius);
    Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(this, targets, check);
    Cell::VisitAllObjects(this, searcher, radius);

    if (_template->Shape.Type == AreaTriggerShapeType::Box)
        std::erase_if(targets, [this](Unit const* unit) { return !CheckIsInBox(unit); });

    HandleUnitEnterExit(targets);
}

void AreaTrigger::HandleUnitEnterExit(std::vector<Unit*> const& targets)
{
    GuidUnorderedSet exitUnits(std::move(_insideUnits));
    _insideUnits.clear();

    std::vector<Unit*> enteringUnits;
    for (Unit* unit : targets)
    {
        if (!exitUnits.erase(unit->GetGUID())) // erase(key) returns 0 if the unit was not inside before
            enteringUnits.push_back(unit);

        _insideUnits.insert(unit->GetGUID());
    }

    for (Unit* unit : enteringUnits)
    {
        _ai->OnUnitEnter(unit);
        if (_isRemoved)
            return;
    }

    for (ObjectGuid const& exitUnitGuid : exitUnits)
    {
        if (Unit* leavingUnit = ObjectAccessor::GetUnit(*this, exitUnitGuid))
        {
            _ai->OnUnitExit(leavingUnit);
            if (_isRemoved)
                return;
        }
    }
}

bool AreaTrigger::CheckIsInBox(Unit const* unit) const
{
    float dx = unit->GetPositionX() - GetPositionX();
    float dy = unit->GetPositionY() - GetPositionY();
    float dz = unit->GetPositionZ() - GetPositionZ();

    // rotate the delta into the trigger-local frame
    float cosO = std::cos(GetOrientation());
    float sinO = std::sin(GetOrientation());
    float localX = dx * cosO + dy * sinO;
    float localY = -dx * sinO + dy * cosO;

    return std::fabs(localX) <= _template->Shape.Data[0]
        && std::fabs(localY) <= _template->Shape.Data[1]
        && std::fabs(dz) <= _template->Shape.Data[2];
}

uint32 AreaTrigger::GetFaction() const
{
    if (_caster)
        return _caster->GetFaction();

    return 0;
}

void AreaTrigger::BindToCaster()
{
    ASSERT(!_caster);
    _caster = ObjectAccessor::GetUnit(*this, _casterGuid);
    ASSERT(_caster);
    ASSERT(_caster->GetMap() == GetMap());
    _caster->_RegisterAreaTrigger(this);
}

void AreaTrigger::UnbindFromCaster()
{
    ASSERT(_caster);
    _caster->_UnregisterAreaTrigger(this);
    _caster = nullptr;
}
