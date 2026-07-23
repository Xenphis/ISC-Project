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

#ifndef TRINITY_COMPANIONAI_H
#define TRINITY_COMPANIONAI_H

#include "CreatureAI.h"

class Player;

class TC_GAME_API CompanionAI : public CreatureAI
{
    public:
        explicit CompanionAI(Creature* creature);

        static int32 Permissible(Creature const* /*creature*/) { return PERMIT_BASE_NO; }

        void UpdateAI(uint32 diff) override;
        void AttackStart(Unit* /*victim*/) override { }
        void MoveInLineOfSight(Unit* /*who*/) override { }
        void JustAppeared() override;
        void JustDied(Unit* /*killer*/) override;

        bool OnGossipHello(Player* player) override;
        bool OnGossipSelect(Player* player, uint32 menuId, uint32 gossipListId) override;

        bool IsRecruited() const { return !_ownerGuid.IsEmpty(); }
        ObjectGuid GetOwnerGuid() const { return _ownerGuid; }
        void StartFollowing(Player* player);
        void StopFollowing();   // walks back home

    private:
        void NotifyDismissed();

        ObjectGuid _ownerGuid;
        uint32 _checkTimer;
};

#endif // TRINITY_COMPANIONAI_H
