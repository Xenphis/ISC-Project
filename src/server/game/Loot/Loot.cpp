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

#include "Loot.h"
#include "GameTime.h"
#include "Group.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "LootPackets.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "Random.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include <algorithm>

 //
 // --------- LootItem ---------
 //

 // Constructor, copies most fields from LootStoreItem and generates random count
LootItem::LootItem(LootStoreItem const& li)
{
    itemid = li.itemid;
    LootListId = 0;
    conditions = li.conditions;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
    freeforall = proto && proto->HasFlag(ITEM_FLAG_MULTI_DROP);
    follow_loot_rules = !li.needs_quest || (proto && proto->HasFlag(ITEM_FLAGS_CU_FOLLOW_LOOT_RULES));

    needs_quest = li.needs_quest;

    randomPropertySeed = GenerateEnchSuffixFactor(itemid);
    randomPropertyId = GenerateItemRandomPropertyId(itemid);
    count = 0;
    is_looted = false;
    is_blocked = false;
    is_underthreshold = false;
    is_counted = false;
    rollWinnerGUID = ObjectGuid::Empty;
}

bool LootItem::AllowedForPlayer(Player const* player, ObjectGuid ownerGuid) const
{
    return AllowedForPlayer(player, false, ownerGuid);
}

bool LootItem::AllowedForPlayer(Player const* player, bool isGivenByMasterLooter) const
{
    return AllowedForPlayer(player, isGivenByMasterLooter, ObjectGuid::Empty);
}

// Basic checks for player/item compatibility - if false no chance to see the item in the loot
bool LootItem::AllowedForPlayer(Player const* player, bool isGivenByMasterLooter, ObjectGuid ownerGuid) const
{
    // DB conditions check
    if (!sConditionMgr->IsObjectMeetToConditions(const_cast<Player*>(player), conditions))
        return false;

    ItemTemplate const* pProto = sObjectMgr->GetItemTemplate(itemid);
    if (!pProto)
        return false;

    // not show loot for not own team
    if (pProto->HasFlag(ITEM_FLAG2_FACTION_HORDE) && player->GetTeam() != HORDE)
        return false;

    if (pProto->HasFlag(ITEM_FLAG2_FACTION_ALLIANCE) && player->GetTeam() != ALLIANCE)
        return false;

    // Master looter can see all items even if the character can't loot them
    if (!isGivenByMasterLooter && player->GetGroup() && player->GetGroup()->GetMasterLooterGuid() == player->GetGUID())
    {
        return true;
    }

    // Don't allow loot for players without profession or those who already know the recipe
    if (pProto->HasFlag(ITEM_FLAG_HIDE_UNUSABLE_RECIPE))
    {
        if (!player->HasSkill(pProto->GetRequiredSkill()))
            return false;

        for (ItemEffect const& itemEffect : pProto->Effects)
        {
            if (itemEffect.TriggerType != ITEM_SPELLTRIGGER_LEARN_SPELL_ID)
                continue;

            if (player->HasSpell(itemEffect.SpellID))
                return false;
        }
    }

    // Don't allow to loot soulbound recipes that the player has already learned
    if (pProto->GetClass() == ITEM_CLASS_RECIPE && pProto->GetBonding() == BIND_WHEN_PICKED_UP)
    {
        for (ItemEffect const& itemEffect : pProto->Effects)
        {
            if (itemEffect.TriggerType != ITEM_SPELLTRIGGER_LEARN_SPELL_ID)
                continue;

            if (player->HasSpell(itemEffect.SpellID))
                return false;
        }
    }

    if (needs_quest && !freeforall && player->GetGroup() && (player->GetGroup()->GetLootMethod() == GROUP_LOOT || player->GetGroup()->GetLootMethod() == ROUND_ROBIN) && !ownerGuid.IsEmpty() && ownerGuid != player->GetGUID())
        return false;

    // check quest requirements
    if (!pProto->HasFlag(ITEM_FLAGS_CU_IGNORE_QUEST_STATUS) && ((needs_quest || (pProto->GetStartQuest() && player->GetQuestStatus(pProto->GetStartQuest()) != QUEST_STATUS_NONE)) && !player->HasQuestForItem(itemid)))
        return false;

    return true;
}

void LootItem::AddAllowedLooter(Player const* player)
{
    allowedGUIDs.insert(player->GetGUID());
}

//
// ------- Loot Roll -------
//

// Send the roll to every player that can still take part in it
void LootRoll::SendStartRoll()
{
    ItemTemplate const* itemTemplate = ASSERT_NOTNULL(sObjectMgr->GetItemTemplate(m_lootItem->itemid));

    for (auto const& [playerGuid, roll] : m_rollVoteMap)
    {
        if (roll.Vote != RollVote::NotEmitedYet)
            continue;

        Player* player = ObjectAccessor::GetPlayer(m_map, playerGuid);
        if (!player)
            continue;

        uint8 voteMask = m_voteMask;
        // In NEED_BEFORE_GREED need is disabled for an item the player cannot use
        if (m_loot->GetLootMethod() == NEED_BEFORE_GREED && player->CanRollForItemInLFG(itemTemplate, m_map) != EQUIP_ERR_OK)
            voteMask &= ~ROLL_FLAG_TYPE_NEED;

        WorldPacket data(SMSG_LOOT_START_ROLL, (8 + 4 + 4 + 4 + 4 + 4 + 4 + 1));
        data << m_lootObject;                               // guid of the object being rolled for
        data << uint32(m_map->GetId());                     // 3.3.3 mapid
        data << uint32(m_lootListId);                       // itemslot
        data << uint32(m_lootItem->itemid);                 // the itemEntryId for the item that shall be rolled for
        data << uint32(m_lootItem->randomPropertySeed);     // randomSuffix
        data << uint32(m_lootItem->randomPropertyId);       // item random property ID
        data << uint32(m_lootItem->count);                  // items in stack
        data << uint32(Milliseconds(LOOT_ROLL_TIMEOUT).count()); // the countdown time to choose "need" or "greed"
        data << uint8(voteMask);                            // roll type mask

        player->SendDirectMessage(&data);
    }

    // Handle auto pass option
    for (auto const& [playerGuid, roll] : m_rollVoteMap)
    {
        if (roll.Vote != RollVote::Pass)
            continue;

        SendRoll(playerGuid, 128, ROLL_PASS, true);
    }
}

// Send all passed message
void LootRoll::SendAllPassed()
{
    WorldPacket data(SMSG_LOOT_ALL_PASSED, (8 + 4 + 4 + 4 + 4));
    data << m_lootObject;                                   // guid of the object being rolled for
    data << uint32(m_lootListId);                           // item loot slot
    data << uint32(m_lootItem->itemid);                     // the itemEntryId for the item that shall be rolled for
    data << uint32(m_lootItem->randomPropertyId);           // item random property ID
    data << uint32(m_lootItem->randomPropertySeed);         // item random suffix ID

    for (auto const& [playerGuid, roll] : m_rollVoteMap)
    {
        if (roll.Vote == RollVote::NotValid)
            continue;

        if (Player* player = ObjectAccessor::GetPlayer(m_map, playerGuid))
            player->SendDirectMessage(&data);
    }
}

// Send the roll of targetGuid to everyone taking part in it
void LootRoll::SendRoll(ObjectGuid const& targetGuid, uint8 rollNumber, uint8 rollType, bool autoPass)
{
    WorldPacket data(SMSG_LOOT_ROLL, (8 + 4 + 8 + 4 + 4 + 4 + 1 + 1 + 1));
    data << m_lootObject;                                   // guid of the object being rolled for
    data << uint32(m_lootListId);                           // slot
    data << targetGuid;
    data << uint32(m_lootItem->itemid);                     // the itemEntryId for the item that shall be rolled for
    data << uint32(m_lootItem->randomPropertySeed);         // randomSuffix
    data << uint32(m_lootItem->randomPropertyId);           // item random property ID
    data << uint8(rollNumber);                              // 0: "Need for: [item name]" > 127: "you passed on: [item name]"
    data << uint8(rollType);                                // 0: pass, 1: need, 2: greed, 3: disenchant
    data << uint8(autoPass);                                // 1: "You automatically passed on: %s because you cannot loot that item."

    for (auto const& [playerGuid, roll] : m_rollVoteMap)
    {
        if (roll.Vote == RollVote::NotValid)
            continue;

        if (Player* player = ObjectAccessor::GetPlayer(m_map, playerGuid))
            player->SendDirectMessage(&data);
    }
}

// Reveal every roll then announce the winner
void LootRoll::SendLootRollWon(ObjectGuid const& targetGuid, uint8 rollNumber, RollVote rollType)
{
    // Send roll values
    for (auto const& [playerGuid, roll] : m_rollVoteMap)
    {
        switch (roll.Vote)
        {
            case RollVote::Pass:
                break;
            case RollVote::NotEmitedYet:
            case RollVote::NotValid:
                SendRoll(playerGuid, 128, ROLL_PASS);
                break;
            default:
                SendRoll(playerGuid, roll.RollNumber, AsUnderlyingType(roll.Vote));
                break;
        }
    }

    WorldPacket data(SMSG_LOOT_ROLL_WON, (8 + 4 + 4 + 4 + 4 + 8 + 1 + 1));
    data << m_lootObject;                                   // guid of the object being rolled for
    data << uint32(m_lootListId);                           // slot
    data << uint32(m_lootItem->itemid);                     // the itemEntryId for the item that shall be rolled for
    data << uint32(m_lootItem->randomPropertySeed);         // randomSuffix
    data << uint32(m_lootItem->randomPropertyId);           // item random property ID
    data << targetGuid;                                     // guid of the player who won
    data << uint8(rollNumber);                              // rollnumber related to SMSG_LOOT_ROLL
    data << uint8(AsUnderlyingType(rollType));              // rollType related to SMSG_LOOT_ROLL

    for (auto const& [playerGuid, roll] : m_rollVoteMap)
    {
        if (roll.Vote == RollVote::NotValid)
            continue;

        if (Player* player = ObjectAccessor::GetPlayer(m_map, playerGuid))
            player->SendDirectMessage(&data);
    }
}

LootRoll::~LootRoll()
{
    if (m_isStarted)
        SendAllPassed();

    for (auto const& [playerGuid, roll] : m_rollVoteMap)
    {
        if (roll.Vote == RollVote::NotValid)
            continue;

        if (Player* player = ObjectAccessor::GetPlayer(m_map, playerGuid))
            player->RemoveLootRoll(this);
    }
}

// Try to start the group roll for the specified item (it may fail for quest item or any condition)
// If this method returns false the roll has to be removed from the container to avoid any problem
bool LootRoll::TryToStart(Map* map, Loot& loot, ObjectGuid const& lootObject, uint32 lootListId, uint32 enchantingSkill)
{
    if (!m_isStarted)
    {
        if (lootListId >= loot.items.size() + loot.quest_items.size())
            return false;

        m_map = map;

        // initialize the data needed for the roll
        m_lootItem = lootListId < loot.items.size() ? &loot.items[lootListId] : &loot.quest_items[lootListId - loot.items.size()];

        m_loot = &loot;
        m_lootObject = lootObject;
        m_lootListId = lootListId;
        m_lootItem->is_blocked = true;                          // block the item while rolling

        uint32 playerCount = 0;
        for (ObjectGuid const& allowedLooter : m_lootItem->GetAllowedLooters())
        {
            Player* plr = ObjectAccessor::GetPlayer(m_map, allowedLooter);
            if (!plr || !m_lootItem->AllowedForPlayer(plr))     // check if player meet the condition to be able to roll this item
            {
                m_rollVoteMap[allowedLooter].Vote = RollVote::NotValid;
                continue;
            }
            // initialize player vote map
            m_rollVoteMap[allowedLooter].Vote = plr->GetPassOnGroupLoot() ? RollVote::Pass : RollVote::NotEmitedYet;
            if (!plr->GetPassOnGroupLoot())
                plr->AddLootRoll(this);

            ++playerCount;
        }

        // initialize item prototype and check enchant possibilities for this group
        ItemTemplate const* itemTemplate = ASSERT_NOTNULL(sObjectMgr->GetItemTemplate(m_lootItem->itemid));
        m_voteMask = ROLL_ALL_TYPE_MASK;
        if (itemTemplate->HasFlag(ITEM_FLAG2_CAN_ONLY_ROLL_GREED))
            m_voteMask = RollMask(m_voteMask & ~ROLL_FLAG_TYPE_NEED);
        if (!itemTemplate->DisenchantID || itemTemplate->GetRequiredDisenchantSkill() > enchantingSkill)
            m_voteMask = RollMask(m_voteMask & ~ROLL_FLAG_TYPE_DISENCHANT);

        if (playerCount > 1)                                    // check if more than one player can loot this item
        {
            // start the roll
            SendStartRoll();
            m_endTime = GameTime::Now() + LOOT_ROLL_TIMEOUT;
            m_isStarted = true;
            return true;
        }
        // no need to start roll if one or less player can loot this item so place it under threshold
        m_lootItem->is_underthreshold = true;
        m_lootItem->is_blocked = false;
    }
    return false;
}

// Add vote from player
bool LootRoll::PlayerVote(Player* player, RollVote vote)
{
    ObjectGuid const& playerGuid = player->GetGUID();
    RollVoteMap::iterator voterItr = m_rollVoteMap.find(playerGuid);
    if (voterItr == m_rollVoteMap.end() || voterItr->second.Vote != RollVote::NotEmitedYet)
        return false;

    voterItr->second.Vote = vote;

    if (vote != RollVote::Pass && vote != RollVote::NotValid)
        voterItr->second.RollNumber = urand(1, 100);

    switch (vote)
    {
        case RollVote::Pass:                                // Player choose pass
            SendRoll(playerGuid, 128, ROLL_PASS);
            break;
        case RollVote::Need:                                // player choose Need
            // the client only announces the choice when both the roll number and the roll type are 0,
            // anything else is rendered as an actual roll and would show up twice once the roll ends
            SendRoll(playerGuid, 0, 0);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_NEED, 1);
            break;
        case RollVote::Greed:                               // player choose Greed
            SendRoll(playerGuid, 128, ROLL_GREED);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_GREED, 1);
            break;
        case RollVote::Disenchant:                          // player choose Disenchant
            SendRoll(playerGuid, 128, ROLL_DISENCHANT);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_GREED, 1);
            break;
        default:                                            // Roll removed case
            return false;
    }
    return true;
}

// check if we can find a winner for this roll or if the timer is expired
bool LootRoll::UpdateRoll()
{
    RollVoteMap::const_iterator winnerItr = m_rollVoteMap.end();

    if (AllPlayerVoted(winnerItr) || m_endTime <= GameTime::Now())
    {
        Finish(winnerItr);
        return true;
    }
    return false;
}

bool LootRoll::IsLootItem(ObjectGuid const& lootObject, uint32 lootListId) const
{
    return m_lootObject == lootObject && m_lootListId == lootListId;
}

/**
* \brief Check if all player have voted and return true in that case. Also return current winner.
* \param winnerItr > will be different than m_rollVoteMap.end() if winner exist. (Someone voted greed or need)
* \returns true if all players voted
**/
bool LootRoll::AllPlayerVoted(RollVoteMap::const_iterator& winnerItr)
{
    uint32 notVoted = 0;
    bool isSomeoneNeed = false;

    winnerItr = m_rollVoteMap.end();
    for (RollVoteMap::const_iterator itr = m_rollVoteMap.begin(); itr != m_rollVoteMap.end(); ++itr)
    {
        switch (itr->second.Vote)
        {
            case RollVote::Need:
                if (!isSomeoneNeed || winnerItr == m_rollVoteMap.end() || itr->second.RollNumber > winnerItr->second.RollNumber)
                {
                    isSomeoneNeed = true;                                               // first passage will force to set winner because need is prioritized
                    winnerItr = itr;
                }
                break;
            case RollVote::Greed:
            case RollVote::Disenchant:
                if (!isSomeoneNeed)                                                      // if at least one need is detected then winner can't be a greed
                {
                    if (winnerItr == m_rollVoteMap.end() || itr->second.RollNumber > winnerItr->second.RollNumber)
                        winnerItr = itr;
                }
                break;
            // Explicitly passing excludes a player from winning loot, so no action required.
            case RollVote::Pass:
                break;
            case RollVote::NotEmitedYet:
                ++notVoted;
                break;
            default:
                break;
        }
    }

    return notVoted == 0;
}

// terminate the roll
void LootRoll::Finish(RollVoteMap::const_iterator winnerItr)
{
    m_lootItem->is_blocked = false;
    if (winnerItr == m_rollVoteMap.end())
    {
        SendAllPassed();
    }
    else
    {
        m_lootItem->rollWinnerGUID = winnerItr->first;

        SendLootRollWon(winnerItr->first, winnerItr->second.RollNumber, winnerItr->second.Vote);

        if (Player* player = ObjectAccessor::FindConnectedPlayer(winnerItr->first))
        {
            if (winnerItr->second.Vote == RollVote::Need)
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_NEED_ON_LOOT, m_lootItem->itemid, winnerItr->second.RollNumber);
            else if (winnerItr->second.Vote == RollVote::Disenchant)
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL, 13262); // Disenchant
            else
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_GREED_ON_LOOT, m_lootItem->itemid, winnerItr->second.RollNumber);

            if (winnerItr->second.Vote == RollVote::Disenchant)
            {
                ItemTemplate const* itemTemplate = ASSERT_NOTNULL(sObjectMgr->GetItemTemplate(m_lootItem->itemid));

                m_lootItem->is_looted = true;
                --m_loot->unlootedCount;
                m_loot->NotifyItemRemoved(m_lootListId);

                Loot loot;
                loot.FillLoot(itemTemplate->DisenchantID, LootTemplates_Disenchant, player, true);
                if (!loot.AutoStore(player, NULL_BAG, NULL_SLOT, true))
                {
                    // If the player's inventory is full, send the disenchant result in a mail.
                    uint32 maxSlot = loot.GetMaxSlotInLootFor(player);
                    for (uint32 i = 0; i < maxSlot; ++i)
                        if (LootItem* disenchantLoot = loot.LootItemInSlot(i, player))
                            player->SendItemRetrievalMail(disenchantLoot->itemid, disenchantLoot->count);
                }
            }
            else
                player->StoreLootItem(GetLootSlotFor(player), m_loot);
        }
    }
    m_isStarted = false;
}

// Quest items are indexed per looter in the loot window, the roll works on the loot wide index
uint8 LootRoll::GetLootSlotFor(Player const* player) const
{
    if (m_lootListId < m_loot->items.size())
        return m_lootListId;

    NotNormalLootItemMap const& questItems = m_loot->GetPlayerQuestItems();
    NotNormalLootItemMap::const_iterator itr = questItems.find(player->GetGUID());
    if (itr == questItems.end())
        return MAX_NR_LOOT_ITEMS + MAX_NR_QUEST_ITEMS;

    uint8 questIndex = m_lootListId - m_loot->items.size();
    for (std::size_t i = 0; i < itr->second->size(); ++i)
        if ((*itr->second)[i].index == questIndex)
            return m_loot->items.size() + i;

    return MAX_NR_LOOT_ITEMS + MAX_NR_QUEST_ITEMS;
}

//
// --------- Loot ---------
//

Loot::Loot(uint32 _gold /*= 0*/) : gold(_gold), unlootedCount(0), roundRobinPlayer(), loot_type(LOOT_NONE), maxDuplicates(1), containerID(0),
    _lootMethod(FREE_FOR_ALL), _wasOpened(false)
{
}

Loot::~Loot()
{
    clear();
}

void Loot::clear()
{
    // rolls hold a pointer into items/quest_items and notify the participants on destruction, drop them first
    _rolls.clear();

    for (NotNormalLootItemMap::const_iterator itr = PlayerQuestItems.begin(); itr != PlayerQuestItems.end(); ++itr)
        delete itr->second;
    PlayerQuestItems.clear();

    for (NotNormalLootItemMap::const_iterator itr = PlayerFFAItems.begin(); itr != PlayerFFAItems.end(); ++itr)
        delete itr->second;
    PlayerFFAItems.clear();

    for (NotNormalLootItemMap::const_iterator itr = PlayerNonQuestNonFFAConditionalItems.begin(); itr != PlayerNonQuestNonFFAConditionalItems.end(); ++itr)
        delete itr->second;
    PlayerNonQuestNonFFAConditionalItems.clear();

    PlayersLooting.clear();
    items.clear();
    quest_items.clear();
    gold = 0;
    unlootedCount = 0;
    roundRobinPlayer.Clear();
    loot_type = LOOT_NONE;
    _lootMethod = FREE_FOR_ALL;
    _lootMaster.Clear();
    _allowedLooters.clear();
    _wasOpened = false;
}

// Inserts the item into the loot (called by LootTemplate processors)
void Loot::AddItem(LootStoreItem const& item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
    if (!proto)
        return;

    uint32 count = urand(item.mincount, item.maxcount);
    uint32 stacks = count / proto->GetMaxStackSize() + ((count % proto->GetMaxStackSize()) ? 1 : 0);

    std::vector<LootItem>& lootItems = item.needs_quest ? quest_items : items;
    uint32 limit = item.needs_quest ? MAX_NR_QUEST_ITEMS : MAX_NR_LOOT_ITEMS;

    for (uint32 i = 0; i < stacks && lootItems.size() < limit; ++i)
    {
        LootItem generatedLoot(item);
        generatedLoot.count = std::min(count, proto->GetMaxStackSize());
        generatedLoot.LootListId = lootItems.size();
        lootItems.push_back(generatedLoot);
        count -= proto->GetMaxStackSize();

        // In some cases, a dropped item should be visible/lootable only for some players in group
        bool canSeeItemInLootWindow = false;
        if (Player* player = ObjectAccessor::FindPlayer(lootOwnerGUID))
        {
            if (Group* group = player->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    if (Player* member = itr->GetSource())
                        if (generatedLoot.AllowedForPlayer(member, lootOwnerGUID))
                            canSeeItemInLootWindow = true;
            }
            else if (generatedLoot.AllowedForPlayer(player, lootOwnerGUID))
                canSeeItemInLootWindow = true;
        }

        if (!canSeeItemInLootWindow)
            continue;

        // non-conditional one-player only items are counted here,
        // free for all items are counted in FillFFALoot(),
        // non-ffa conditionals are counted in FillNonQuestNonFFAConditionalLoot()
        if (!item.needs_quest && item.conditions.empty() && !proto->HasFlag(ITEM_FLAG_MULTI_DROP))
            ++unlootedCount;
    }
}

// Calls processor of corresponding LootTemplate (which handles everything including references)
bool Loot::FillLoot(uint32 lootId, LootStore const& store, Player* lootOwner, bool personal, bool noEmptyError, uint16 lootMode /*= LOOT_MODE_DEFAULT*/)
{
    // Must be provided
    if (!lootOwner)
        return false;

    lootOwnerGUID = lootOwner->GetGUID();

    LootTemplate const* tab = store.GetLootFor(lootId);

    if (!tab)
    {
        if (!noEmptyError)
            TC_LOG_ERROR("sql.sql", "Table '{}' loot id #{} used but it doesn't have records.", store.GetName(), lootId);
        return false;
    }

    items.reserve(MAX_NR_LOOT_ITEMS);
    quest_items.reserve(MAX_NR_QUEST_ITEMS);

    tab->Process(*this, store.IsRatesAllowed(), lootMode);          // Processing is done there, callback via Loot::AddItem()

                                                                    // Setting access rights for group loot case
    Group* group = lootOwner->GetGroup();
    if (!personal && group)
    {
        roundRobinPlayer = lootOwner->GetGUID();
        _lootMethod = group->GetLootMethod();
        _lootMaster = group->GetMasterLooterGuid();

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (Player* player = itr->GetSource())   // should actually be looted object instead of lootOwner but looter has to be really close so doesnt really matter
                if (player->IsAtGroupRewardDistance(lootOwner))
                    FillNotNormalLootFor(player);

        auto processLootItem = [&](LootItem& item)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
            if (!proto)
                return;

            if (proto->GetQuality() < uint32(group->GetLootThreshold()))
                item.is_underthreshold = true;
            else
            {
                switch (_lootMethod)
                {
                    case MASTER_LOOT:
                    case GROUP_LOOT:
                    case NEED_BEFORE_GREED:
                        item.is_blocked = true;
                        break;
                    default:
                        break;
                }
            }
        };

        for (LootItem& item : items)
        {
            if (item.freeforall)
                continue;

            processLootItem(item);
        }

        for (LootItem& item : quest_items)
        {
            if (!item.follow_loot_rules)
                continue;

            processLootItem(item);
        }
    }
    // ... for personal loot
    else
        FillNotNormalLootFor(lootOwner);

    return true;
}

void Loot::FillNotNormalLootFor(Player* player)
{
    ObjectGuid plguid = player->GetGUID();
    _allowedLooters.insert(plguid);

    NotNormalLootItemMap::const_iterator qmapitr = PlayerQuestItems.find(plguid);
    if (qmapitr == PlayerQuestItems.end())
        FillQuestLoot(player);

    qmapitr = PlayerFFAItems.find(plguid);
    if (qmapitr == PlayerFFAItems.end())
        FillFFALoot(player);

    qmapitr = PlayerNonQuestNonFFAConditionalItems.find(plguid);
    if (qmapitr == PlayerNonQuestNonFFAConditionalItems.end())
        FillNonQuestNonFFAConditionalLoot(player);

    // Process currency items
    uint32 max_slot = GetMaxSlotInLootFor(player);
    LootItem const* item = nullptr;
    uint32 itemsSize = uint32(items.size());
    for (uint32 i = 0; i < max_slot; ++i)
    {
        if (i < items.size())
            item = &items[i];
        else
            item = &quest_items[i - itemsSize];

        if (!item->is_looted && item->freeforall && item->AllowedForPlayer(player, lootOwnerGUID))
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid))
                if (proto->IsCurrencyToken())
                    player->StoreLootItem(i, this);
    }
}

NotNormalLootItemList* Loot::FillFFALoot(Player* player)
{
    NotNormalLootItemList* ql = new NotNormalLootItemList();

    for (uint8 i = 0; i < items.size(); ++i)
    {
        LootItem &item = items[i];
        if (!item.is_looted && item.freeforall && item.AllowedForPlayer(player, lootOwnerGUID))
        {
            ql->push_back(NotNormalLootItem(i));
            ++unlootedCount;
        }
    }
    if (ql->empty())
    {
        delete ql;
        return nullptr;
    }

    PlayerFFAItems[player->GetGUID()] = ql;
    return ql;
}

NotNormalLootItemList* Loot::FillQuestLoot(Player* player)
{
    if (items.size() == MAX_NR_LOOT_ITEMS)
        return nullptr;

    NotNormalLootItemList* ql = new NotNormalLootItemList();

    for (uint8 i = 0; i < quest_items.size(); ++i)
    {
        LootItem &item = quest_items[i];

        if (!item.is_looted && (item.AllowedForPlayer(player, lootOwnerGUID) || (item.follow_loot_rules && player->GetGroup() && ((player->GetGroup()->GetLootMethod() == MASTER_LOOT && player->GetGroup()->GetMasterLooterGuid() == player->GetGUID()) || player->GetGroup()->GetLootMethod() != MASTER_LOOT))))
        {
            item.AddAllowedLooter(player);

            ql->push_back(NotNormalLootItem(i));

            // quest items get blocked when they first appear in a
            // player's quest vector
            //
            // increase once if one looter only, looter-times if free for all
            if (item.freeforall || !item.is_blocked)
                ++unlootedCount;
            if (!player->GetGroup() || (player->GetGroup()->GetLootMethod() != GROUP_LOOT && player->GetGroup()->GetLootMethod() != ROUND_ROBIN))
                item.is_blocked = true;

            if (items.size() + ql->size() == MAX_NR_LOOT_ITEMS)
                break;
        }
    }
    if (ql->empty())
    {
        delete ql;
        return nullptr;
    }

    PlayerQuestItems[player->GetGUID()] = ql;
    return ql;
}

NotNormalLootItemList* Loot::FillNonQuestNonFFAConditionalLoot(Player* player)
{
    NotNormalLootItemList* ql = new NotNormalLootItemList();

    for (uint8 i = 0; i < items.size(); ++i)
    {
        LootItem &item = items[i];
        if (!item.is_looted && !item.freeforall && (item.AllowedForPlayer(player, lootOwnerGUID)))
        {
            item.AddAllowedLooter(player);
            if (!item.conditions.empty())
            {
                ql->push_back(NotNormalLootItem(i));
                if (!item.is_counted)
                {
                    ++unlootedCount;
                    item.is_counted = true;
                }
            }
        }
    }
    if (ql->empty())
    {
        delete ql;
        return nullptr;
    }

    PlayerNonQuestNonFFAConditionalItems[player->GetGUID()] = ql;
    return ql;
}

//===================================================

void Loot::NotifyLootList(Map const* map, ObjectGuid const& owner) const
{
    WorldPacket data(SMSG_LOOT_LIST, (8 + 8));
    data << owner;

    if (GetLootMethod() == MASTER_LOOT && hasOverThresholdItem())
        data << GetLootMasterGUID().WriteAsPacked();
    else
        data << uint8(0);

    if (!roundRobinPlayer.IsEmpty())
        data << roundRobinPlayer.WriteAsPacked();
    else
        data << uint8(0);

    for (ObjectGuid const& allowedLooterGuid : _allowedLooters)
        if (Player* allowedLooter = ObjectAccessor::GetPlayer(map, allowedLooterGuid))
            allowedLooter->SendDirectMessage(&data);
}

void Loot::NotifyItemRemoved(uint8 lootIndex)
{
    // notify all players that are looting this that the item was removed
    // convert the index to the slot the player sees
    GuidSet::iterator i_next;
    for (GuidSet::iterator i = PlayersLooting.begin(); i != PlayersLooting.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        if (Player* player = ObjectAccessor::FindPlayer(*i))
            player->SendNotifyLootItemRemoved(lootIndex);
        else
            PlayersLooting.erase(i);
    }
}

void Loot::NotifyMoneyRemoved()
{
    // notify all players that are looting this that the money was removed
    GuidSet::iterator i_next;
    for (GuidSet::iterator i = PlayersLooting.begin(); i != PlayersLooting.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        if (Player* player = ObjectAccessor::FindPlayer(*i))
            player->SendNotifyLootMoneyRemoved();
        else
            PlayersLooting.erase(i);
    }
}

void Loot::NotifyQuestItemRemoved(uint8 questIndex)
{
    // when a free for all questitem is looted
    // all players will get notified of it being removed
    // (other questitems can be looted by each group member)
    // bit inefficient but isn't called often

    GuidSet::iterator i_next;
    for (GuidSet::iterator i = PlayersLooting.begin(); i != PlayersLooting.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        if (Player* player = ObjectAccessor::FindPlayer(*i))
        {
            NotNormalLootItemMap::const_iterator pq = PlayerQuestItems.find(player->GetGUID());
            if (pq != PlayerQuestItems.end() && pq->second)
            {
                // find where/if the player has the given item in it's vector
                NotNormalLootItemList& pql = *pq->second;

                uint8 j;
                for (j = 0; j < pql.size(); ++j)
                    if (pql[j].index == questIndex)
                        break;

                if (j < pql.size())
                    player->SendNotifyLootItemRemoved(items.size() + j);
            }
        }
        else
            PlayersLooting.erase(i);
    }
}

void Loot::OnLootOpened(Map* map, ObjectGuid const& lootObject, ObjectGuid const& looter)
{
    AddLooter(looter);
    if (_wasOpened)
        return;

    _wasOpened = true;

    if (_lootMethod == GROUP_LOOT || _lootMethod == NEED_BEFORE_GREED)
    {
        uint32 maxEnchantingSkill = 0;
        for (ObjectGuid const& allowedLooterGuid : _allowedLooters)
            if (Player* allowedLooter = ObjectAccessor::GetPlayer(map, allowedLooterGuid))
                maxEnchantingSkill = std::max<uint32>(maxEnchantingSkill, allowedLooter->GetSkillValue(SKILL_ENCHANTING));

        uint32 lootListId = 0;
        for (; lootListId < items.size(); ++lootListId)
        {
            if (!items[lootListId].is_blocked)
                continue;

            auto itr = _rolls.try_emplace(lootListId).first;
            if (!itr->second.TryToStart(map, *this, lootObject, lootListId, maxEnchantingSkill))
                _rolls.erase(itr);
        }

        for (; lootListId - items.size() < quest_items.size(); ++lootListId)
        {
            LootItem const& item = quest_items[lootListId - items.size()];
            // quest items use is_blocked for other purposes as well, only those following the loot rules are rolled for
            if (!item.is_blocked || !item.follow_loot_rules)
                continue;

            auto itr = _rolls.try_emplace(lootListId).first;
            if (!itr->second.TryToStart(map, *this, lootObject, lootListId, maxEnchantingSkill))
                _rolls.erase(itr);
        }
    }
    else if (_lootMethod == MASTER_LOOT && looter == _lootMaster)
    {
        if (Player* lootMaster = ObjectAccessor::GetPlayer(map, looter))
        {
            WorldPacket data(SMSG_LOOT_MASTER_LIST, 1 + _allowedLooters.size() * 8);
            data << uint8(_allowedLooters.size());
            for (ObjectGuid const& allowedLooterGuid : _allowedLooters)
                data << allowedLooterGuid;

            lootMaster->SendDirectMessage(&data);
        }
    }
}

void Loot::Update()
{
    for (auto itr = _rolls.begin(); itr != _rolls.end(); )
    {
        if (itr->second.UpdateRoll())
            itr = _rolls.erase(itr);
        else
            ++itr;
    }
}

void Loot::generateMoneyLoot(uint32 minAmount, uint32 maxAmount)
{
    if (maxAmount > 0)
    {
        if (maxAmount <= minAmount)
            gold = uint32(maxAmount * sWorld->getRate(RATE_DROP_MONEY));
        else if ((maxAmount - minAmount) < 32700)
            gold = uint32(urand(minAmount, maxAmount) * sWorld->getRate(RATE_DROP_MONEY));
        else
            gold = uint32(urand(minAmount >> 8, maxAmount >> 8) * sWorld->getRate(RATE_DROP_MONEY)) << 8;
    }
}

LootItem* Loot::LootItemInSlot(uint32 lootSlot, Player* player, NotNormalLootItem* *qitem, NotNormalLootItem* *ffaitem, NotNormalLootItem* *conditem)
{
    LootItem* item = nullptr;
    bool is_looted = true;
    if (lootSlot >= items.size())
    {
        uint32 questSlot = lootSlot - items.size();
        NotNormalLootItemMap::const_iterator itr = PlayerQuestItems.find(player->GetGUID());
        if (itr != PlayerQuestItems.end() && questSlot < itr->second->size())
        {
            NotNormalLootItem* qitem2 = &itr->second->at(questSlot);
            if (qitem)
                *qitem = qitem2;
            item = &quest_items[qitem2->index];
            is_looted = qitem2->is_looted;
        }
    }
    else
    {
        item = &items[lootSlot];
        is_looted = item->is_looted;
        if (item->freeforall)
        {
            NotNormalLootItemMap::const_iterator itr = PlayerFFAItems.find(player->GetGUID());
            if (itr != PlayerFFAItems.end())
            {
                for (NotNormalLootItemList::const_iterator iter = itr->second->begin(); iter != itr->second->end(); ++iter)
                    if (iter->index == lootSlot)
                    {
                        NotNormalLootItem* ffaitem2 = (NotNormalLootItem*)&(*iter);
                        if (ffaitem)
                            *ffaitem = ffaitem2;
                        is_looted = ffaitem2->is_looted;
                        break;
                    }
            }
        }
        else if (!item->conditions.empty())
        {
            NotNormalLootItemMap::const_iterator itr = PlayerNonQuestNonFFAConditionalItems.find(player->GetGUID());
            if (itr != PlayerNonQuestNonFFAConditionalItems.end())
            {
                for (NotNormalLootItemList::const_iterator iter = itr->second->begin(); iter != itr->second->end(); ++iter)
                {
                    if (iter->index == lootSlot)
                    {
                        NotNormalLootItem* conditem2 = (NotNormalLootItem*)&(*iter);
                        if (conditem)
                            *conditem = conditem2;
                        is_looted = conditem2->is_looted;
                        break;
                    }
                }
            }
        }
    }

    if (is_looted)
        return nullptr;

    return item;
}

bool Loot::AutoStore(Player* player, uint8 bag, uint8 slot, bool broadcast /*= false*/, bool createdByPlayer /*= false*/)
{
    bool allLooted = true;
    uint32 max_slot = GetMaxSlotInLootFor(player);
    for (uint32 i = 0; i < max_slot; ++i)
    {
        NotNormalLootItem* qitem = nullptr;
        NotNormalLootItem* ffaitem = nullptr;
        NotNormalLootItem* conditem = nullptr;

        LootItem* lootItem = LootItemInSlot(i, player, &qitem, &ffaitem, &conditem);
        if (!lootItem || lootItem->is_looted)
            continue;

        if (!lootItem->AllowedForPlayer(player))
            continue;

        // questitems use the blocked field for other purposes
        if (!qitem && lootItem->is_blocked)
            continue;

        // dont allow protected item to be looted by someone else
        if (!lootItem->rollWinnerGUID.IsEmpty() && lootItem->rollWinnerGUID != player->GetGUID())
            continue;

        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(bag, slot, dest, lootItem->itemid, lootItem->count);
        if (msg != EQUIP_ERR_OK && slot != NULL_SLOT)
            msg = player->CanStoreNewItem(bag, NULL_SLOT, dest, lootItem->itemid, lootItem->count);
        if (msg != EQUIP_ERR_OK && bag != NULL_BAG)
            msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, lootItem->itemid, lootItem->count);
        if (msg != EQUIP_ERR_OK)
        {
            player->SendEquipError(msg, nullptr, nullptr, lootItem->itemid);
            allLooted = false;
            continue;
        }

        if (qitem)
            qitem->is_looted = true;
        else if (ffaitem)
            ffaitem->is_looted = true;
        else if (conditem)
            conditem->is_looted = true;

        if (!lootItem->freeforall)
            lootItem->is_looted = true;

        --unlootedCount;

        Item* pItem = player->StoreNewItem(dest, lootItem->itemid, true, lootItem->randomPropertyId);
        player->SendNewItem(pItem, lootItem->count, false, createdByPlayer, broadcast);
    }

    return allLooted;
}

uint32 Loot::GetMaxSlotInLootFor(Player* player) const
{
    NotNormalLootItemMap::const_iterator itr = PlayerQuestItems.find(player->GetGUID());
    return items.size() + (itr != PlayerQuestItems.end() ? itr->second->size() : 0);
}

// return true if there is any item that is lootable for any player (not quest item, FFA or conditional)
bool Loot::hasItemForAll() const
{
    // Gold is always lootable
    if (gold)
        return true;

    for (LootItem const& item : items)
        if (!item.is_looted && !item.freeforall && item.conditions.empty())
            return true;
    return false;
}

// return true if there is any FFA, quest or conditional item for the player.
bool Loot::hasItemFor(Player const* player) const
{
    NotNormalLootItemMap const& lootPlayerQuestItems = GetPlayerQuestItems();
    NotNormalLootItemMap::const_iterator q_itr = lootPlayerQuestItems.find(player->GetGUID());
    if (q_itr != lootPlayerQuestItems.end())
    {
        NotNormalLootItemList* q_list = q_itr->second;
        for (NotNormalLootItemList::const_iterator qi = q_list->begin(); qi != q_list->end(); ++qi)
        {
            LootItem const& item = quest_items[qi->index];
            if (!qi->is_looted && !item.is_looted)
                return true;
        }
    }

    NotNormalLootItemMap const& lootPlayerFFAItems = GetPlayerFFAItems();
    NotNormalLootItemMap::const_iterator ffa_itr = lootPlayerFFAItems.find(player->GetGUID());
    if (ffa_itr != lootPlayerFFAItems.end())
    {
        NotNormalLootItemList* ffa_list = ffa_itr->second;
        for (NotNormalLootItemList::const_iterator fi = ffa_list->begin(); fi != ffa_list->end(); ++fi)
        {
            LootItem const& item = items[fi->index];
            if (!fi->is_looted && !item.is_looted)
                return true;
        }
    }

    NotNormalLootItemMap const& lootPlayerNonQuestNonFFAConditionalItems = GetPlayerNonQuestNonFFAConditionalItems();
    NotNormalLootItemMap::const_iterator nn_itr = lootPlayerNonQuestNonFFAConditionalItems.find(player->GetGUID());
    if (nn_itr != lootPlayerNonQuestNonFFAConditionalItems.end())
    {
        NotNormalLootItemList* conditional_list = nn_itr->second;
        for (NotNormalLootItemList::const_iterator ci = conditional_list->begin(); ci != conditional_list->end(); ++ci)
        {
            LootItem const& item = items[ci->index];
            if (!ci->is_looted && !item.is_looted)
                return true;
        }
    }

    return false;
}

// return true if there is any item over the group threshold (i.e. not underthreshold).
bool Loot::hasOverThresholdItem() const
{
    for (uint8 i = 0; i < items.size(); ++i)
    {
        if (!items[i].is_looted && !items[i].is_underthreshold && !items[i].freeforall)
            return true;
    }

    return false;
}

static void FillLootItemData(WorldPackets::Loot::LootItemData& lootItem, uint8 lootListId, LootItem const& li, LootSlotType slot_type)
{
    lootItem.LootListID = lootListId;
    lootItem.UIType = slot_type;
    lootItem.ItemID = li.itemid;
    lootItem.Quantity = li.count;
    lootItem.ItemDisplayInfoID = ASSERT_NOTNULL(sObjectMgr->GetItemTemplate(li.itemid))->GetDisplayId();
    lootItem.RandomPropertiesSeed = li.randomPropertySeed;
    lootItem.RandomPropertiesID = li.randomPropertyId;
}

void Loot::BuildLootResponse(WorldPackets::Loot::LootResponse& packet, Player const* viewer, PermissionTypes permission) const
{
    if (permission == NONE_PERMISSION)
        return;

    packet.Coins = gold;

    switch (permission)
    {
        case GROUP_PERMISSION:
        case MASTER_PERMISSION:
        case RESTRICTED_PERMISSION:
        {
            // if you are not the round-robin group looter, you can only see
            // blocked rolled items and quest items, and !ffa items
            for (uint8 i = 0; i < items.size(); ++i)
            {
                if (!items[i].is_looted && !items[i].freeforall && items[i].conditions.empty() && items[i].AllowedForPlayer(viewer, roundRobinPlayer))
                {
                    LootSlotType slot_type;

                    if (items[i].is_blocked) // for ML & restricted is_blocked = !is_underthreshold
                    {
                        switch (permission)
                        {
                            case GROUP_PERMISSION:
                                slot_type = LOOT_SLOT_TYPE_ROLL_ONGOING;
                                break;
                            case MASTER_PERMISSION:
                            {
                                if (viewer->GetGroup() && viewer->GetGroup()->GetMasterLooterGuid() == viewer->GetGUID())
                                    slot_type = LOOT_SLOT_TYPE_MASTER;
                                else
                                    slot_type = LOOT_SLOT_TYPE_LOCKED;
                                break;
                            }
                            case RESTRICTED_PERMISSION:
                                slot_type = LOOT_SLOT_TYPE_LOCKED;
                                break;
                            default:
                                continue;
                        }
                    }
                    else if (!items[i].rollWinnerGUID.IsEmpty())
                    {
                        if (items[i].rollWinnerGUID == viewer->GetGUID())
                            slot_type = LOOT_SLOT_TYPE_OWNER;
                        else
                            continue;
                    }
                    else if (roundRobinPlayer.IsEmpty() || viewer->GetGUID() == roundRobinPlayer || !items[i].is_underthreshold)
                    {
                        // no round robin owner or he has released the loot
                        // or it IS the round robin group owner
                        // => item is lootable
                        slot_type = LOOT_SLOT_TYPE_ALLOW_LOOT;
                    }
                    else
                        // item shall not be displayed.
                        continue;

                    FillLootItemData(packet.Items.emplace_back(), i, items[i], slot_type);
                }
            }
            break;
        }
        case ROUND_ROBIN_PERMISSION:
        {
            for (uint8 i = 0; i < items.size(); ++i)
            {
                if (!items[i].is_looted && !items[i].freeforall && items[i].conditions.empty() && items[i].AllowedForPlayer(viewer, roundRobinPlayer))
                {
                    if (!roundRobinPlayer.IsEmpty() && viewer->GetGUID() != roundRobinPlayer)
                        // item shall not be displayed.
                        continue;

                    FillLootItemData(packet.Items.emplace_back(), i, items[i], LOOT_SLOT_TYPE_ALLOW_LOOT);
                }
            }
            break;
        }
        case ALL_PERMISSION:
        case OWNER_PERMISSION:
        {
            LootSlotType slot_type = permission == OWNER_PERMISSION ? LOOT_SLOT_TYPE_OWNER : LOOT_SLOT_TYPE_ALLOW_LOOT;
            for (uint8 i = 0; i < items.size(); ++i)
                if (!items[i].is_looted && !items[i].freeforall && items[i].conditions.empty() && items[i].AllowedForPlayer(viewer, roundRobinPlayer))
                    FillLootItemData(packet.Items.emplace_back(), i, items[i], slot_type);
            break;
        }
        default:
            return;
    }

    LootSlotType slotType = permission == OWNER_PERMISSION ? LOOT_SLOT_TYPE_OWNER : LOOT_SLOT_TYPE_ALLOW_LOOT;
    NotNormalLootItemMap const& lootPlayerQuestItems = GetPlayerQuestItems();
    NotNormalLootItemMap::const_iterator q_itr = lootPlayerQuestItems.find(viewer->GetGUID());
    if (q_itr != lootPlayerQuestItems.end())
    {
        NotNormalLootItemList* q_list = q_itr->second;
        for (NotNormalLootItemList::const_iterator qi = q_list->begin(); qi != q_list->end(); ++qi)
        {
            LootItem const& item = quest_items[qi->index];
            if (!qi->is_looted && !item.is_looted)
            {
                LootSlotType effectiveSlotType = slotType;
                if (item.follow_loot_rules)
                {
                    switch (permission)
                    {
                        case MASTER_PERMISSION:
                            effectiveSlotType = LOOT_SLOT_TYPE_MASTER;
                            break;
                        case RESTRICTED_PERMISSION:
                            if (item.is_blocked)
                                effectiveSlotType = LOOT_SLOT_TYPE_LOCKED;
                            break;
                        case GROUP_PERMISSION:
                        case ROUND_ROBIN_PERMISSION:
                            effectiveSlotType = !item.is_blocked ? LOOT_SLOT_TYPE_ALLOW_LOOT : LOOT_SLOT_TYPE_ROLL_ONGOING;
                            break;
                        default:
                            break;
                    }
                }

                FillLootItemData(packet.Items.emplace_back(), items.size() + (qi - q_list->begin()), item, effectiveSlotType);
            }
        }
    }

    NotNormalLootItemMap const& lootPlayerFFAItems = GetPlayerFFAItems();
    NotNormalLootItemMap::const_iterator ffa_itr = lootPlayerFFAItems.find(viewer->GetGUID());
    if (ffa_itr != lootPlayerFFAItems.end())
    {
        NotNormalLootItemList* ffa_list = ffa_itr->second;
        for (NotNormalLootItemList::const_iterator fi = ffa_list->begin(); fi != ffa_list->end(); ++fi)
        {
            LootItem const& item = items[fi->index];
            if (!fi->is_looted && !item.is_looted)
                FillLootItemData(packet.Items.emplace_back(), fi->index, item, slotType);
        }
    }

    NotNormalLootItemMap const& lootPlayerNonQuestNonFFAConditionalItems = GetPlayerNonQuestNonFFAConditionalItems();
    NotNormalLootItemMap::const_iterator nn_itr = lootPlayerNonQuestNonFFAConditionalItems.find(viewer->GetGUID());
    if (nn_itr != lootPlayerNonQuestNonFFAConditionalItems.end())
    {
        NotNormalLootItemList* conditional_list = nn_itr->second;
        for (NotNormalLootItemList::const_iterator ci = conditional_list->begin(); ci != conditional_list->end(); ++ci)
        {
            LootItem const& item = items[ci->index];
            if (!ci->is_looted && !item.is_looted)
            {
                LootSlotType effectiveSlotType = slotType;
                switch (permission)
                {
                    case MASTER_PERMISSION:
                        effectiveSlotType = LOOT_SLOT_TYPE_MASTER;
                        break;
                    case RESTRICTED_PERMISSION:
                        if (item.is_blocked)
                            effectiveSlotType = LOOT_SLOT_TYPE_LOCKED;
                        break;
                    case GROUP_PERMISSION:
                    case ROUND_ROBIN_PERMISSION:
                        effectiveSlotType = !item.is_blocked ? LOOT_SLOT_TYPE_ALLOW_LOOT : LOOT_SLOT_TYPE_ROLL_ONGOING;
                        break;
                    default:
                        break;
                }

                FillLootItemData(packet.Items.emplace_back(), ci->index, item, effectiveSlotType);
            }
        }
    }
}
