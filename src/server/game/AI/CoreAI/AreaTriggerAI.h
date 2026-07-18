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

#ifndef TRINITYCORE_AREATRIGGER_AI_H
#define TRINITYCORE_AREATRIGGER_AI_H

#include "Define.h"

class AreaTrigger;
class Unit;

class TC_GAME_API AreaTriggerAI
{
    protected:
        AreaTrigger* const at;

    public:
        explicit AreaTriggerAI(AreaTrigger* a);
        virtual ~AreaTriggerAI();

        virtual void OnCreate() { }
        virtual void OnUpdate(uint32 /*diff*/) { }
        virtual void OnUnitEnter(Unit* /*unit*/) { }
        virtual void OnUnitExit(Unit* /*unit*/) { }
        virtual void OnRemove() { }
};

class NullAreaTriggerAI : public AreaTriggerAI
{
    public:
        using AreaTriggerAI::AreaTriggerAI;
};

#endif // TRINITYCORE_AREATRIGGER_AI_H
