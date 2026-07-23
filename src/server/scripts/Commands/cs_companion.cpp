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

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CompanionAI.h"
#include "CompanionMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "WorldSession.h"

using namespace Trinity::ChatCommands;

class companion_commandscript : public CommandScript
{
public:
    companion_commandscript() : CommandScript("companion_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable companionCommandTable =
        {
            { "add",     HandleCompanionAddCommand,     rbac::RBAC_PERM_COMMAND_COMPANION, Console::No },
            { "dismiss", HandleCompanionDismissCommand, rbac::RBAC_PERM_COMMAND_COMPANION, Console::No },
            { "clear",   HandleCompanionClearCommand,   rbac::RBAC_PERM_COMMAND_COMPANION, Console::No },
            { "list",    HandleCompanionListCommand,    rbac::RBAC_PERM_COMMAND_COMPANION, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "companion", companionCommandTable },
        };
        return commandTable;
    }

    // Recruits the selected creature (must be listed in `companion_template`)
    static bool HandleCompanionAddCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Creature* creature = handler->getSelectedCreature();
        if (!creature)
        {
            handler->PSendSysMessage("Select a creature first.");
            return false;
        }

        if (!sCompanionMgr->IsCompanion(creature->GetEntry()))
        {
            handler->PSendSysMessage("%s (entry %u) is not listed in companion_template.", creature->GetName().c_str(), creature->GetEntry());
            return false;
        }

        if (!sCompanionMgr->Recruit(player, creature))
        {
            handler->PSendSysMessage("Cannot recruit %s: already recruited, dead, missing CompanionAI, or you reached the limit (%u).", creature->GetName().c_str(), uint32(MAX_COMPANIONS));
            return false;
        }

        handler->PSendSysMessage("%s is now following you.", creature->GetName().c_str());
        return true;
    }

    // Dismisses the selected companion
    static bool HandleCompanionDismissCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Creature* creature = handler->getSelectedCreature();
        if (!creature)
        {
            handler->PSendSysMessage("Select a companion first.");
            return false;
        }

        CompanionAI* ai = dynamic_cast<CompanionAI*>(creature->AI());
        if (!ai || ai->GetOwnerGuid() != player->GetGUID())
        {
            handler->PSendSysMessage("%s is not following you.", creature->GetName().c_str());
            return false;
        }

        ai->StopFollowing();
        handler->PSendSysMessage("%s stops following you.", creature->GetName().c_str());
        return true;
    }

    static bool HandleCompanionClearCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        sCompanionMgr->DismissAll(player);
        handler->PSendSysMessage("All companions dismissed.");
        return true;
    }

    static bool HandleCompanionListCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        std::vector<ObjectGuid> companions = sCompanionMgr->GetCompanions(player->GetGUID());
        if (companions.empty())
        {
            handler->PSendSysMessage("No companion is following you.");
            return true;
        }

        handler->PSendSysMessage("Companions (%u/%u):", uint32(companions.size()), uint32(MAX_COMPANIONS));
        for (ObjectGuid guid : companions)
        {
            Creature* creature = ObjectAccessor::GetCreature(*player, guid);
            handler->PSendSysMessage("  %s", creature ? creature->GetName().c_str() : "<not on this map>");
        }
        return true;
    }
};

void AddSC_companion_commandscript()
{
    new companion_commandscript();
}
