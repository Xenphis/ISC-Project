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

#ifndef TRINITYCORE_AREATRIGGER_TEMPLATE_H
#define TRINITYCORE_AREATRIGGER_TEMPLATE_H

#include "Define.h"
#include "Position.h"
#include <array>
#include <cmath>

enum class AreaTriggerShapeType : uint8
{
    Sphere = 0,
    Box    = 1,
    Max
};

struct AreaTriggerShape
{
    AreaTriggerShapeType Type = AreaTriggerShapeType::Sphere;
    // Sphere: [0] = radius | Box: [0] = half-extent X, [1] = half-extent Y, [2] = half-extent Z
    std::array<float, 4> Data = { };

    float GetMaxSearchRadius() const
    {
        switch (Type)
        {
            case AreaTriggerShapeType::Sphere:
                return Data[0];
            case AreaTriggerShapeType::Box:
                return std::sqrt(Data[0] * Data[0] + Data[1] * Data[1] + Data[2] * Data[2]);
            default:
                return 0.0f;
        }
    }

    bool IsValid() const
    {
        switch (Type)
        {
            case AreaTriggerShapeType::Sphere:
                return Data[0] > 0.0f;
            case AreaTriggerShapeType::Box:
                return Data[0] > 0.0f && Data[1] > 0.0f && Data[2] > 0.0f;
            default:
                return false;
        }
    }
};

struct AreaTriggerTemplate
{
    uint32 Id = 0;
    AreaTriggerShape Shape;
    uint32 ScriptId = 0;
};

struct SpellAreaTrigger
{
    uint32 AreaTriggerId = 0;
    int32 Duration = 0;                                     // ms; 0 = spell duration, -1 = infinite
};

struct AreaTriggerSpawn
{
    uint32 SpawnId = 0;
    uint32 AreaTriggerId = 0;
    WorldLocation Location;
};

#endif // TRINITYCORE_AREATRIGGER_TEMPLATE_H
