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

#include "CompanionMgr.h"
#include "CompanionAI.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Timer.h"

CompanionMgr* CompanionMgr::instance()
{
    static CompanionMgr instance;
    return &instance;
}

void CompanionMgr::LoadTemplates()
{
    uint32 oldMSTime = getMSTime();
    _companionTemplates.clear();

    QueryResult result = WorldDatabase.Query("SELECT CreatureEntry FROM companion_template");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 companion templates. DB table `companion_template` is empty.");
        return;
    }

    do
    {
        _companionTemplates.insert((*result)[0].GetUInt32());
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} companion templates in {} ms", _companionTemplates.size(), GetMSTimeDiffToNow(oldMSTime));
}

bool CompanionMgr::Recruit(Player* player, Creature* creature)
{
    if (!IsCompanion(creature->GetEntry()) || !creature->IsAlive())
        return false;

    CompanionAI* ai = dynamic_cast<CompanionAI*>(creature->AI());
    if (!ai || ai->IsRecruited())
        return false;

    std::vector<ObjectGuid>& companions = _activeCompanions[player->GetGUID()];
    if (companions.size() >= MAX_COMPANIONS)
        return false;

    companions.push_back(creature->GetGUID());
    ai->StartFollowing(player);
    return true;
}

void CompanionMgr::DismissAll(Player* player)
{
    auto itr = _activeCompanions.find(player->GetGUID());
    if (itr == _activeCompanions.end())
        return;

    // StopFollowing calls back into OnCompanionDismissed, which erases from
    // the vector we are iterating: work on a copy.
    std::vector<ObjectGuid> companions = itr->second;
    for (ObjectGuid guid : companions)
        if (Creature* creature = ObjectAccessor::GetCreature(*player, guid))
            if (CompanionAI* ai = dynamic_cast<CompanionAI*>(creature->AI()))
                ai->StopFollowing();

    _activeCompanions.erase(player->GetGUID());
}

std::vector<ObjectGuid> CompanionMgr::GetCompanions(ObjectGuid playerGuid) const
{
    auto itr = _activeCompanions.find(playerGuid);
    return itr != _activeCompanions.end() ? itr->second : std::vector<ObjectGuid>();
}

void CompanionMgr::OnCompanionDismissed(ObjectGuid playerGuid, ObjectGuid companionGuid)
{
    auto itr = _activeCompanions.find(playerGuid);
    if (itr == _activeCompanions.end())
        return;

    std::erase(itr->second, companionGuid);
    if (itr->second.empty())
        _activeCompanions.erase(itr);
}
