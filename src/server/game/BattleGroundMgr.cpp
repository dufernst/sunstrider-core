/*
 * Copyright (C) 2005-2008 MaNGOS <http://www.mangosproject.org/>
 *
 * Copyright (C) 2008 Trinity <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Player.h"
#include "BattleGroundMgr.h"
#include "BattleGroundAV.h"
#include "BattleGroundAB.h"
#include "BattleGroundEY.h"
#include "BattleGroundWS.h"
#include "BattleGroundNA.h"
#include "BattleGroundBE.h"
#include "BattleGroundAA.h"
#include "BattleGroundRL.h"
#include "SharedDefines.h"
#include "MapManager.h"
#include "Map.h"
#include "MapInstanced.h"
#include "ObjectMgr.h"
#include "World.h"
#include "Chat.h"
#include "ArenaTeam.h"

/*********************************************************/
/***            BATTLEGROUND QUEUE SYSTEM              ***/
/*********************************************************/

BattlegroundQueue::BattlegroundQueue()
{
    //queues are empty, we don't have to call clear()
/*    for (int i = 0; i < MAX_BATTLEGROUND_QUEUE_RANGES; i++)
    {
        //m_QueuedPlayers[i].Horde = 0;
        //m_QueuedPlayers[i].Alliance = 0;
        //m_QueuedPlayers[i].AverageTime = 0;
    }*/
    
    m_avgTime = 0;
    m_lastTimes.clear();
}

BattlegroundQueue::~BattlegroundQueue()
{
    for (int i = 0; i < MAX_BATTLEGROUND_QUEUE_RANGES; i++)
    {
        m_QueuedPlayers[i].clear();
        for(auto & itr : m_QueuedGroups[i])
        {
            delete itr;
        }
        m_QueuedGroups[i].clear();
    }
}

void BattlegroundQueue::AddStatsForAvgTime(uint32 time)
{
    m_lastTimes.push_back(time);
    if (m_lastTimes.size() > 10)
        m_lastTimes.pop_front();
        
    uint32 totalTime = 0;
    for (std::list<uint32>::const_iterator itr = m_lastTimes.begin(); itr != m_lastTimes.end(); itr++)
        totalTime += (*itr);
        
    m_avgTime = uint32(totalTime / m_lastTimes.size());
    //TC_LOG_INFO("bg.battleground","New average time: %u", m_avgTime);
    //m_avgTime = uint32((prevTime + time) / m_playerCount);
}

// initialize eligible groups from the given source matching the given specifications
void BattlegroundQueue::EligibleGroups::Init(BattlegroundQueue::QueuedGroupsList *source, uint32 BgTypeId, uint32 side, uint32 MaxPlayers, uint8 ArenaType, bool IsRated, uint32 MinRating, uint32 MaxRating, uint32 DisregardTime, uint32 excludeTeam)
{
    // clear from prev initialization
    clear();
    BattlegroundQueue::QueuedGroupsList::iterator itr, next;
    // iterate through the source
    for(itr = source->begin(); itr!= source->end(); itr = next)
    {
        next = itr;
        ++next;
        if( (*itr)->BgTypeId == BgTypeId &&     // bg type must match
            (*itr)->ArenaType == ArenaType &&   // arena type must match
            (*itr)->IsRated == IsRated &&       // israted must match
            (*itr)->IsInvitedToBGInstanceGUID == 0 && // leave out already invited groups
            (*itr)->Team == side &&             // match side
            (*itr)->Players.size() <= MaxPlayers &&   // the group must fit in the bg
            ( !excludeTeam || (*itr)->ArenaTeamId != excludeTeam ) && // if excludeTeam is specified, leave out those arena team ids
            ( !IsRated || (*itr)->Players.size() == MaxPlayers ) &&   // if rated, then pass only if the player count is exact NEEDS TESTING! (but now this should never happen)
            ( !DisregardTime || (*itr)->JoinTime <= DisregardTime              // pass if disregard time is greater than join time
               || (*itr)->ArenaTeamRating == 0                 // pass if no rating info
               || ( (*itr)->ArenaTeamRating >= MinRating       // pass if matches the rating range
                     && (*itr)->ArenaTeamRating <= MaxRating ) ) )
        {
            // the group matches the conditions
            // using push_back for proper selecting when inviting
            push_back((*itr));
        }
    }
}

// selection pool initialization, used to clean up from prev selection
void BattlegroundQueue::SelectionPool::Init(EligibleGroups * curr)
{
    m_CurrEligGroups = curr;
    SelectedGroups.clear();
    PlayerCount = 0;
}

// remove group info from selection pool
void BattlegroundQueue::SelectionPool::RemoveGroup(GroupQueueInfo *ginfo)
{
    // find what to remove
    for(auto itr = SelectedGroups.begin(); itr != SelectedGroups.end(); ++itr)
    {
        if((*itr)==ginfo)
        {
            SelectedGroups.erase(itr);
            // decrease selected players count
            PlayerCount -= ginfo->Players.size();
            return;
        }
    }
}

// add group to selection
// used when building selection pools
void BattlegroundQueue::SelectionPool::AddGroup(GroupQueueInfo * ginfo)
{
    SelectedGroups.push_back(ginfo);
    // increase selected players count
    PlayerCount+=ginfo->Players.size();
}

// add group to bg queue with the given leader and bg specifications
GroupQueueInfo * BattlegroundQueue::AddGroup(Player *leader, uint32 BgTypeId, uint8 ArenaType, bool isRated, uint32 arenaRating, uint32 arenateamid)
{
    uint32 queue_id = leader->GetBattlegroundQueueIdFromLevel();

    // create new ginfo
    // cannot use the method like in addplayer, because that could modify an in-queue group's stats
    // (e.g. leader leaving queue then joining as individual again)
    auto  ginfo = new GroupQueueInfo;
    ginfo->BgTypeId                  = BgTypeId;
    ginfo->ArenaType                 = ArenaType;
    ginfo->ArenaTeamId               = arenateamid;
    ginfo->IsRated                   = isRated;
    ginfo->IsInvitedToBGInstanceGUID = 0;                       // maybe this should be modifiable by function arguments to enable selection of running instances?
    ginfo->JoinTime                  = GetMSTime();
    ginfo->Team                      = leader->GetTeam();
    ginfo->ArenaTeamRating           = arenaRating;
    ginfo->OpponentsTeamRating       = 0;                       //initialize it to 0

    ginfo->Players.clear();

    m_QueuedGroups[queue_id].push_back(ginfo);

    // return ginfo, because it is needed to add players to this group info
    return ginfo;
}

void BattlegroundQueue::AddPlayer(Player *plr, GroupQueueInfo *ginfo)
{
    uint32 queue_id = plr->GetBattlegroundQueueIdFromLevel();

    //if player isn't in queue, he is added, if already is, then values are overwritten, no memory leak
    PlayerQueueInfo& info = m_QueuedPlayers[queue_id][plr->GetGUID()];
    info.InviteTime                 = 0;
    info.LastInviteTime             = 0;
    info.LastOnlineTime             = GetMSTime();
    info.GroupInfo                  = ginfo;

    // add the pinfo to ginfo's list
    ginfo->Players[plr->GetGUID()]  = &info;
}

void BattlegroundQueue::RemovePlayer(uint64 guid, bool decreaseInvitedCount)
{
    Player *plr = ObjectAccessor::FindConnectedPlayer(guid);

    int32 queue_id = 0;                                     // signed for proper for-loop finish
    QueuedPlayersMap::iterator itr;
    GroupQueueInfo * group;
    QueuedGroupsList::iterator group_itr;
    bool IsSet = false;
    if(plr)
    {
        queue_id = plr->GetBattlegroundQueueIdFromLevel();

        itr = m_QueuedPlayers[queue_id].find(guid);
        if(itr != m_QueuedPlayers[queue_id].end())
            IsSet = true;
    }

    if(!IsSet)
    {
        // either player is offline, or he levelled up to another queue category
        // TC_LOG_ERROR("bg.battleground","Battleground: removing offline player from BG queue - this might not happen, but it should not cause crash");
        for (uint32 i = 0; i < MAX_BATTLEGROUND_QUEUE_RANGES; i++)
        {
            itr = m_QueuedPlayers[i].find(guid);
            if(itr != m_QueuedPlayers[i].end())
            {
                queue_id = i;
                IsSet = true;
                break;
            }
        }
    }

    // couldn't find the player in bg queue, return
    if(!IsSet)
    {
        //TC_LOG_ERROR("bg.battleground","Battleground: couldn't find player to remove.");
        return;
    }

    group = itr->second.GroupInfo;

    for(group_itr=m_QueuedGroups[queue_id].begin(); group_itr != m_QueuedGroups[queue_id].end(); ++group_itr)
    {
        if(group == (GroupQueueInfo*)(*group_itr))
            break;
    }

    // variables are set (what about leveling up when in queue????)
    // remove player from group
    // if only player there, remove group

    // remove player queue info from group queue info
    auto pitr = group->Players.find(guid);

    if(pitr != group->Players.end())
        group->Players.erase(pitr);

    // check for iterator correctness
    if (group_itr != m_QueuedGroups[queue_id].end() && itr != m_QueuedPlayers[queue_id].end())
    {
        // used when player left the queue, NOT used when porting to bg
        if (decreaseInvitedCount)
        {
            // if invited to bg, and should decrease invited count, then do it
            if(group->IsInvitedToBGInstanceGUID)
            {
                Battleground* bg = sBattlegroundMgr->GetBattleground(group->IsInvitedToBGInstanceGUID);
                if (bg)
                    bg->DecreaseInvitedCount(group->Team);
                if (bg && !bg->GetPlayersSize() && !bg->GetInvitedCount(TEAM_ALLIANCE) && !bg->GetInvitedCount(TEAM_HORDE))
                {
                    // no more players on battleground, set delete it
                    bg->SetDeleteThis();
                }
            }
            // update the join queue, maybe now the player's group fits in a queue!
            // not yet implemented (should store bgTypeId in group queue info?)
        }
        // remove player queue info
        m_QueuedPlayers[queue_id].erase(itr);
        // remove group queue info if needed
        if(group->Players.empty())
        {
            m_QueuedGroups[queue_id].erase(group_itr);
            delete group;
        }
        // NEEDS TESTING!
        // group wasn't empty, so it wasn't deleted, and player have left a rated queue -> everyone from the group should leave too
        // don't remove recursively if already invited to bg!
        else if(!group->IsInvitedToBGInstanceGUID && decreaseInvitedCount && group->IsRated)
        {
            // remove next player, this is recursive
            // first send removal information
            if(Player *plr2 = ObjectAccessor::FindConnectedPlayer(group->Players.begin()->first))
            {
                Battleground * bg = sBattlegroundMgr->GetBattlegroundTemplate(group->BgTypeId);
                uint32 bgQueueTypeId = sBattlegroundMgr->BGQueueTypeId(group->BgTypeId,group->ArenaType);
                uint32 queueSlot = plr2->GetBattlegroundQueueIndex(bgQueueTypeId);
                plr2->RemoveBattlegroundQueueId(bgQueueTypeId); // must be called this way, because if you move this call to queue->removeplayer, it causes bugs
                WorldPacket data;
                sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, plr2->GetTeam(), queueSlot, STATUS_NONE, 0, 0);
                plr2->SendDirectMessage(&data);
            }
            // then actually delete, this may delete the group as well!
            RemovePlayer(group->Players.begin()->first,decreaseInvitedCount);
        }
    }
}

bool BattlegroundQueue::InviteGroupToBG(GroupQueueInfo * ginfo, Battleground * bg, uint32 side)
{
    // set side if needed
    if(side)
        ginfo->Team = side;

    if(!ginfo->IsInvitedToBGInstanceGUID)
    {
        // not yet invited
        // set invitation
        ginfo->IsInvitedToBGInstanceGUID = bg->GetInstanceID();
        uint32 bgQueueTypeId = sBattlegroundMgr->BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
        // loop through the players
        for(auto itr = ginfo->Players.begin(); itr != ginfo->Players.end(); ++itr)
        {
            // set status
            itr->second->InviteTime = GetMSTime();
            itr->second->LastInviteTime = GetMSTime();

            // get the player
            Player* plr = ObjectAccessor::FindConnectedPlayer(itr->first);
            // if offline, skip him
            if(!plr)
                continue;

            // invite the player
            sBattlegroundMgr->InvitePlayer(plr, bg->GetInstanceID(),ginfo->Team);

            WorldPacket data;

            uint32 queueSlot = plr->GetBattlegroundQueueIndex(bgQueueTypeId);

            // send status packet
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, side?side:plr->GetTeam(), queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME, 0);
            plr->SendDirectMessage(&data);
            
            // Update average wait time information
            AddStatsForAvgTime(GetMSTimeDiff(itr->second->GroupInfo->JoinTime, GetMSTime()));
        }
        return true;
    }

    return false;
}

// used to recursively select groups from eligible groups
bool BattlegroundQueue::SelectionPool::Build(uint32 MinPlayers, uint32 MaxPlayers, EligibleGroups::iterator startitr)
{
    // start from the specified start iterator
    for(auto itr1 = startitr; itr1 != m_CurrEligGroups->end(); ++itr1)
    {
        // if it fits in, select it
        if(GetPlayerCount() + (*itr1)->Players.size() <= MaxPlayers)
        {
            auto next = itr1;
            ++next;
            AddGroup((*itr1));
            if(GetPlayerCount() >= MinPlayers)
            {
                // enough players are selected
                return true;
            }
            // try building from the rest of the elig. groups
            // if that succeeds, return true
            if(Build(MinPlayers,MaxPlayers,next))
                return true;
            // the rest didn't succeed, so this group cannot be included
            RemoveGroup((*itr1));
        }
    }
    if (MinPlayers == 0)
        return true;

    // build didn't succeed
    return false;
}

// this function is responsible for the selection of queued groups when trying to create new battlegrounds
bool BattlegroundQueue::BuildSelectionPool(uint32 bgTypeId, uint32 queue_id, uint32 MinPlayers, uint32 MaxPlayers,  SelectionPoolBuildMode mode, uint8 ArenaType, bool isRated, uint32 MinRating, uint32 MaxRating, uint32 DisregardTime, uint32 excludeTeam)
{
    uint32 side;
    switch(mode)
    {
    case NORMAL_ALLIANCE:
    case ONESIDE_ALLIANCE_TEAM1:
    case ONESIDE_ALLIANCE_TEAM2:
        side = TEAM_ALLIANCE;
        break;
    case NORMAL_HORDE:
    case ONESIDE_HORDE_TEAM1:
    case ONESIDE_HORDE_TEAM2:
        side = TEAM_HORDE;
        break;
    default:
        //unknown mode, return false
        TC_LOG_ERROR("battleground","Battleground: unknown selection pool build mode %u, returning...", mode);
        return false;
        break;
    }

    // initiate the groups eligible to create the bg
    m_EligibleGroups.Init(&(m_QueuedGroups[queue_id]), bgTypeId, side, MaxPlayers, ArenaType, isRated, MinRating, MaxRating, DisregardTime, excludeTeam);
    // init the selected groups (clear)
    // and set m_CurrEligGroups pointer
    // we set it this way to only have one EligibleGroups object to save some memory
    m_SelectionPools[mode].Init(&m_EligibleGroups);
    // build succeeded
    if(m_SelectionPools[mode].Build(MinPlayers,MaxPlayers,m_EligibleGroups.begin()))
        return true;

    // failed to build a selection pool matching the given values
    return false;
}

// used to remove the Enter Battle window if the battle has already, but someone still has it
// (this can happen in arenas mainly, since the preparation is shorter than the timer for the bgqueueremove event
void BattlegroundQueue::BGEndedRemoveInvites(Battleground *bg)
{
    uint32 queue_id = bg->GetQueueType();
    uint32 bgInstanceId = bg->GetInstanceID();
    uint32 bgQueueTypeId = sBattlegroundMgr->BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
    QueuedGroupsList::iterator itr, next;
    for(itr = m_QueuedGroups[queue_id].begin(); itr != m_QueuedGroups[queue_id].end(); itr = next)
    {
        // must do this way, because the groupinfo will be deleted when all playerinfos are removed
        GroupQueueInfo * ginfo = (*itr);
        next = itr;
        ++next;
        // if group was invited to this bg instance, then remove all references
        if(ginfo->IsInvitedToBGInstanceGUID == bgInstanceId)
        {
            // after removing this much playerinfos, the ginfo will be deleted, so we'll use a for loop
            uint32 to_remove = ginfo->Players.size();
            uint32 team = ginfo->Team;
            for(int i = 0; i < to_remove; ++i)
            {
                // always remove the first one in the group
                auto itr2 = ginfo->Players.begin();
                if(itr2 == ginfo->Players.end())
                {
                    TC_LOG_ERROR("battleground","Empty Players in ginfo, this should never happen!");
                    return;
                }

                // get the player
                Player * plr = ObjectAccessor::FindConnectedPlayer(itr2->first);
                if(!plr)
                {
                    TC_LOG_ERROR("battleground","Player offline when trying to remove from GroupQueueInfo, this should never happen.");
                    continue;
                }

                // get the queueslot
                uint32 queueSlot = plr->GetBattlegroundQueueIndex(bgQueueTypeId);
                if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES) // player is in queue
                {
                    plr->RemoveBattlegroundQueueId(bgQueueTypeId);
                    // remove player from queue, this might delete the ginfo as well! don't use that pointer after this!
                    RemovePlayer(itr2->first, true);
                    // this is probably unneeded, since this player was already invited -> does not fit when initing eligible groups
                    // but updating the queue can't hurt
                    Update(bgQueueTypeId, bg->GetQueueType());
                    // send info to client
                    WorldPacket data;
                    sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, team, queueSlot, STATUS_NONE, 0, 0);
                    plr->SendDirectMessage(&data);
                }
            }
        }
    }
}

/*
this method is called when group is inserted, or player / group is removed from BG Queue - there is only one player's status changed, so we don't use while(true) cycles to invite whole queue
it must be called after fully adding the members of a group to ensure group joining
should be called after removeplayer functions in some cases
*/
void BattlegroundQueue::Update(uint32 bgTypeId, uint32 queue_id, uint8 arenatype, bool isRated, uint32 arenaRating)
{
    if (queue_id >= MAX_BATTLEGROUND_QUEUE_RANGES)
    {
        //this is error, that caused crashes (not in , but now it shouldn't)
        TC_LOG_ERROR("battleground","BattlegroundQueue::Update() called for non existing queue type - this can cause crash, pls report problem, if this is the last line of error log before crash");
        return;
    }

    //if no players in queue ... do nothing
    if (m_QueuedGroups[queue_id].empty())
        return;

    //battleground with free slot for player should be always the last in this queue
    BGFreeSlotQueueType::iterator itr, next;
    for (itr = sBattlegroundMgr->BGFreeSlotQueue[bgTypeId].begin(); itr != sBattlegroundMgr->BGFreeSlotQueue[bgTypeId].end(); itr = next)
    {
        next = itr;
        ++next;
        // battleground is running, so if:
        // DO NOT allow queue manager to invite new player to running arena
        if ((*itr)->isBattleground() && (*itr)->GetTypeID() == bgTypeId && (*itr)->GetQueueType() == queue_id && (*itr)->GetStatus() > STATUS_WAIT_QUEUE && (*itr)->GetStatus() < STATUS_WAIT_LEAVE)
        {
            //we must check both teams
            Battleground* bg = *itr; //we have to store battleground pointer here, because when battleground is full, it is removed from free queue (not yet implemented!!)
            // and iterator is invalid

            for(auto & itr : m_QueuedGroups[queue_id])
            {
                // did the group join for this bg type?
                if(itr->BgTypeId != bgTypeId)
                    continue;
                // if so, check if fits in
                if(bg->GetFreeSlotsForTeam(itr->Team) >= itr->Players.size())
                {
                    // if group fits in, invite it
                    InviteGroupToBG(itr,bg,itr->Team);
                }
                // invite group if it has enough free slots in absolute (not with GetFreeSlotsForTeam)
                if (itr->Players.size() > 1 && (bg->GetInvitedCount(itr->Team) + itr->Players.size()) < bg->GetMaxPlayersPerTeam()) {
                    InviteGroupToBG(itr,bg,itr->Team);
                }
            }

            if (!bg->HasFreeSlots())
            {
                //remove BG from BGFreeSlotQueue
                bg->RemoveFromBGFreeSlotQueue();
            }
        }
    }

    // finished iterating through the bgs with free slots, maybe we need to create a new bg

    Battleground * bg_template = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if(!bg_template)
    {
        TC_LOG_ERROR("bg.battleground","Battleground: Update: bg template not found for %u", bgTypeId);
        return;
    }

    // get the min. players per team, properly for larger arenas as well. (must have full teams for arena matches!)
    uint32 MinPlayersPerTeam = bg_template->GetMinPlayersPerTeam();
    uint32 MaxPlayersPerTeam = bg_template->GetMaxPlayersPerTeam();
    if(bg_template->isBattleground())
    {
        if(sBattlegroundMgr->IsBattleGroundTesting())
            MinPlayersPerTeam = 0;
    }
    if(bg_template->IsArena())
    {
        
        //is this really needed ?
        switch(arenatype)
        {
        case ARENA_TYPE_2v2:
            MaxPlayersPerTeam = 2;
            MinPlayersPerTeam = 2;
            break;
        case ARENA_TYPE_3v3:
            MaxPlayersPerTeam = 3;
            MinPlayersPerTeam = 3;
            break;
        case ARENA_TYPE_5v5:
            MaxPlayersPerTeam = 5;
            MinPlayersPerTeam = 5;
            break;
        }

        if (sBattlegroundMgr->IsArenaTesting())
            MinPlayersPerTeam = 1;
    }

    // found out the minimum and maximum ratings the newly added team should battle against
    // arenaRating is the rating of the latest joined team
    uint32 arenaMinRating = (arenaRating <= sBattlegroundMgr->GetMaxRatingDifference()) ? 0 : arenaRating - sBattlegroundMgr->GetMaxRatingDifference();
    // if no rating is specified, set maxrating to 0
    uint32 arenaMaxRating = (arenaRating == 0)? 0 : arenaRating + sBattlegroundMgr->GetMaxRatingDifference();
    uint32 discardTime = 0;
    // if max rating difference is set and the time past since server startup is greater than the rating discard time
    // (after what time the ratings aren't taken into account when making teams) then
    // the discard time is current_time - time_to_discard, teams that joined after that, will have their ratings taken into account
    // else leave the discard time on 0, this way all ratings will be discarded
    if(sBattlegroundMgr->GetMaxRatingDifference() && GetMSTime() >= sBattlegroundMgr->GetRatingDiscardTimer() && arenatype == ARENA_TYPE_2v2)
        discardTime = GetMSTime() - sBattlegroundMgr->GetRatingDiscardTimer();

    // try to build the selection pools
    bool bAllyOK = BuildSelectionPool(bgTypeId, queue_id, MinPlayersPerTeam, MaxPlayersPerTeam, NORMAL_ALLIANCE, arenatype, isRated, arenaMinRating, arenaMaxRating, discardTime);
    bool bHordeOK = BuildSelectionPool(bgTypeId, queue_id, MinPlayersPerTeam, MaxPlayersPerTeam, NORMAL_HORDE, arenatype, isRated, arenaMinRating, arenaMaxRating, discardTime);

    // if selection pools are ready, create the new bg
    if (bAllyOK && bHordeOK)
    {
        Battleground * bg2 = nullptr;
        // special handling for arenas
        if(bg_template->IsArena())
        {
            // Find a random arena, that can be created
            uint8 arenas[] = {BATTLEGROUND_NA, BATTLEGROUND_BE, BATTLEGROUND_RL};
            uint32 arena_num = urand(0,2);
            if( !(bg2 = sBattlegroundMgr->CreateNewBattleground(arenas[arena_num%3])) &&
                !(bg2 = sBattlegroundMgr->CreateNewBattleground(arenas[(arena_num+1)%3])) &&
                !(bg2 = sBattlegroundMgr->CreateNewBattleground(arenas[(arena_num+2)%3])) )
            {
                TC_LOG_ERROR("bg.battleground","Battleground: couldn't create any arena instance!");
                return;
            }

            // set the MaxPlayersPerTeam values based on arenatype
            // setting the min player values isn't needed, since we won't be using that value later on.
            if(sBattlegroundMgr->IsArenaTesting())
            {
                bg2->SetMaxPlayersPerTeam(1);
                bg2->SetMaxPlayers(2);
            }
            else
            {
                switch(arenatype)
                {
                case ARENA_TYPE_2v2:
                    bg2->SetMaxPlayersPerTeam(2);
                    bg2->SetMaxPlayers(4);
                    break;
                case ARENA_TYPE_3v3:
                    bg2->SetMaxPlayersPerTeam(3);
                    bg2->SetMaxPlayers(6);
                    break;
                case ARENA_TYPE_5v5:
                    bg2->SetMaxPlayersPerTeam(5);
                    bg2->SetMaxPlayers(10);
                    break;
                default:
                    break;
                }
            }
        }
        else
        {
            // create new battleground
            bg2 = sBattlegroundMgr->CreateNewBattleground(bgTypeId);
            if(sBattlegroundMgr->IsBattleGroundTesting())
            {
                bg2->SetMaxPlayersPerTeam(1);
                bg2->SetMaxPlayers(2);
            }
        }

        if(!bg2)
        {
            TC_LOG_ERROR("bg.battleground","Battleground: couldn't create bg %u",bgTypeId);
            return;
        }

        // start the joining of the bg
        bg2->SetStatus(STATUS_WAIT_JOIN);
        bg2->SetQueueType(queue_id);
        // initialize arena / rating info
        bg2->SetArenaType(arenatype);
        // set rating
        bg2->SetRated(isRated);

        std::list<GroupQueueInfo* >::iterator itr;

        // Send amount of invites based on the difference between the sizes of the two faction's queues
        uint32 QUEUED_HORDE = m_SelectionPools[NORMAL_HORDE].SelectedGroups.size();
        uint32 QUEUED_ALLIANCE = m_SelectionPools[NORMAL_ALLIANCE].SelectedGroups.size();
        uint16 maxbginvites = 0;

        if(QUEUED_ALLIANCE <= QUEUED_HORDE)
            maxbginvites = QUEUED_ALLIANCE;
        else
            maxbginvites = QUEUED_HORDE;

        // invite groups from horde selection pool
        uint16 invitecounter = 0;
        for(itr = m_SelectionPools[NORMAL_HORDE].SelectedGroups.begin(); itr != m_SelectionPools[NORMAL_HORDE].SelectedGroups.end(); ++itr)
        {
            if (invitecounter >= maxbginvites)
                return;
            InviteGroupToBG((*itr),bg2,TEAM_HORDE);
            ++invitecounter;
        }

        // invite groups from ally selection pool
        invitecounter = 0;
        for(itr = m_SelectionPools[NORMAL_ALLIANCE].SelectedGroups.begin(); itr != m_SelectionPools[NORMAL_ALLIANCE].SelectedGroups.end(); ++itr)
        {
            if (invitecounter >= maxbginvites)
                return;
            InviteGroupToBG((*itr),bg2,TEAM_ALLIANCE);
            ++invitecounter;
        }

        if (isRated)
        {
            auto itr_alliance = m_SelectionPools[NORMAL_ALLIANCE].SelectedGroups.begin();
            auto itr_horde = m_SelectionPools[NORMAL_HORDE].SelectedGroups.begin();
            (*itr_alliance)->OpponentsTeamRating = (*itr_horde)->ArenaTeamRating;
            (*itr_horde)->OpponentsTeamRating = (*itr_alliance)->ArenaTeamRating;
        }

        bg2->StartBattleground();
    }

    // there weren't enough players for a "normal" match
    // if arena, enable horde versus horde or alliance versus alliance teams here

    else if(bg_template->IsArena())
    {
        bool bOneSideHordeTeam1 = false, bOneSideHordeTeam2 = false;
        bool bOneSideAllyTeam1 = false, bOneSideAllyTeam2 = false;
        bOneSideHordeTeam1 = BuildSelectionPool(bgTypeId, queue_id,MaxPlayersPerTeam,MaxPlayersPerTeam,ONESIDE_HORDE_TEAM1,arenatype, isRated, arenaMinRating, arenaMaxRating, discardTime);
        if(bOneSideHordeTeam1)
        {
            // one team has been selected, find out if other can be selected too
            std::list<GroupQueueInfo* >::iterator itr;
            // temporarily change the team side to enable building the next pool excluding the already selected groups
            for(itr = m_SelectionPools[ONESIDE_HORDE_TEAM1].SelectedGroups.begin(); itr != m_SelectionPools[ONESIDE_HORDE_TEAM1].SelectedGroups.end(); ++itr)
                (*itr)->Team=TEAM_ALLIANCE;

            bOneSideHordeTeam2 = BuildSelectionPool(bgTypeId, queue_id,MaxPlayersPerTeam,MaxPlayersPerTeam,ONESIDE_HORDE_TEAM2,arenatype, isRated, arenaMinRating, arenaMaxRating, discardTime, (*(m_SelectionPools[ONESIDE_HORDE_TEAM1].SelectedGroups.begin()))->ArenaTeamId);

            // change back the team to horde
            for(itr = m_SelectionPools[ONESIDE_HORDE_TEAM1].SelectedGroups.begin(); itr != m_SelectionPools[ONESIDE_HORDE_TEAM1].SelectedGroups.end(); ++itr)
                (*itr)->Team=TEAM_HORDE;

            if(!bOneSideHordeTeam2)
                bOneSideHordeTeam1 = false;
        }
        if(!bOneSideHordeTeam1)
        {
            // check for one sided ally
            bOneSideAllyTeam1 = BuildSelectionPool(bgTypeId, queue_id,MaxPlayersPerTeam,MaxPlayersPerTeam,ONESIDE_ALLIANCE_TEAM1,arenatype, isRated, arenaMinRating, arenaMaxRating, discardTime);
            if(bOneSideAllyTeam1)
            {
                // one team has been selected, find out if other can be selected too
                std::list<GroupQueueInfo* >::iterator itr;
                // temporarily change the team side to enable building the next pool excluding the already selected groups
                for(itr = m_SelectionPools[ONESIDE_ALLIANCE_TEAM1].SelectedGroups.begin(); itr != m_SelectionPools[ONESIDE_ALLIANCE_TEAM1].SelectedGroups.end(); ++itr)
                    (*itr)->Team=TEAM_HORDE;

                bOneSideAllyTeam2 = BuildSelectionPool(bgTypeId, queue_id,MaxPlayersPerTeam,MaxPlayersPerTeam,ONESIDE_ALLIANCE_TEAM2,arenatype, isRated, arenaMinRating, arenaMaxRating, discardTime,(*(m_SelectionPools[ONESIDE_ALLIANCE_TEAM1].SelectedGroups.begin()))->ArenaTeamId);

                // change back the team to ally
                for(itr = m_SelectionPools[ONESIDE_ALLIANCE_TEAM1].SelectedGroups.begin(); itr != m_SelectionPools[ONESIDE_ALLIANCE_TEAM1].SelectedGroups.end(); ++itr)
                    (*itr)->Team=TEAM_ALLIANCE;
            }

            if(!bOneSideAllyTeam2)
                bOneSideAllyTeam1 = false;
        }
        // 1-sided BuildSelectionPool() will work, because the MinPlayersPerTeam == MaxPlayersPerTeam in every arena!!!!
        if( (bOneSideHordeTeam1 && bOneSideHordeTeam2) ||
            (bOneSideAllyTeam1 && bOneSideAllyTeam2) )
        {
            // which side has enough players?
            uint32 side = 0;
            SelectionPoolBuildMode mode1, mode2;
            // find out what pools are we using
            if(bOneSideAllyTeam1 && bOneSideAllyTeam2)
            {
                side = TEAM_ALLIANCE;
                mode1 = ONESIDE_ALLIANCE_TEAM1;
                mode2 = ONESIDE_ALLIANCE_TEAM2;
            }
            else
            {
                side = TEAM_HORDE;
                mode1 = ONESIDE_HORDE_TEAM1;
                mode2 = ONESIDE_HORDE_TEAM2;
            }

            // create random arena
            uint8 arenas[] = {BATTLEGROUND_NA, BATTLEGROUND_BE, BATTLEGROUND_RL};
            uint32 arena_num = urand(0,2);
            Battleground* bg2 = nullptr;
            if( !(bg2 = sBattlegroundMgr->CreateNewBattleground(arenas[arena_num%3])) &&
                !(bg2 = sBattlegroundMgr->CreateNewBattleground(arenas[(arena_num+1)%3])) &&
                !(bg2 = sBattlegroundMgr->CreateNewBattleground(arenas[(arena_num+2)%3])) )
            {
                TC_LOG_ERROR("bg.battleground","Could not create arena.");
                return;
            }

            // init stats
            if(sBattlegroundMgr->IsArenaTesting())
            {
                bg2->SetMaxPlayersPerTeam(1);
                bg2->SetMaxPlayers(2);
            }
            else
            {
                switch(arenatype)
                {
                case ARENA_TYPE_2v2:
                    bg2->SetMaxPlayersPerTeam(2);
                    bg2->SetMaxPlayers(4);
                    break;
                case ARENA_TYPE_3v3:
                    bg2->SetMaxPlayersPerTeam(3);
                    bg2->SetMaxPlayers(6);
                    break;
                case ARENA_TYPE_5v5:
                    bg2->SetMaxPlayersPerTeam(5);
                    bg2->SetMaxPlayers(10);
                    break;
                default:
                    break;
                }
            }

            bg2->SetRated(isRated);

            // assigned team of the other group
            uint32 other_side;
            if(side == TEAM_ALLIANCE)
                other_side = TEAM_HORDE;
            else
                other_side = TEAM_ALLIANCE;

            // start the joining of the bg
            bg2->SetStatus(STATUS_WAIT_JOIN);
            bg2->SetQueueType(queue_id);
            // initialize arena / rating info
            bg2->SetArenaType(arenatype);

            std::list<GroupQueueInfo* >::iterator itr;

            // invite players from the first group as horde players (actually green team)
            for(itr = m_SelectionPools[mode1].SelectedGroups.begin(); itr != m_SelectionPools[mode1].SelectedGroups.end(); ++itr)
            {
                InviteGroupToBG((*itr),bg2,TEAM_HORDE);
            }

            // invite players from the second group as ally players (actually gold team)
            for(itr = m_SelectionPools[mode2].SelectedGroups.begin(); itr != m_SelectionPools[mode2].SelectedGroups.end(); ++itr)
            {
                InviteGroupToBG((*itr),bg2,TEAM_ALLIANCE);
            }

            if (isRated)
            {
                auto itr_alliance = m_SelectionPools[mode1].SelectedGroups.begin();
                auto itr_horde = m_SelectionPools[mode2].SelectedGroups.begin();
                (*itr_alliance)->OpponentsTeamRating = (*itr_horde)->ArenaTeamRating;
                (*itr_horde)->OpponentsTeamRating = (*itr_alliance)->ArenaTeamRating;
            }

            bg2->StartBattleground();
        }
    }
}

/*********************************************************/
/***            BATTLEGROUND QUEUE EVENTS              ***/
/*********************************************************/

bool BGQueueInviteEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* plr = ObjectAccessor::FindConnectedPlayer( m_PlayerGuid );

    // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
    if (!plr)
        return true;

    // Player can be in another BG queue and must be removed in normal way in any case
    // // player is already in battleground ... do nothing (battleground queue status is deleted when player is teleported to BG)
    // if (plr->GetBattlegroundId() > 0)
    //    return true;

    Battleground* bg = sBattlegroundMgr->GetBattleground(m_BgInstanceGUID);
    if (!bg)
        return true;

    uint32 queueSlot = plr->GetBattlegroundQueueIndex(bg->GetTypeID());
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)         // player is in queue
    {
        uint32 bgQueueTypeId = sBattlegroundMgr->BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
        uint32 queueSlot = plr->GetBattlegroundQueueIndex(bgQueueTypeId);
        if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES) // player is in queue
        {
            // check if player is invited to this bg ... this check must be here, because when player leaves queue and joins another, it would cause a problems
            BattlegroundQueue::QueuedPlayersMap const& qpMap = sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId].m_QueuedPlayers[plr->GetBattlegroundQueueIdFromLevel()];
            auto qItr = qpMap.find(m_PlayerGuid);
            if (qItr != qpMap.end() && qItr->second.GroupInfo->IsInvitedToBGInstanceGUID == m_BgInstanceGUID)
            {
                WorldPacket data;
                sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, qItr->second.GroupInfo->Team, queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME/2, 0);
                plr->SendDirectMessage(&data);
            }
        }
    }
    return true;                                            //event will be deleted
}

void BGQueueInviteEvent::Abort(uint64 /*e_time*/)
{
    //this should not be called
    TC_LOG_ERROR("bg.battleground","Battleground invite event ABORTED!");
}

bool BGQueueRemoveEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* plr = ObjectAccessor::FindConnectedPlayer( m_PlayerGuid );
    if (!plr)
        // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
        return true;

    Battleground* bg = sBattlegroundMgr->GetBattleground(m_BgInstanceGUID);
    if (!bg)
        return true;

    uint32 bgQueueTypeId = sBattlegroundMgr->BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
    uint32 queueSlot = plr->GetBattlegroundQueueIndex(bgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES) // player is in queue
    {
        // check if player is invited to this bg ... this check must be here, because when player leaves queue and joins another, it would cause a problems
        auto qMapItr = sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId].m_QueuedPlayers[plr->GetBattlegroundQueueIdFromLevel()].find(m_PlayerGuid);
        if (qMapItr != sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId].m_QueuedPlayers[plr->GetBattlegroundQueueIdFromLevel()].end() && qMapItr->second.GroupInfo && qMapItr->second.GroupInfo->IsInvitedToBGInstanceGUID == m_BgInstanceGUID)
        {
            if (qMapItr->second.GroupInfo->IsRated)
            {
                ArenaTeam * at = sObjectMgr->GetArenaTeamById(qMapItr->second.GroupInfo->ArenaTeamId);
                if (at)
                {
                    at->MemberLost(plr, qMapItr->second.GroupInfo->OpponentsTeamRating);
                    at->SaveToDB();
                }
            }
            plr->RemoveBattlegroundQueueId(bgQueueTypeId);
            sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId].RemovePlayer(m_PlayerGuid, true);
            sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId].Update(bgQueueTypeId, bg->GetQueueType());
            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, m_PlayersTeam, queueSlot, STATUS_NONE, 0, 0);
            plr->SendDirectMessage(&data);
        }
    }

    //event will be deleted
    return true;
}

void BGQueueRemoveEvent::Abort(uint64 /*e_time*/)
{
    //this should not be called
    TC_LOG_ERROR("bg.battleground","Battleground remove event ABORTED!");
}

/*********************************************************/
/***            BATTLEGROUND MANAGER                   ***/
/*********************************************************/

BattlegroundMgr::BattlegroundMgr()
{
    m_Battlegrounds.clear();
    m_AutoDistributePoints = (bool)sWorld->getConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_POINTS);
    m_MaxRatingDifference = sWorld->getConfig(CONFIG_ARENA_MAX_RATING_DIFFERENCE);
    m_RatingDiscardTimer = sWorld->getConfig(CONFIG_ARENA_RATING_DISCARD_TIMER);
    m_PrematureFinishTimer = sWorld->getConfig(CONFIG_BATTLEGROUND_PREMATURE_FINISH_TIMER);
    m_NextRatingDiscardUpdate = m_RatingDiscardTimer;
    m_AutoDistributionTimeChecker = 0;
    m_ArenaTesting = false;
    m_BattleGroundTesting = false;
}

BattlegroundMgr::~BattlegroundMgr()
{
    DeleteAllBattlegrounds();
}

void BattlegroundMgr::DeleteAllBattlegrounds()
{
    for(auto itr = m_Battlegrounds.begin(); itr != m_Battlegrounds.end();)
    {
        Battleground * bg = itr->second;
        m_Battlegrounds.erase(itr++);
        delete bg;
    }

    // destroy template battlegrounds that listed only in queues (other already terminated)
    for(auto & bgTypeId : BGFreeSlotQueue)
    {
        // ~Battleground call unregistring BG from queue
        while(!bgTypeId.empty())
            delete bgTypeId.front();
    }
}

// used to update running battlegrounds, and delete finished ones
void BattlegroundMgr::Update(time_t diff)
{
    BattlegroundSet::iterator itr, next;
    for(itr = m_Battlegrounds.begin(); itr != m_Battlegrounds.end(); itr = next)
    {
        next = itr;
        ++next;
        itr->second->Update(diff);
        // use the SetDeleteThis variable
        // direct deletion caused crashes
        if(itr->second->m_SetDeleteThis)
        {
            Battleground * bg = itr->second;
            m_Battlegrounds.erase(itr);
            delete bg;
        }
    }
    // if rating difference counts, maybe force-update queues
    if(m_MaxRatingDifference)
    {
        // it's time to force update
        if(m_NextRatingDiscardUpdate < diff)
        {
            // forced update for level 70 rated arenas
            m_BattlegroundQueues[BATTLEGROUND_QUEUE_2v2].Update(BATTLEGROUND_AA,6,ARENA_TYPE_2v2,true,0);
            m_BattlegroundQueues[BATTLEGROUND_QUEUE_3v3].Update(BATTLEGROUND_AA,6,ARENA_TYPE_3v3,true,0);
            m_BattlegroundQueues[BATTLEGROUND_QUEUE_5v5].Update(BATTLEGROUND_AA,6,ARENA_TYPE_5v5,true,0);
            m_NextRatingDiscardUpdate = m_RatingDiscardTimer;
        }
        else
            m_NextRatingDiscardUpdate -= diff;
    }
    if(m_AutoDistributePoints)
    {
        if(m_AutoDistributionTimeChecker < diff)
        {
            if(time(nullptr) > m_NextAutoDistributionTime)
            {
                DistributeArenaPoints();
                m_NextAutoDistributionTime = time(nullptr) + BATTLEGROUND_ARENA_POINT_DISTRIBUTION_DAY * sWorld->getConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_INTERVAL_DAYS);
                CharacterDatabase.PExecute("UPDATE saved_variables SET NextArenaPointDistributionTime = '" UI64FMTD "'", m_NextAutoDistributionTime);
            }
            m_AutoDistributionTimeChecker = 600000; // check 10 minutes
        }
        else
            m_AutoDistributionTimeChecker -= diff;
    }
}

void BattlegroundMgr::BuildBattlegroundStatusPacket(WorldPacket *data, Battleground *bg, uint32 team, uint8 QueueSlot, uint8 StatusID, uint32 Time1, uint32 Time2, uint32 arenatype, uint8 israted)
{
    // we can be in 3 queues in same time...
    if(StatusID == 0)
    {
        data->Initialize(SMSG_BATTLEFIELD_STATUS, 4*3);
        *data << uint32(QueueSlot);                         // queue id (0...2)
        *data << uint64(0);
        return;
    }

    data->Initialize(SMSG_BATTLEFIELD_STATUS, (4+1+1+4+2+4+1+4+4+4));
    *data << uint32(QueueSlot);                             // queue id (0...2) - player can be in 3 queues in time
    // uint64 in client
    *data << uint64( uint64(arenatype ? arenatype : bg->GetArenaType()) | (uint64(0x0D) << 8) | (uint64(bg->GetTypeID()) << 16) | (uint64(0x1F90) << 48) );
    *data << uint32(0);                                     // unknown
    // alliance/horde for BG and skirmish/rated for Arenas
    *data << uint8(bg->IsArena() ? ( israted ? israted : bg->isRated() ) : bg->GetTeamIndexByTeamId(team));
/*    *data << uint8(arenatype ? arenatype : bg->GetArenaType());                     // team type (0=BG, 2=2x2, 3=3x3, 5=5x5), for arenas    // NOT PROPER VALUE IF ARENA ISN'T RUNNING YET!!!!
    switch(bg->GetTypeID())                                 // value depends on bg id
    {
        case BATTLEGROUND_AV:
            *data << uint8(1);
            break;
        case BATTLEGROUND_WS:
            *data << uint8(2);
            break;
        case BATTLEGROUND_AB:
            *data << uint8(3);
            break;
        case BATTLEGROUND_NA:
            *data << uint8(4);
            break;
        case BATTLEGROUND_BE:
            *data << uint8(5);
            break;
        case BATTLEGROUND_AA:
            *data << uint8(6);
            break;
        case BATTLEGROUND_EY:
            *data << uint8(7);
            break;
        case BATTLEGROUND_RL:
            *data << uint8(8);
            break;
        default:                                            // unknown
            *data << uint8(0);
            break;
    }

    if(bg->IsArena() && (StatusID == STATUS_WAIT_QUEUE))
        *data << uint32(BATTLEGROUND_AA);                   // all arenas   I don't think so.
    else
    *data << uint32(bg->GetTypeID());                   // BG id from DBC

    *data << uint16(0x1F90);                                // unk value 8080
    *data << uint32(bg->GetInstanceID());                   // instance id

    if(bg->isBattleground())
        *data << uint8(bg->GetTeamIndexByTeamId(team));     // team
    else
        *data << uint8(israted?israted:bg->isRated());                      // is rated battle
*/
    *data << uint32(StatusID);                              // status
    switch(StatusID)
    {
        case STATUS_WAIT_QUEUE:                             // status_in_queue
            *data << uint32(Time1);                         // average wait time, milliseconds
            *data << uint32(Time2);                         // time in queue, updated every minute?
            break;
        case STATUS_WAIT_JOIN:                              // status_invite
            *data << uint32(bg->GetMapId());                // map id
            *data << uint32(Time1);                         // time to remove from queue, milliseconds
            break;
        case STATUS_IN_PROGRESS:                            // status_in_progress
            *data << uint32(bg->GetMapId());                // map id
            *data << uint32(Time1);                         // 0 at bg start, 120000 after bg end, time to bg auto leave, milliseconds
            *data << uint32(Time2);                         // time from bg start, milliseconds
            *data << uint8(0x1);                            // unk sometimes 0x0!
            break;
        default:
            TC_LOG_ERROR("bg.battleground","Unknown BG status!");
            break;
    }
}

void BattlegroundMgr::BuildPvpLogDataPacket(WorldPacket *data, Battleground *bg)
{
    uint32 plScSize = bg->GetPlayerScoresSize();
    data->Initialize(MSG_PVP_LOG_DATA, (1+1+4+4*plScSize));
    
    uint8 type = (bg->IsArena() ? 1 : 0);
    *data << uint8(type); // battleground = 0 / arena = 1

    if (type) { // arena
        // it seems this must be according to BG_WINNER_A/H and _NOT_ BG_TEAM_A/H
        for (int i = 1; i >= 0; --i) {
            *data << uint32(3000 - bg->m_ArenaTeamRatingChanges[i]); // rating change: showed value - 3000
            *data << uint32(3999); // huge thanks to TOM_RUS for this!
        }
        
        for (int i = 1; i >= 0; --i) {
            uint32 teamId = bg->m_ArenaTeamIds[i];
            ArenaTeam* at = sObjectMgr->GetArenaTeamById(teamId);
            if (at)
                *data << at->GetName();
            else
                *data << uint8(0);
        }
    }

    uint8 winner = bg->GetWinner();
    if (winner == 2)
        *data << uint8(0); // bg in progress
    else {
        *data << uint8(1); // bg ended
        *data << uint8(winner); // who win
    }

    *data << plScSize;

    for (auto itr = bg->GetPlayerScoresBegin(); itr != bg->GetPlayerScoresEnd(); ++itr) {
        *data << uint64(itr->first); // GUID
        *data << uint32(itr->second->KillingBlows);
        
        if (type) { // Arena
            Player* player = ObjectAccessor::FindConnectedPlayer(itr->first);
            uint32 team = bg->GetPlayerTeam(itr->first);;
            
            if (player) {
                if (!team)
                    team = player->GetTeam();
                
                if (team == TEAM_ALLIANCE)
                    *data << uint8(1);
                else
                    *data << uint8(0);
            } else
                *data << uint8(0);
        } else { // Battleground
            *data << uint32(itr->second->HonorableKills);
            *data << uint32(itr->second->Deaths);
            *data << uint32(itr->second->BonusHonor);
        }
        
        *data << uint32(itr->second->DamageDone);
        *data << uint32(itr->second->HealingDone);
        
        switch (bg->GetTypeID()) // battleground specific things
        {
        case BATTLEGROUND_AV:
            *data << uint32(5); // count of next fields
            *data << uint32(((BattlegroundAVScore*) itr->second)->GraveyardsAssaulted);
            *data << uint32(((BattlegroundAVScore*) itr->second)->GraveyardsDefended);
            *data << uint32(((BattlegroundAVScore*) itr->second)->TowersAssaulted);
            *data << uint32(((BattlegroundAVScore*) itr->second)->TowersDefended);
            *data << uint32(((BattlegroundAVScore*) itr->second)->MinesCaptured);
            break;
        case BATTLEGROUND_WS:
            *data << uint32(2); // count of next fields
            *data << uint32(((BattlegroundWGScore*) itr->second)->FlagCaptures);
            *data << uint32(((BattlegroundWGScore*) itr->second)->FlagReturns);
            break;
        case BATTLEGROUND_AB:
            *data << uint32(2); // count of next fields
            *data << uint32(((BattlegroundABScore*) itr->second)->BasesAssaulted);
            *data << uint32(((BattlegroundABScore*) itr->second)->BasesDefended);
            break;
        case BATTLEGROUND_EY:
            *data << uint32(1); // count of next fields
            *data << uint32(((BattlegroundEYScore*) itr->second)->FlagCaptures);
            break;
        case BATTLEGROUND_NA:
        case BATTLEGROUND_BE:
        case BATTLEGROUND_AA:
        case BATTLEGROUND_RL:
            *data << uint32(0); // count of next fields
            break;
        default:
            TC_LOG_ERROR("bg.battleground","Unhandled MSG_PVP_LOG_DATA for BG id %u", bg->GetTypeID());
            *data << uint32(0);
            break;
        }
    }
}

void BattlegroundMgr::BuildGroupJoinedBattlegroundPacket(WorldPacket *data, uint32 bgTypeId)
{
    /*bgTypeId is:
    0 - Your group has joined a battleground queue, but you are not eligible
    1 - Your group has joined the queue for AV
    2 - Your group has joined the queue for WS
    3 - Your group has joined the queue for AB
    4 - Your group has joined the queue for NA
    5 - Your group has joined the queue for BE Arena
    6 - Your group has joined the queue for All Arenas
    7 - Your group has joined the queue for EotS*/
    data->Initialize(SMSG_GROUP_JOINED_BATTLEGROUND, 4);
    *data << uint32(bgTypeId);
}

void BattlegroundMgr::BuildUpdateWorldStatePacket(WorldPacket *data, uint32 field, uint32 value)
{
    data->Initialize(SMSG_UPDATE_WORLD_STATE, 4+4);
    *data << uint32(field);
    *data << uint32(value);
}

void BattlegroundMgr::BuildPlaySoundPacket(WorldPacket *data, uint32 soundid)
{
    data->Initialize(SMSG_PLAY_SOUND, 4);
    *data << uint32(soundid);
}

void BattlegroundMgr::BuildPlayerLeftBattlegroundPacket(WorldPacket *data, Player *plr)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_LEFT, 8);
    *data << uint64(plr->GetGUID());
}

void BattlegroundMgr::BuildPlayerJoinedBattlegroundPacket(WorldPacket *data, Player *plr)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_JOINED, 8);
    *data << uint64(plr->GetGUID());
}

void BattlegroundMgr::InvitePlayer(Player* plr, uint32 bgInstanceGUID, uint32 team)
{
    // set invited player counters:
    Battleground* bg = GetBattleground(bgInstanceGUID);
    if(!bg)
        return;
    bg->IncreaseInvitedCount(team);
    if (bg->IsArena() && bg->isRated())
        bg->PlayerInvitedInRatedArena(plr, team);

    plr->SetInviteForBattlegroundQueueType(BGQueueTypeId(bg->GetTypeID(),bg->GetArenaType()), bgInstanceGUID);

    // set the arena teams for rated matches
    if(bg->IsArena() && bg->isRated())
    {
        switch(bg->GetArenaType())
        {
        case ARENA_TYPE_2v2:
            bg->SetArenaTeamIdForTeam(team, plr->GetArenaTeamId(0));
            break;
        case ARENA_TYPE_3v3:
            bg->SetArenaTeamIdForTeam(team, plr->GetArenaTeamId(1));
            break;
        case ARENA_TYPE_5v5:
            bg->SetArenaTeamIdForTeam(team, plr->GetArenaTeamId(2));
            break;
        default:
            break;
        }
    }

    // create invite events:
    //add events to player's counters ---- this is not good way - there should be something like global event processor, where we should add those events
    auto  inviteEvent = new BGQueueInviteEvent(plr->GetGUID(), bgInstanceGUID);
    plr->m_Events.AddEvent(inviteEvent, plr->m_Events.CalculateTime(INVITE_ACCEPT_WAIT_TIME/2));
    auto  removeEvent = new BGQueueRemoveEvent(plr->GetGUID(), bgInstanceGUID, team);
    plr->m_Events.AddEvent(removeEvent, plr->m_Events.CalculateTime(INVITE_ACCEPT_WAIT_TIME));
}

Battleground * BattlegroundMgr::GetBattlegroundTemplate(uint32 bgTypeId)
{
    return BGFreeSlotQueue[bgTypeId].empty() ? nullptr : BGFreeSlotQueue[bgTypeId].back();
}

// create a new battleground that will really be used to play
Battleground * BattlegroundMgr::CreateNewBattleground(uint32 bgTypeId)
{
    Battleground *bg = nullptr;

    // get the template BG
    Battleground *bg_template = GetBattlegroundTemplate(bgTypeId);

    if(!bg_template)
    {
        TC_LOG_ERROR("bg.battleground","Battleground: CreateNewBattleground - bg template not found for %u", bgTypeId);
        return nullptr;
    }

    // create a copy of the BG template
    switch(bgTypeId)
    {
        case BATTLEGROUND_AV:
            bg = new BattlegroundAV(*(BattlegroundAV*)bg_template);
            break;
        case BATTLEGROUND_WS:
            bg = new BattlegroundWS(*(BattlegroundWS*)bg_template);
            break;
        case BATTLEGROUND_AB:
            bg = new BattlegroundAB(*(BattlegroundAB*)bg_template);
            break;
        case BATTLEGROUND_NA:
            bg = new BattlegroundNA(*(BattlegroundNA*)bg_template);
            break;
        case BATTLEGROUND_BE:
            bg = new BattlegroundBE(*(BattlegroundBE*)bg_template);
            break;
        case BATTLEGROUND_AA:
            bg = new BattlegroundAA(*(BattlegroundAA*)bg_template);
            break;
        case BATTLEGROUND_EY:
            bg = new BattlegroundEY(*(BattlegroundEY*)bg_template);
            break;
        case BATTLEGROUND_RL:
            bg = new BattlegroundRL(*(BattlegroundRL*)bg_template);
            break;
        default:
            //bg = new Battleground;
            return nullptr;
            break;             // placeholder for non implemented BG
    }

    // generate a new instance id
    bg->SetInstanceID(sMapMgr->GenerateInstanceId()); // set instance id

    // reset the new bg (set status to status_wait_queue from status_none)
    bg->Reset();

    /*   will be setup in BG::Update() when the first player is ported in
    if(!(bg->SetupBattleground()))
    {
        TC_LOG_ERROR("bg.battleground","Battleground: CreateNewBattleground: SetupBattleground failed for bg %u", bgTypeId);
        delete bg;
        return 0;
    }
    */

    // add BG to free slot queue
    bg->AddToBGFreeSlotQueue();

    // add bg to update list
    AddBattleground(bg->GetInstanceID(), bg);

    return bg;
}

// used to create the BG templates
uint32 BattlegroundMgr::CreateBattleground(uint32 bgTypeId, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam, uint32 LevelMin, uint32 LevelMax, char* BattlegroundName, uint32 MapID, float Team1StartLocX, float Team1StartLocY, float Team1StartLocZ, float Team1StartLocO, float Team2StartLocX, float Team2StartLocY, float Team2StartLocZ, float Team2StartLocO)
{
    // Create the BG
    Battleground *bg = nullptr;

    switch(bgTypeId)
    {
        case BATTLEGROUND_AV: bg = new BattlegroundAV; break;
        case BATTLEGROUND_WS: bg = new BattlegroundWS; break;
        case BATTLEGROUND_AB: bg = new BattlegroundAB; break;
        case BATTLEGROUND_NA: bg = new BattlegroundNA; break;
        case BATTLEGROUND_BE: bg = new BattlegroundBE; break;
        case BATTLEGROUND_AA: bg = new BattlegroundAA; break;
        case BATTLEGROUND_EY: bg = new BattlegroundEY; break;
        case BATTLEGROUND_RL: bg = new BattlegroundRL; break;
        default:bg = new Battleground;   break;             // placeholder for non implemented BG
    }

    bg->SetMapId(MapID);

    bg->Reset();

    BattlemasterListEntry const *bl = sBattlemasterListStore.LookupEntry(bgTypeId);
    //in previous method is checked if exists entry in sBattlemasterListStore, so no check needed
    if (bl)
    {
        bg->SetArenaorBGType(bl->type == TYPE_ARENA);
    }

    bg->SetTypeID(bgTypeId);
    bg->SetInstanceID(0);                               // template bg, instance id is 0
    bg->SetMinPlayersPerTeam(MinPlayersPerTeam);
    bg->SetMaxPlayersPerTeam(MaxPlayersPerTeam);
    bg->SetMinPlayers(MinPlayersPerTeam*2);
    bg->SetMaxPlayers(MaxPlayersPerTeam*2);
    bg->SetName(BattlegroundName);
    bg->SetTeamStartLoc(TEAM_ALLIANCE, Team1StartLocX, Team1StartLocY, Team1StartLocZ, Team1StartLocO);
    bg->SetTeamStartLoc(TEAM_HORDE,    Team2StartLocX, Team2StartLocY, Team2StartLocZ, Team2StartLocO);
    bg->SetLevelRange(LevelMin, LevelMax);
    if(bl->type == TYPE_ARENA)
        bg->SetTimeLimit(sWorld->getConfig(CONFIG_BATTLEGROUND_TIMELIMIT_ARENA)*1000);
    else if(bgTypeId == BATTLEGROUND_WS)
        bg->SetTimeLimit(sWorld->getConfig(CONFIG_BATTLEGROUND_TIMELIMIT_WARSONG)*1000);

    //add Battleground instance to FreeSlotQueue (.back() will return the template!)
    bg->AddToBGFreeSlotQueue();

    // do NOT add to update list, since this is a template battleground!

    // return some not-null value, bgTypeId is good enough for me
    return bgTypeId;
}

void BattlegroundMgr::CreateInitialBattlegrounds()
{
    float AStartLoc[4];
    float HStartLoc[4];
    uint32 MaxPlayersPerTeam, MinPlayersPerTeam, MinLvl, MaxLvl, start1, start2;
    BattlemasterListEntry const *bl;
    WorldSafeLocsEntry const *start;

    uint32 count = 0;

    //                                                0   1                 2                 3      4      5                6              7             8
    QueryResult result = WorldDatabase.Query("SELECT id, MinPlayersPerTeam,MaxPlayersPerTeam,MinLvl,MaxLvl,AllianceStartLoc,AllianceStartO,HordeStartLoc,HordeStartO FROM battleground_template");

    if(!result)
    {
        TC_LOG_INFO("bg.battleground"," ");
        TC_LOG_ERROR("battleground",">> Loaded 0 battlegrounds. DB table `battleground_template` is empty.");
        return;
    }

    do
    {
        Field *fields = result->Fetch();

        uint32 bgTypeID = fields[0].GetUInt32();

        // can be overwrited by values from DB
        bl = sBattlemasterListStore.LookupEntry(bgTypeID);
        if(!bl)
        {
            TC_LOG_ERROR("bg.battleground","Battleground ID %u not found in BattlemasterList.dbc. Battleground not created.",bgTypeID);
            continue;
        }

        MaxPlayersPerTeam = bl->maxplayersperteam;
        MinPlayersPerTeam = bl->maxplayersperteam/2;
        MinLvl = bl->minlvl;
        MaxLvl = bl->maxlvl;

        if(fields[1].GetUInt16())
            MinPlayersPerTeam = fields[1].GetUInt16();

        if(fields[2].GetUInt16())
            MaxPlayersPerTeam = fields[2].GetUInt16();

        if(fields[3].GetUInt8())
            MinLvl = fields[3].GetUInt8();

        if(fields[4].GetUInt8())
            MaxLvl = fields[4].GetUInt8();

        start1 = fields[5].GetUInt32();

        start = sWorldSafeLocsStore.LookupEntry(start1);
        if(start)
        {
            AStartLoc[0] = start->x;
            AStartLoc[1] = start->y;
            AStartLoc[2] = start->z;
            AStartLoc[3] = fields[6].GetFloat();
        }
        else if(bgTypeID == BATTLEGROUND_AA)
        {
            AStartLoc[0] = 0;
            AStartLoc[1] = 0;
            AStartLoc[2] = 0;
            AStartLoc[3] = fields[6].GetFloat();
        }
        else
        {
            TC_LOG_ERROR("bg.battleground","Table `battleground_template` for id %u have non-existed WorldSafeLocs.dbc id %u in field `AllianceStartLoc`. BG not created.",bgTypeID,start1);
            continue;
        }

        start2 = fields[7].GetUInt32();

        start = sWorldSafeLocsStore.LookupEntry(start2);
        if(start)
        {
            HStartLoc[0] = start->x;
            HStartLoc[1] = start->y;
            HStartLoc[2] = start->z;
            HStartLoc[3] = fields[8].GetFloat();
        }
        else if(bgTypeID == BATTLEGROUND_AA)
        {
            HStartLoc[0] = 0;
            HStartLoc[1] = 0;
            HStartLoc[2] = 0;
            HStartLoc[3] = fields[8].GetFloat();
        }
        else
        {
            TC_LOG_ERROR("bg.battleground","Table `battleground_template` for id %u have non-existed WorldSafeLocs.dbc id %u in field `HordeStartLoc`. BG not created.",bgTypeID,start2);
            continue;
        }

        //TC_LOG_DEBUG("bg.battleground","Creating battleground %s, %u-%u", bl->name[sWorld->GetDBClang()], MinLvl, MaxLvl);
        if(!CreateBattleground(bgTypeID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl, bl->name[sWorld->GetDefaultDbcLocale()], bl->mapid[0], AStartLoc[0], AStartLoc[1], AStartLoc[2], AStartLoc[3], HStartLoc[0], HStartLoc[1], HStartLoc[2], HStartLoc[3]))
            continue;

        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("bg.battleground"," ");
    TC_LOG_INFO("battleground", ">> Loaded %u battlegrounds", count );
}

void BattlegroundMgr::InitAutomaticArenaPointDistribution()
{
    if(m_AutoDistributePoints)
    {
        QueryResult  result = CharacterDatabase.Query("SELECT NextArenaPointDistributionTime FROM saved_variables");
        if(!result)
        {
            TC_LOG_ERROR("battleground","Battleground: Next arena point distribution time not found in SavedVariables, reseting it now.");
            m_NextAutoDistributionTime = time(nullptr) + BATTLEGROUND_ARENA_POINT_DISTRIBUTION_DAY * sWorld->getConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_INTERVAL_DAYS);
            CharacterDatabase.PExecute("INSERT INTO saved_variables (NextArenaPointDistributionTime) VALUES ('" UI64FMTD "')", m_NextAutoDistributionTime);
        }
        else
        {
            m_NextAutoDistributionTime = (*result)[0].GetUInt64();
        }
    }
}

void BattlegroundMgr::DistributeArenaPoints()
{
    // used to distribute arena points based on last week's stats
    sWorld->SendGlobalText("Flushing Arena points based on team ratings, this may take a few minutes. Please stand by...", nullptr);

    sWorld->SendGlobalText("Distributing arena points to players...", nullptr);

    //temporary structure for storing maximum points to add values for all players
    std::map<uint32, uint32> PlayerPoints;

    //at first update all points for all team members
    for(auto team_itr = sObjectMgr->GetArenaTeamMapBegin(); team_itr != sObjectMgr->GetArenaTeamMapEnd(); ++team_itr)
    {
        if(ArenaTeam * at = team_itr->second)
        {
            at->UpdateArenaPointsHelper(PlayerPoints);
        }
    }

    //cycle that gives points to all players
    for (auto & PlayerPoint : PlayerPoints)
    {
        // Update database
        CharacterDatabase.PExecute("UPDATE characters SET arenaPoints = arenaPoints + '%u' WHERE guid = '%u'", PlayerPoint.second, PlayerPoint.first);
        
        // Add points if player is online
        Player* pl = ObjectAccessor::FindConnectedPlayer(PlayerPoint.first);
        if (pl)
            pl->ModifyArenaPoints(PlayerPoint.second);
    }

    PlayerPoints.clear();

    sWorld->SendGlobalText("Finished setting arena points for online players.", nullptr);

    sWorld->SendGlobalText("Modifying played count, arena points etc. for loaded arena teams, sending updated stats to online players...", nullptr);
    for(auto titr = sObjectMgr->GetArenaTeamMapBegin(); titr != sObjectMgr->GetArenaTeamMapEnd(); ++titr)
    {
        if(ArenaTeam * at = titr->second)
        {
            if(at->GetType() == ARENA_TEAM_2v2 && sWorld->getConfig(CONFIG_ARENA_DECAY_ENABLED))
                at->HandleDecay();
           
            at->FinishWeek();                              // set played this week etc values to 0 in memory, too
            at->SaveToDB();                                // save changes
            at->NotifyStatsChanged();                      // notify the players of the changes
        }
    }

    if(sWorld->getConfig(CONFIG_ARENA_NEW_TITLE_DISTRIB))
        sWorld->updateArenaLeadersTitles();

    sWorld->SendGlobalText("Modification done.", nullptr);

    sWorld->SendGlobalText("Done flushing Arena points.", nullptr);
}

void BattlegroundMgr::BuildBattlegroundListPacket(WorldPacket *data, uint64 guid, Player* plr, uint32 bgTypeId)
{
    uint32 PlayerLevel = 10;

    if(plr)
        PlayerLevel = plr->GetLevel();

    data->Initialize(SMSG_BATTLEFIELD_LIST);
    *data << uint64(guid);                                  // battlemaster guid
    *data << uint32(bgTypeId);                              // battleground id
    if(bgTypeId == BATTLEGROUND_AA)                         // arena
    {
        *data << uint8(5);                                  // unk
        *data << uint32(0);                                 // unk
    }
    else                                                    // battleground
    {
        *data << uint8(0x00);                               // unk (bool to remove join as group button ?)

        size_t count_pos = data->wpos();
        uint32 count = 0;
        *data << uint32(0x00);                              // number of bg instances

        for(auto & m_Battleground : m_Battlegrounds)
        {
            if(m_Battleground.second->GetTypeID() == bgTypeId && (PlayerLevel >= m_Battleground.second->GetMinLevel()) && (PlayerLevel <= m_Battleground.second->GetMaxLevel()))
            {
                *data << uint32(m_Battleground.second->GetInstanceID());
                ++count;
            }
        }
        data->put<uint32>( count_pos , count);
    }
}

void BattlegroundMgr::SendToBattleground(Player *pl, uint32 instanceId)
{
    Battleground *bg = GetBattleground(instanceId);
    if(bg)
    {
        uint32 mapid = bg->GetMapId();
        float x, y, z, O;
        uint32 team = pl->GetBGTeam();
        if(team==0)
            team = pl->GetTeam();
        bg->GetTeamStartLoc(team, x, y, z, O);

        TC_LOG_DEBUG("battleground","BATTLEGROUND: Sending %s to map %u, X %f, Y %f, Z %f, O %f", pl->GetName().c_str(), mapid, x, y, z, O);
        pl->TeleportTo(mapid, x, y, z, O);
    }
    else
    {
        TC_LOG_ERROR("battleground","player %u trying to port to non-existent bg instance %u",pl->GetGUIDLow(), instanceId);
    }
}

void BattlegroundMgr::SendAreaSpiritHealerQueryOpcode(Player *pl, Battleground *bg, uint64 guid)
{
    WorldPacket data(SMSG_AREA_SPIRIT_HEALER_TIME, 12);
    uint32 time_ = 30000 - bg->GetLastResurrectTime();      // resurrect every 30 seconds
    if(time_ == uint32(-1))
        time_ = 0;
    data << guid << time_;
    pl->SendDirectMessage(&data);
}

void BattlegroundMgr::RemoveBattleground(uint32 instanceID)
{
    auto itr = m_Battlegrounds.find(instanceID);
    if(itr!=m_Battlegrounds.end())
        m_Battlegrounds.erase(itr);
}

bool BattlegroundMgr::IsArenaType(uint32 bgTypeId) const
{
    return ( bgTypeId == BATTLEGROUND_AA ||
        bgTypeId == BATTLEGROUND_BE ||
        bgTypeId == BATTLEGROUND_NA ||
        bgTypeId == BATTLEGROUND_RL );
}

bool BattlegroundMgr::IsBattlegroundType(uint32 bgTypeId) const
{
    return !IsArenaType(bgTypeId);
}

uint32 BattlegroundMgr::BGQueueTypeId(uint32 bgTypeId, uint8 arenaType)
{
    switch(bgTypeId)
    {
    case BATTLEGROUND_WS:
        return BATTLEGROUND_QUEUE_WS;
    case BATTLEGROUND_AB:
        return BATTLEGROUND_QUEUE_AB;
    case BATTLEGROUND_AV:
        return BATTLEGROUND_QUEUE_AV;
    case BATTLEGROUND_EY:
        return BATTLEGROUND_QUEUE_EY;
    case BATTLEGROUND_AA:
    case BATTLEGROUND_NA:
    case BATTLEGROUND_RL:
    case BATTLEGROUND_BE:
        switch(arenaType)
        {
        case ARENA_TYPE_2v2:
            return BATTLEGROUND_QUEUE_2v2;
        case ARENA_TYPE_3v3:
            return BATTLEGROUND_QUEUE_3v3;
        case ARENA_TYPE_5v5:
            return BATTLEGROUND_QUEUE_5v5;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

uint32 BattlegroundMgr::BGTemplateId(uint32 bgQueueTypeId) const
{
    switch(bgQueueTypeId)
    {
    case BATTLEGROUND_QUEUE_WS:
        return BATTLEGROUND_WS;
    case BATTLEGROUND_QUEUE_AB:
        return BATTLEGROUND_AB;
    case BATTLEGROUND_QUEUE_AV:
        return BATTLEGROUND_AV;
    case BATTLEGROUND_QUEUE_EY:
        return BATTLEGROUND_EY;
    case BATTLEGROUND_QUEUE_2v2:
    case BATTLEGROUND_QUEUE_3v3:
    case BATTLEGROUND_QUEUE_5v5:
        return BATTLEGROUND_AA;
    default:
        return 0;
    }
}

uint8 BattlegroundMgr::BGArenaType(uint32 bgQueueTypeId) const
{
    switch(bgQueueTypeId)
    {
    case BATTLEGROUND_QUEUE_2v2:
        return ARENA_TYPE_2v2;
    case BATTLEGROUND_QUEUE_3v3:
        return ARENA_TYPE_3v3;
    case BATTLEGROUND_QUEUE_5v5:
        return ARENA_TYPE_5v5;
    default:
        return 0;
    }
}

bool BattlegroundMgr::ToggleArenaTesting()
{
    m_ArenaTesting = !m_ArenaTesting;

    UpdateAllQueues();

    return m_ArenaTesting;
}

bool BattlegroundMgr::ToggleBattleGroundTesting()
{
    m_BattleGroundTesting = !m_BattleGroundTesting;
    
    UpdateAllQueues();

    return m_BattleGroundTesting;
}

//To test
void BattlegroundMgr::UpdateAllQueues()
{
    for(uint32 bgQueueTypeId = 0; bgQueueTypeId < BATTLEGROUND_QUEUE_TYPES_TOTAL; bgQueueTypeId++)
        for(uint32 queueId = 0; queueId < MAX_BATTLEGROUND_QUEUE_RANGES; queueId++)
            sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId].Update(bgQueueTypeId, queueId);
}

void BattlegroundMgr::SetHolidayWeekends(uint32 mask)
{
    for(uint32 bgtype = 1; bgtype < MAX_BATTLEGROUND_TYPE_ID; ++bgtype)
    {
        if(Battleground * bg = GetBattlegroundTemplate(bgtype))
        {
            bg->SetHoliday(mask & (1 << bgtype));
        }
    }
}

BattlegroundSet BattlegroundMgr::GetBattlegroundByType(uint32 bgTypeId)
{
    BattlegroundSet Battlegrounds;
    for (auto & m_Battleground : m_Battlegrounds)
    {
        Battleground* bg = m_Battleground.second;
        if (bg->GetTypeID() == bgTypeId)
            Battlegrounds[bg->GetInstanceID()]= bg;
    }

    return Battlegrounds;
}