/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2017  MaNGOS project <https://getmangos.eu>
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
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include <zlib.h>
#include "Common.h"
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseImpl.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "Player.h"
#include "World.h"
#include "GuildMgr.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "Auth/BigNumber.h"
#include "Auth/Sha1.h"
#include "UpdateData.h"
#include "LootMgr.h"
#include "Chat.h"
#include "ScriptMgr.h"
#include "ObjectAccessor.h"
#include "Object.h"
#include "BattleGround/BattleGround.h"
#include "OutdoorPvP/OutdoorPvP.h"
#include "Pet.h"
#include "SocialMgr.h"
#ifdef ENABLE_ELUNA
#include "LuaEngine.h"
#endif /* ENABLE_ELUNA */

void WorldSession::HandleRepopRequestOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_REPOP_REQUEST");

    // recv_data.read_skip<uint8>(); client crash

    if (GetPlayer()->IsAlive() || GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        { return; }

    // the world update order is sessions, players, creatures
    // the netcode runs in parallel with all of these
    // creatures can kill players
    // so if the server is lagging enough the player can
    // release spirit after he's killed but before he is updated
    if (GetPlayer()->GetDeathState() == JUST_DIED)
    {
        DEBUG_LOG("HandleRepopRequestOpcode: got request after player %s(%d) was killed and before he was updated", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow());
        GetPlayer()->KillPlayer();
    }

    // Used by Eluna
#ifdef ENABLE_ELUNA
    sEluna->OnRepop(GetPlayer());
#endif /* ENABLE_ELUNA */

    // this is spirit release confirm?
    GetPlayer()->RemovePet(PET_SAVE_REAGENTS);
    GetPlayer()->BuildPlayerRepop();
    GetPlayer()->RepopAtGraveyard();
}

void WorldSession::HandleWhoOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_WHO");
    // recv_data.hexlike();

    uint32 level_min, level_max, racemask, classmask, zones_count, str_count;
    uint32 zoneids[10];                                     // 10 is client limit
    std::string player_name, guild_name;

    recv_data >> level_min;                                 // maximal player level, default 0
    recv_data >> level_max;                                 // minimal player level, default 100 (MAX_LEVEL)
    recv_data >> player_name;                               // player name, case sensitive...

    recv_data >> guild_name;                                // guild name, case sensitive...

    recv_data >> racemask;                                  // race mask
    recv_data >> classmask;                                 // class mask
    recv_data >> zones_count;                               // zones count, client limit=10 (2.0.10)

    if (zones_count > 10)
        { return; }                                             // can't be received from real client or broken packet

    for (uint32 i = 0; i < zones_count; ++i)
    {
        uint32 temp;
        recv_data >> temp;                                  // zone id, 0 if zone is unknown...
        zoneids[i] = temp;
        DEBUG_LOG("Zone %u: %u", i, zoneids[i]);
    }

    recv_data >> str_count;                                 // user entered strings count, client limit=4 (checked on 2.0.10)

    if (str_count > 4)
        { return; }                                             // can't be received from real client or broken packet

    DEBUG_LOG("Minlvl %u, maxlvl %u, name %s, guild %s, racemask %u, classmask %u, zones %u, strings %u", level_min, level_max, player_name.c_str(), guild_name.c_str(), racemask, classmask, zones_count, str_count);

    std::wstring str[4];                                    // 4 is client limit
    for (uint32 i = 0; i < str_count; ++i)
    {
        std::string temp;
        recv_data >> temp;                                  // user entered string, it used as universal search pattern(guild+player name)?

        if (!Utf8toWStr(temp, str[i]))
            { continue; }

        wstrToLower(str[i]);

        DEBUG_LOG("String %u: %s", i, temp.c_str());
    }

    std::wstring wplayer_name;
    std::wstring wguild_name;
    if (!(Utf8toWStr(player_name, wplayer_name) && Utf8toWStr(guild_name, wguild_name)))
        { return; }
    wstrToLower(wplayer_name);
    wstrToLower(wguild_name);

    // client send in case not set max level value 100 but mangos support 255 max level,
    // update it to show GMs with characters after 100 level
    if (level_max >= MAX_LEVEL)
        { level_max = STRONG_MAX_LEVEL; }

    Team team = _player->GetTeam();
    AccountTypes security = GetSecurity();
    bool allowTwoSideWhoList = sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_WHO_LIST);
    AccountTypes gmLevelInWhoList = (AccountTypes)sWorld.getConfig(CONFIG_UINT32_GM_LEVEL_IN_WHO_LIST);

    uint32 matchcount = 0;
    uint32 displaycount = 0;

    WorldPacket data(SMSG_WHO, 50);                         // guess size
    data << uint32(matchcount);                             // placeholder, count of players matching criteria
    data << uint32(displaycount);                           // placeholder, count of players displayed

    // TODO: Guard Player map
    HashMapHolder<Player>::MapType& m = sObjectAccessor.GetPlayers();
    for (HashMapHolder<Player>::MapType::const_iterator itr = m.begin(); itr != m.end(); ++itr)
    {
        Player* pl = itr->second;

        if (security == SEC_PLAYER)
        {
            // player can see member of other team only if CONFIG_BOOL_ALLOW_TWO_SIDE_WHO_LIST
            if (pl->GetTeam() != team && !allowTwoSideWhoList)
                { continue; }

            // player can see MODERATOR, GAME MASTER, ADMINISTRATOR only if CONFIG_GM_IN_WHO_LIST
            if (pl->GetSession()->GetSecurity() > gmLevelInWhoList)
                { continue; }
        }

        // do not process players which are not in world
        if (!pl->IsInWorld())
            { continue; }

        // check if target is globally visible for player
        if (!pl->IsVisibleGloballyFor(_player))
            { continue; }

        // check if target's level is in level range
        uint32 lvl = pl->getLevel();
        if (lvl < level_min || lvl > level_max)
            { continue; }

        // check if class matches classmask
        uint32 class_ = pl->getClass();
        if (!(classmask & (1 << class_)))
            { continue; }

        // check if race matches racemask
        uint32 race = pl->getRace();
        if (!(racemask & (1 << race)))
            { continue; }

        uint32 pzoneid = pl->GetZoneId();

        bool z_show = true;
        for (uint32 i = 0; i < zones_count; ++i)
        {
            if (zoneids[i] == pzoneid)
            {
                z_show = true;
                break;
            }

            z_show = false;
        }
        if (!z_show)
            { continue; }

        std::string pname = pl->GetName();
        std::wstring wpname;
        if (!Utf8toWStr(pname, wpname))
            { continue; }
        wstrToLower(wpname);

        if (!(wplayer_name.empty() || wpname.find(wplayer_name) != std::wstring::npos))
            { continue; }

        std::string gname = sGuildMgr.GetGuildNameById(pl->GetGuildId());
        std::wstring wgname;
        if (!Utf8toWStr(gname, wgname))
            { continue; }
        wstrToLower(wgname);

        if (!(wguild_name.empty() || wgname.find(wguild_name) != std::wstring::npos))
            { continue; }

        std::string aname;
        if (AreaTableEntry const* areaEntry = GetAreaEntryByAreaID(pzoneid))
            { aname = areaEntry->area_name[GetSessionDbcLocale()]; }

        bool s_show = true;
        for (uint32 i = 0; i < str_count; ++i)
        {
            if (!str[i].empty())
            {
                if (wgname.find(str[i]) != std::wstring::npos ||
                    wpname.find(str[i]) != std::wstring::npos ||
                    Utf8FitTo(aname, str[i]))
                {
                    s_show = true;
                    break;
                }
                s_show = false;
            }
        }
        if (!s_show)
            { continue; }

        // 49 is maximum player count sent to client
        ++matchcount;
        if (matchcount > 49)
            continue;

        ++displaycount;

        data << pname;                                      // player name
        data << gname;                                      // guild name
        data << uint32(lvl);                                // player level
        data << uint32(class_);                             // player class
        data << uint32(race);                               // player race
        data << uint32(pzoneid);                            // player zone id
    }

    data.put(0, displaycount);                              // insert right count, count displayed
    data.put(4, matchcount);                                // insert right count, count of matches

    SendPacket(&data);
    DEBUG_LOG("WORLD: Send SMSG_WHO Message");
}

void WorldSession::HandleLogoutRequestOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_LOGOUT_REQUEST, security %u", GetSecurity());

    if (ObjectGuid lootGuid = GetPlayer()->GetLootGuid())
        { DoLootRelease(lootGuid); }

    uint8 reason = 0;

    if (GetPlayer()->IsInCombat())
    {
        reason = 1;
    }
    else if (GetPlayer()->m_movementInfo.HasMovementFlag(MovementFlags(MOVEFLAG_FALLING | MOVEFLAG_FALLINGFAR)))
    {
        reason = 3;                                         // is jumping or falling
    }
    else if (GetPlayer()->duel || GetPlayer()->HasAura(9454)) // is dueling or frozen by GM via freeze command
    {
        reason = 2;                                         // FIXME - Need the correct value
    }
    
    if (reason)
    {
        WorldPacket data(SMSG_LOGOUT_RESPONSE, 1+4);
        data << uint8(reason);
        data << uint32(0);
        SendPacket(&data);
        LogoutRequest(0);
        return;
    }

    // instant logout in taverns/cities or on taxi or for admins, gm's, mod's if its enabled in mangosd.conf
    if (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || GetPlayer()->IsTaxiFlying())
    {
        WorldPacket data(SMSG_LOGOUT_RESPONSE, 1+4);
        data << uint8(0);
        data << uint32(16777216);
        SendPacket(&data);
        LogoutPlayer(true);
        return;
    }

    // not set flags if player can't free move to prevent lost state at logout cancel
    if (GetPlayer()->CanFreeMove())
    {
        float height = GetPlayer()->GetMap()->GetHeight(GetPlayer()->GetPositionX(), GetPlayer()->GetPositionY(), GetPlayer()->GetPositionZ());
        if ((GetPlayer()->GetPositionZ() < height + 0.1f) && !(GetPlayer()->IsInWater()))
            { GetPlayer()->SetStandState(UNIT_STAND_STATE_SIT); }

        GetPlayer()->SetRoot(true);
        GetPlayer()->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    WorldPacket data(SMSG_LOGOUT_RESPONSE, 1 + 4);
    data << uint8(0);
    data << uint32(0);
    SendPacket(&data);
    LogoutRequest(time(NULL));
}

void WorldSession::HandlePlayerLogoutOpcode(WorldPacket& recv_data)
{
	if (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || GetPlayer()->IsTaxiFlying() || GetSecurity() > 0)
	{
		WorldPacket data(SMSG_LOGOUT_RESPONSE, 1 + 4);
		data << uint8(0);
		data << uint32(16777216);
		SendPacket(&data);
		LogoutPlayer(true);
	}
	else
		SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
}

void WorldSession::HandleLogoutCancelOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_LOGOUT_CANCEL Message");

    LogoutRequest(0);

    WorldPacket data(SMSG_LOGOUT_CANCEL_ACK, 0);
    SendPacket(&data);

    // not remove flags if can't free move - its not set in Logout request code.
    if (GetPlayer()->CanFreeMove())
    {
        //!we can move again
        GetPlayer()->SetRoot(false);

        //! Stand Up
        GetPlayer()->SetStandState(UNIT_STAND_STATE_STAND);

        //! DISABLE_ROTATE
        GetPlayer()->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    DEBUG_LOG("WORLD: sent SMSG_LOGOUT_CANCEL_ACK Message");
}

void WorldSession::HandleTogglePvP(WorldPacket& recv_data)
{
    // this opcode can be used in two ways: Either set explicit new status or toggle old status
    if (recv_data.size() == 1)
    {
        bool newPvPStatus;
        recv_data >> newPvPStatus;
        GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP, newPvPStatus);
    }
    else
    {
        GetPlayer()->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP);
    }

    if (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
    {
        if (!GetPlayer()->IsPvP() || GetPlayer()->pvpInfo.endTimer != 0)
            { GetPlayer()->UpdatePvP(true, true); }
    }
    else
    {
        if (!GetPlayer()->pvpInfo.inHostileArea && GetPlayer()->IsPvP())
            { GetPlayer()->pvpInfo.endTimer = time(NULL); }     // start toggle-off
    }
}

void WorldSession::HandleZoneUpdateOpcode(WorldPacket& recv_data)
{
    uint32 newZone;
    recv_data >> newZone;

    DETAIL_LOG("WORLD: Received opcode CMSG_ZONEUPDATE: newzone is %u", newZone);

    // use server side data
    uint32 newzone, newarea;
    GetPlayer()->GetZoneAndAreaId(newzone, newarea);
    GetPlayer()->UpdateZone(newzone, newarea);
}

void WorldSession::HandleSetTargetOpcode(WorldPacket& recv_data)
{
    // When this packet send?
    ObjectGuid guid ;
    recv_data >> guid;

    _player->SetTargetGuid(guid);

    // update reputation list if need
    Unit* unit = ObjectAccessor::GetUnit(*_player, guid);   // can select group members at diff maps
    if (!unit)
        { return; }

    if (FactionTemplateEntry const* factionTemplateEntry = sFactionTemplateStore.LookupEntry(unit->getFaction()))
        { _player->GetReputationMgr().SetVisible(factionTemplateEntry); }
}

void WorldSession::HandleSetSelectionOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;

    _player->SetSelectionGuid(guid);

    if (guid.IsEmpty())     // TODO this is probably a wrong place for such action, so it's a "hacky" or "wrong" fix
    {
        _player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, false);
        return;
    }

    // update reputation list if need
    Unit* unit = ObjectAccessor::GetUnit(*_player, guid);   // can select group members at diff maps
    if (!unit)
        { return; }

    if (FactionTemplateEntry const* factionTemplateEntry = sFactionTemplateStore.LookupEntry(unit->getFaction()))
        { _player->GetReputationMgr().SetVisible(factionTemplateEntry); }
}

void WorldSession::HandleStandStateChangeOpcode(WorldPacket& recv_data)
{
    // DEBUG_LOG("WORLD: Received opcode CMSG_STANDSTATECHANGE"); -- too many spam in log at lags/debug stop
    uint32 animstate;
    recv_data >> animstate;

    _player->SetStandState(animstate);
}

void WorldSession::HandleFriendListOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_FRIEND_LIST");
    _player->GetSocial()->SendFriendList();
}

void WorldSession::HandleAddFriendOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_ADD_FRIEND");

    std::string friendName = GetMangosString(LANG_FRIEND_IGNORE_UNKNOWN);

    recv_data >> friendName;

    if (!normalizePlayerName(friendName))
        { return; }

    CharacterDatabase.escape_string(friendName);            // prevent SQL injection - normal name don't must changed by this call

    DEBUG_LOG("WORLD: %s asked to add friend : '%s'",
              GetPlayer()->GetName(), friendName.c_str());

    CharacterDatabase.AsyncPQuery(&WorldSession::HandleAddFriendOpcodeCallBack, GetAccountId(), "SELECT guid, race FROM characters WHERE name = '%s'", friendName.c_str());
}

void WorldSession::HandleAddFriendOpcodeCallBack(QueryResult* result, uint32 accountId)
{
    if (!result)
        { return; }

    uint32 friendLowGuid = (*result)[0].GetUInt32();
    ObjectGuid friendGuid = ObjectGuid(HIGHGUID_PLAYER, friendLowGuid);
    Team team = Player::TeamForRace((*result)[1].GetUInt8());

    delete result;

    WorldSession* session = sWorld.FindSession(accountId);
    if (!session)
        { return; }

    Player* player = session->GetPlayer();
    if (!player)
        { return; }

    FriendsResult friendResult = FRIEND_NOT_FOUND;
    if (friendGuid)
    {
        if (friendGuid == player->GetObjectGuid())
            { friendResult = FRIEND_SELF; }
        else if (player->GetTeam() != team && !sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_ADD_FRIEND) && session->GetSecurity() < SEC_MODERATOR)
            { friendResult = FRIEND_ENEMY; }
        else if (player->GetSocial()->HasFriend(friendGuid))
            { friendResult = FRIEND_ALREADY; }
        else
        {
            Player* pFriend = ObjectAccessor::FindPlayer(friendGuid);
            if (pFriend && pFriend->IsInWorld() && pFriend->IsVisibleGloballyFor(player))
                { friendResult = FRIEND_ADDED_ONLINE; }
            else
                { friendResult = FRIEND_ADDED_OFFLINE; }

            if (!player->GetSocial()->AddToSocialList(friendGuid, false))
            {
                friendResult = FRIEND_LIST_FULL;
                DEBUG_LOG("WORLD: %s's friend list is full.", player->GetName());
            }
        }
    }

    sSocialMgr.SendFriendStatus(player, friendResult, friendGuid, false);

    DEBUG_LOG("WORLD: Sent (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleDelFriendOpcode(WorldPacket& recv_data)
{
    ObjectGuid friendGuid;

    DEBUG_LOG("WORLD: Received opcode CMSG_DEL_FRIEND");

    recv_data >> friendGuid;

    _player->GetSocial()->RemoveFromSocialList(friendGuid, false);

    sSocialMgr.SendFriendStatus(GetPlayer(), FRIEND_REMOVED, friendGuid, false);

    DEBUG_LOG("WORLD: Sent motd (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleAddIgnoreOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_ADD_IGNORE");

    std::string IgnoreName = GetMangosString(LANG_FRIEND_IGNORE_UNKNOWN);

    recv_data >> IgnoreName;

    if (!normalizePlayerName(IgnoreName))
        { return; }

    CharacterDatabase.escape_string(IgnoreName);            // prevent SQL injection - normal name don't must changed by this call

    DEBUG_LOG("WORLD: %s asked to Ignore: '%s'",
              GetPlayer()->GetName(), IgnoreName.c_str());

    CharacterDatabase.AsyncPQuery(&WorldSession::HandleAddIgnoreOpcodeCallBack, GetAccountId(), "SELECT guid FROM characters WHERE name = '%s'", IgnoreName.c_str());
}

void WorldSession::HandleAddIgnoreOpcodeCallBack(QueryResult* result, uint32 accountId)
{
    if (!result)
        { return; }

    uint32 ignoreLowGuid = (*result)[0].GetUInt32();
    ObjectGuid ignoreGuid = ObjectGuid(HIGHGUID_PLAYER, ignoreLowGuid);

    delete result;

    WorldSession* session = sWorld.FindSession(accountId);
    if (!session)
        { return; }

    Player* player = session->GetPlayer();
    if (!player)
        { return; }

    FriendsResult ignoreResult = FRIEND_IGNORE_NOT_FOUND;
    if (ignoreGuid)
    {
        if (ignoreGuid == player->GetObjectGuid())
            { ignoreResult = FRIEND_IGNORE_SELF; }
        else if (player->GetSocial()->HasIgnore(ignoreGuid))
            { ignoreResult = FRIEND_IGNORE_ALREADY; }
        else
        {
            ignoreResult = FRIEND_IGNORE_ADDED;

            // ignore list full
            if (!player->GetSocial()->AddToSocialList(ignoreGuid, true))
                { ignoreResult = FRIEND_IGNORE_FULL; }
        }
    }

    sSocialMgr.SendFriendStatus(player, ignoreResult, ignoreGuid, false);

    DEBUG_LOG("WORLD: Sent (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleDelIgnoreOpcode(WorldPacket& recv_data)
{
    ObjectGuid ignoreGuid;

    DEBUG_LOG("WORLD: Received opcode CMSG_DEL_IGNORE");

    recv_data >> ignoreGuid;

    _player->GetSocial()->RemoveFromSocialList(ignoreGuid, true);

    sSocialMgr.SendFriendStatus(GetPlayer(), FRIEND_IGNORE_REMOVED, ignoreGuid, false);

    DEBUG_LOG("WORLD: Sent motd (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleBugOpcode(WorldPacket& recv_data)
{
    uint32 suggestion, contentlen, typelen;
    std::string content, type;

    recv_data >> suggestion >> contentlen >> content;

    recv_data >> typelen >> type;

    if (suggestion == 0)
        { DEBUG_LOG("WORLD: Received opcode CMSG_BUG [Bug Report]"); }
    else
        { DEBUG_LOG("WORLD: Received opcode CMSG_BUG [Suggestion]"); }

    DEBUG_LOG("%s", type.c_str());
    DEBUG_LOG("%s", content.c_str());

    CharacterDatabase.escape_string(type);
    CharacterDatabase.escape_string(content);
    CharacterDatabase.PExecute("INSERT INTO bugreport (type,content) VALUES('%s', '%s')", type.c_str(), content.c_str());
}

void WorldSession::HandleReclaimCorpseOpcode(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_RECLAIM_CORPSE");

    ObjectGuid guid;
    recv_data >> guid;

    if (GetPlayer()->IsAlive())
        { return; }

    // body not released yet
    if (!GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        { return; }

    Corpse* corpse = GetPlayer()->GetCorpse();

    if (!corpse)
        { return; }

    // prevent resurrect before 30-sec delay after body release not finished
    if (corpse->GetGhostTime() + GetPlayer()->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) > time(NULL))
        { return; }

    if (!corpse->IsWithinDistInMap(GetPlayer(), CORPSE_RECLAIM_RADIUS, true))
        { return; }

    // resurrect
    GetPlayer()->ResurrectPlayer(GetPlayer()->InBattleGround() ? 1.0f : 0.5f);

    // spawn bones
    GetPlayer()->SpawnCorpseBones();
}

void WorldSession::HandleResurrectResponseOpcode(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_RESURRECT_RESPONSE");

    ObjectGuid guid;
    uint8 status;
    recv_data >> guid;
    recv_data >> status;

    if (GetPlayer()->IsAlive())
        { return; }

    if (status == 0)
    {
        GetPlayer()->clearResurrectRequestData();           // reject
        return;
    }

    if (!GetPlayer()->isRessurectRequestedBy(guid))
        { return; }

    GetPlayer()->ResurectUsingRequestData();                // will call spawncorpsebones
}

void WorldSession::HandleAreaTriggerOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_AREATRIGGER");

    uint32 Trigger_ID;

    recv_data >> Trigger_ID;
    DEBUG_LOG("Trigger ID: %u", Trigger_ID);
    Player* player = GetPlayer();

    if (player->IsTaxiFlying())
    {
        DEBUG_LOG("Player '%s' (GUID: %u) in flight, ignore Area Trigger ID: %u", player->GetName(), player->GetGUIDLow(), Trigger_ID);
        return;
    }

    AreaTriggerEntry const* atEntry = sAreaTriggerStore.LookupEntry(Trigger_ID);
    if (!atEntry)
    {
        DEBUG_LOG("Player '%s' (GUID: %u) send unknown (by DBC) Area Trigger ID: %u", player->GetName(), player->GetGUIDLow(), Trigger_ID);
        return;
    }

    // delta is safe radius
    const float delta = 5.0f;

    // check if player in the range of areatrigger
    if (!IsPointInAreaTriggerZone(atEntry, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), delta))
    {
        DEBUG_LOG("Player '%s' (GUID: %u) too far, ignore Area Trigger ID: %u", player->GetName(), player->GetGUIDLow(), Trigger_ID);
        return;
    }

    if (sScriptMgr.OnAreaTrigger(player, atEntry))
        { return; }

    uint32 quest_id = sObjectMgr.GetQuestForAreaTrigger(Trigger_ID);
    if (quest_id && player->IsAlive() && player->IsActiveQuest(quest_id))
    {
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest_id);
        if (pQuest)
        {
            if (player->GetQuestStatus(quest_id) == QUEST_STATUS_INCOMPLETE)
                { player->AreaExploredOrEventHappens(quest_id); }
        }
    }

    // enter to tavern, not overwrite city rest
    if (sObjectMgr.IsTavernAreaTrigger(Trigger_ID))
    {
        // set resting flag we are in the inn
        if (player->GetRestType() != REST_TYPE_IN_CITY)
            player->SetRestType(REST_TYPE_IN_TAVERN, Trigger_ID);
        return;
    }

    if (BattleGround* bg = player->GetBattleGround())
    {
        if (bg->HandleAreaTrigger(player, Trigger_ID))
        return;
    }
    else if (OutdoorPvP* outdoorPvP = sOutdoorPvPMgr.GetScript(player->GetCachedZoneId()))
    {
        if (outdoorPvP->HandleAreaTrigger(player, Trigger_ID))
            { return; }
    }

    // NULL if all values default (non teleport trigger)
    AreaTrigger const* at = sObjectMgr.GetAreaTrigger(Trigger_ID);
    if (!at)
        { return; }

    MapEntry const* targetMapEntry = sMapStore.LookupEntry(at->target_mapId);
    if (!targetMapEntry)
        { return; }

    // ghost resurrected at enter attempt to dungeon with corpse (including fail enter cases)
    if (!player->IsAlive() && targetMapEntry->IsDungeon())
    {
        uint32 corpseMapId = 0; // was planned to be negative as "incorrect" id? anyway map 0 is not instanceable
        if (Corpse* corpse = player->GetCorpse())
            { corpseMapId = corpse->GetMapId(); }

        // check back way from corpse to entrance
        uint32 instance_map = corpseMapId;
        do
        {
            // most often fast case
            if (instance_map == targetMapEntry->MapID)
                { break; }

            InstanceTemplate const* instance = ObjectMgr::GetInstanceTemplate(instance_map);
            instance_map = instance ? instance->parent : 0;
        }
        while (instance_map);

        // corpse not in dungeon or some linked deep dungeons
        if (!instance_map)
        {
            player->GetSession()->SendAreaTriggerMessage("You can not enter %s while in a ghost mode",
                    targetMapEntry->name[player->GetSession()->GetSessionDbcLocale()]);
            return;
        }

        // need find areatrigger to inner dungeon for landing point
        if (at->target_mapId != corpseMapId)
        {
            if (AreaTrigger const* corpseAt = sObjectMgr.GetMapEntranceTrigger(corpseMapId))
            {
                at = corpseAt;
                targetMapEntry = sMapStore.LookupEntry(at->target_mapId);
                if (!targetMapEntry)
                    { return; }
            }
        }

        // now we can resurrect player, and then check teleport requirements
        player->ResurrectPlayer(0.5f);
        player->SpawnCorpseBones();
    }

    uint32 miscRequirement = 0;
    AreaLockStatus lockStatus = player->GetAreaTriggerLockStatus(at, miscRequirement);
    if (lockStatus != AREA_LOCKSTATUS_OK)
    {
        player->SendTransferAbortedByLockStatus(targetMapEntry, lockStatus, miscRequirement);
        return;
    }

    // teleport player
    player->TeleportTo(at->target_mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation, TELE_TO_NOT_LEAVE_TRANSPORT, true);
}

void WorldSession::HandleUpdateAccountData(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_UPDATE_ACCOUNT_DATA");
    recv_data.rpos(recv_data.wpos());                       // prevent spam at unimplemented packet
    // recv_data.hexlike();
}

void WorldSession::HandleRequestAccountData(WorldPacket& /*recv_data*/)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_REQUEST_ACCOUNT_DATA");
    // recv_data.hexlike();
}

void WorldSession::HandleSetActionButtonOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_SET_ACTION_BUTTON");
    uint8 button;
    uint32 packetData;
    recv_data >> button >> packetData;

    uint32 action = ACTION_BUTTON_ACTION(packetData);
    uint8  type   = ACTION_BUTTON_TYPE(packetData);

    DETAIL_LOG("BUTTON: %u ACTION: %u TYPE: %u", button, action, type);
    if (!packetData)
    {
        DETAIL_LOG("MISC: Remove action from button %u", button);
        GetPlayer()->removeActionButton(button);
    }
    else
    {
        switch (type)
        {
            case ACTION_BUTTON_MACRO:
            case ACTION_BUTTON_CMACRO:
                DETAIL_LOG("MISC: Added Macro %u into button %u", action, button);
                break;
            case ACTION_BUTTON_SPELL:
                DETAIL_LOG("MISC: Added Spell %u into button %u", action, button);
                break;
            case ACTION_BUTTON_ITEM:
                DETAIL_LOG("MISC: Added Item %u into button %u", action, button);
                break;
            default:
                sLog.outError("MISC: Unknown action button type %u for action %u into button %u", type, action, button);
                return;
        }
        GetPlayer()->addActionButton(button, action, type);
    }
}

void WorldSession::HandleCompleteCinematic(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_COMPLETE_CINEMATIC");
}

void WorldSession::HandleNextCinematicCamera(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_NEXT_CINEMATIC_CAMERA");
}

void WorldSession::HandleFeatherFallAck(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_MOVE_FEATHER_FALL_ACK");

    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
}

void WorldSession::HandleMoveUnRootAck(WorldPacket& recv_data)
{
    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
    /*
        ObjectGuid guid;
        recv_data >> guid;

        // now can skip not our packet
        if(_player->GetGUID() != guid)
        {
            recv_data.rpos(recv_data.wpos());               // prevent warnings spam
            return;
        }

        DEBUG_LOG("WORLD: Received opcode CMSG_FORCE_MOVE_UNROOT_ACK");

        recv_data.read_skip<uint32>();                      // unk

        MovementInfo movementInfo;
        ReadMovementInfo(recv_data, &movementInfo);
    */
}

void WorldSession::HandleMoveRootAck(WorldPacket& recv_data)
{
    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
    /*
        ObjectGuid guid;
        recv_data >> guid;

        // now can skip not our packet
        if(_player->GetObjectGuid() != guid)
        {
            recv_data.rpos(recv_data.wpos());               // prevent warnings spam
            return;
        }

        DEBUG_LOG("WORLD: Received opcode CMSG_FORCE_MOVE_ROOT_ACK");

        recv_data.read_skip<uint32>();                      // unk

        MovementInfo movementInfo;
        ReadMovementInfo(recv_data, &movementInfo);
    */
}

void WorldSession::HandleSetActionBarTogglesOpcode(WorldPacket& recv_data)
{
    uint8 ActionBar;

    recv_data >> ActionBar;

    if (!GetPlayer())                                       // ignore until not logged (check needed because STATUS_AUTHED)
    {
        if (ActionBar != 0)
            { sLog.outError("WorldSession::HandleSetActionBarToggles in not logged state with value: %u, ignored", uint32(ActionBar)); }
        return;
    }

    GetPlayer()->SetByteValue(PLAYER_FIELD_BYTES, 2, ActionBar);
}

void WorldSession::HandlePlayedTime(WorldPacket& /*recv_data*/)
{
    WorldPacket data(SMSG_PLAYED_TIME, 4 + 4);
    data << uint32(_player->GetTotalPlayedTime());
    data << uint32(_player->GetLevelPlayedTime());
    SendPacket(&data);
}

void WorldSession::HandleInspectOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;
    DEBUG_LOG("Inspected guid is %s", guid.GetString().c_str());

    Player* plr = sObjectMgr.GetPlayer(guid);
    if (plr && _player->IsFriendlyTo(plr) && _player->IsWithinDistInMap(plr, TRADE_DISTANCE, false))  // why not 3D check?
    {
        _player->SetSelectionGuid(guid);

        WorldPacket data(SMSG_INSPECT, 8);
        data << ObjectGuid(guid);
        SendPacket(&data);
    }
    else
        { DEBUG_LOG("%s not found!", guid.GetString().c_str()); }

}

void WorldSession::HandleInspectHonorStatsOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;

    Player* pl = sObjectMgr.GetPlayer(guid);
    if (pl && _player->IsFriendlyTo(pl) && _player->IsWithinDistInMap(pl, TRADE_DISTANCE, false))
    {
        WorldPacket data(MSG_INSPECT_HONOR_STATS, (8 + 1 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 1));
        data << guid;                                       // player guid
        // Rank, filling bar, PLAYER_BYTES_3, ??
        data << (uint8)pl->GetByteValue(PLAYER_FIELD_BYTES2, 0);
        // Today Honorable and Dishonorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_SESSION_KILLS);
        // Yesterday Honorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_YESTERDAY_KILLS);
        // Last Week Honorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_KILLS);
        // This Week Honorable kills
        data << pl->GetUInt32Value(PLAYER_FIELD_THIS_WEEK_KILLS);
        // Lifetime Honorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);
        // Lifetime Dishonorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_LIFETIME_DISHONORABLE_KILLS);
        // Yesterday Honor
        data << pl->GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION);
        // Last Week Honor
        data << pl->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_CONTRIBUTION);
        // This Week Honor
        data << pl->GetUInt32Value(PLAYER_FIELD_THIS_WEEK_CONTRIBUTION);
        // Last Week Standing
        data << pl->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_RANK);
        data << (uint8)pl->GetHonorHighestRankInfo().visualRank;           // Highest Rank, ??
        SendPacket(&data);
    }
    else
        { DEBUG_LOG("%s not found!", guid.GetString().c_str()); }
}

void WorldSession::HandleFarSightOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_FAR_SIGHT");
    // recv_data.hexlike();

    uint8 op;
    recv_data >> op;

    WorldObject* obj = _player->GetMap()->GetWorldObject(_player->GetFarSightGuid());
    if (!obj)
        { return; }

    switch (op)
    {
        case 0:
            DEBUG_LOG("Removed FarSight from %s", _player->GetGuidStr().c_str());
            _player->GetCamera().ResetView(false);
            break;
        case 1:
            DEBUG_LOG("Added FarSight %s to %s", _player->GetFarSightGuid().GetString().c_str(), _player->GetGuidStr().c_str());
            _player->GetCamera().SetView(obj, false);
            break;
    }
}

void WorldSession::HandleResetInstancesOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_RESET_INSTANCES");

    if (Group* pGroup = _player->GetGroup())
    {
        if (pGroup->IsLeader(_player->GetObjectGuid()))
            { pGroup->ResetInstances(INSTANCE_RESET_ALL, _player); }
    }
    else
        { _player->ResetInstances(INSTANCE_RESET_ALL); }
}

void WorldSession::HandleCancelMountAuraOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode  CMSG_CANCEL_MOUNT_AURA");

    // If player is not mounted, so go out :)
    if (!_player->IsMounted())                              // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_CHAR_NON_MOUNTED);
        return;
    }

    if (_player->IsTaxiFlying())                            // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_YOU_IN_FLIGHT);
        return;
    }

    _player->Unmount(_player->HasAuraType(SPELL_AURA_MOUNTED));
    _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
}

void WorldSession::HandleRequestPetInfoOpcode(WorldPacket& /*recv_data */)
{
    /*
        DEBUG_LOG("WORLD: Received opcode CMSG_REQUEST_PET_INFO");
        recv_data.hexlike();
    */
}

void WorldSession::HandleSetTaxiBenchmarkOpcode(WorldPacket& recv_data)
{
    uint8 mode;
    recv_data >> mode;

    DEBUG_LOG("Client used \"/timetest %d\" command", mode);
}
