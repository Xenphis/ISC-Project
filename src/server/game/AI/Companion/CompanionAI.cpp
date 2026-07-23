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

#include "CompanionAI.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "GossipDef.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "ObjectAccessor.h"
#include "PetDefines.h"
#include "Player.h"
#include "ScriptedGossip.h"

namespace
{
    constexpr uint32 COMPANION_CHECK_INTERVAL = 1000;
    constexpr float COMPANION_TELEPORT_DIST = 60.0f;
}

CompanionAI::CompanionAI(Creature* creature) : CreatureAI(creature), _checkTimer(0)
{
    creature->SetReactState(REACT_PASSIVE);
}

void CompanionAI::JustAppeared()
{
    CreatureAI::JustAppeared();
    // Covers both initial spawn and respawn after death: always start unrecruited
    NotifyDismissed();
}

void CompanionAI::JustDied(Unit* /*killer*/)
{
    NotifyDismissed();
}

void CompanionAI::NotifyDismissed()
{
    if (_ownerGuid.IsEmpty())
        return;

    sCompanionMgr->OnCompanionDismissed(_ownerGuid, me->GetGUID());
    _ownerGuid.Clear();
}

void CompanionAI::StartFollowing(Player* player)
{
    _ownerGuid = player->GetGUID();
    // Follow angle spread by companion count so multiple companions don't stack
    float angle = float(M_PI_2) + float(me->GetGUID().GetCounter() % 4) * float(M_PI_4);
    me->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, ChaseAngle(angle));
}

void CompanionAI::StopFollowing()
{
    NotifyDismissed();
    me->GetMotionMaster()->Clear();
    me->GetMotionMaster()->MoveTargetedHome();
}

void CompanionAI::UpdateAI(uint32 diff)
{
    if (!IsRecruited())
        return;

    if (_checkTimer > diff)
    {
        _checkTimer -= diff;
        return;
    }
    _checkTimer = COMPANION_CHECK_INTERVAL;

    // Map-local lookup: fails when the owner logged out or changed map,
    // in which case the companion dismisses itself and walks home.
    Player* owner = ObjectAccessor::GetPlayer(*me, _ownerGuid);
    if (!owner)
    {
        StopFollowing();
        return;
    }

    if (!me->IsAlive())
        return;

    if (!me->IsWithinDist(owner, COMPANION_TELEPORT_DIST))
        me->NearTeleportTo(owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(), owner->GetOrientation());

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        StartFollowing(owner);
}

bool CompanionAI::OnGossipHello(Player* player)
{
    if (!sCompanionMgr->IsCompanion(me->GetEntry()))
        return false;   // fall back to default database gossip

    if (GetOwnerGuid() == player->GetGUID())
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "You can leave now.", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
    else if (!IsRecruited())
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Follow me.", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);

    SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, me);
    return true;
}

bool CompanionAI::OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId)
{
    if (gossipListId != 0)
        return false;

    CloseGossipMenuFor(player);

    if (GetOwnerGuid() == player->GetGUID())
        StopFollowing();
    else if (!IsRecruited())
        sCompanionMgr->Recruit(player, me);

    return true;
}
