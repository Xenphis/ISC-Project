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

#ifndef TRINITY_COMPANIONMGR_H
#define TRINITY_COMPANIONMGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Player;
class Unit;

constexpr uint8 MAX_COMPANIONS = 4;

class TC_GAME_API CompanionMgr
{
    public:
        static CompanionMgr* instance();

        void LoadTemplates();

        bool IsCompanion(uint32 creatureEntry) const { return _companionTemplates.contains(creatureEntry); }
        bool Recruit(Player* player, Creature* creature);
        void DismissAll(Player* player);
        std::vector<ObjectGuid> GetCompanions(ObjectGuid playerGuid) const;

        // Companions are not part of Player::m_Controlled, so the core sites that notify
        // pets of their owner's combat (Unit::DealDamage, Unit::Attack, Spell::prepare)
        // reach them through here instead.
        void AssistOwner(Player* owner, Unit* target);

        // Called by CompanionAI when a companion stops following (dismiss, death, owner gone)
        void OnCompanionDismissed(ObjectGuid playerGuid, ObjectGuid companionGuid);

    private:
        std::unordered_set<uint32> _companionTemplates;
        std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> _activeCompanions;
};

#define sCompanionMgr CompanionMgr::instance()

#endif // TRINITY_COMPANIONMGR_H
