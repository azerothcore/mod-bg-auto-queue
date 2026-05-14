#include "BgAutoQueue.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    constexpr uint32 BG_AUTO_QUEUE_WARNING_LEAD_MS = 2u * 60u * 1000u;  // 2 minutes
    constexpr char const* BG_AUTO_QUEUE_DEFAULT_BROADCAST =
        "BG Event starting in 2 minutes, you can opt-out by typing \".bgevents off\". "
        "You can always join manually by pressing H and going to the BG tab";
}

BgAutoQueue* BgAutoQueue::instance()
{
    static BgAutoQueue instance;
    return &instance;
}

void BgAutoQueue::LoadConfig()
{
    _enabled  = sConfigMgr->GetOption<bool>("BgAutoQueue.Enable", true);
    _minLevel = sConfigMgr->GetOption<uint32>("BgAutoQueue.Level.Min", 10);
    _maxLevel = sConfigMgr->GetOption<uint32>("BgAutoQueue.Level.Max", 80);

    uint32 choice = sConfigMgr->GetOption<uint32>("BgAutoQueue.Battleground", BG_AUTO_QUEUE_WSG);
    if (choice >= BG_AUTO_QUEUE_MAX)
    {
        LOG_WARN("module", "BgAutoQueue.Battleground has invalid value {}, defaulting to Warsong Gulch.", choice);
        choice = BG_AUTO_QUEUE_WSG;
    }
    _defaultChoice = static_cast<BgAutoQueueChoice>(choice);

    if (_minLevel > _maxLevel)
    {
        LOG_WARN("module", "BgAutoQueue level range is inverted ({} > {}), swapping.", _minLevel, _maxLevel);
        std::swap(_minLevel, _maxLevel);
    }

    uint32 minutes = sConfigMgr->GetOption<uint32>("BgAutoQueue.Interval", 45);
    _intervalMs = minutes * 60u * 1000u;
    _elapsedMs = 0;
    _warningSent = false;

    _broadcastMessage = sConfigMgr->GetOption<std::string>("BgAutoQueue.BroadcastMessage",
        BG_AUTO_QUEUE_DEFAULT_BROADCAST);

    LOG_INFO("module", "mod-bg-auto-queue: enabled={}, levels=[{}-{}], choice={}, interval={} min ({} ms)",
        _enabled, _minLevel, _maxLevel, static_cast<uint32>(_defaultChoice), minutes, _intervalMs);
}

void BgAutoQueue::LoadOptOutData()
{
    _optedOut.clear();

    QueryResult result = CharacterDatabase.Query("SELECT guid FROM mod_bg_auto_queue_optout");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        _optedOut.insert(fields[0].Get<uint32>());
    } while (result->NextRow());

    LOG_INFO("module", "mod-bg-auto-queue: loaded {} opt-out entries.", _optedOut.size());
}

bool BgAutoQueue::IsOptedOut(ObjectGuid guid) const
{
    return _optedOut.find(guid.GetCounter()) != _optedOut.end();
}

void BgAutoQueue::SetOptOut(ObjectGuid guid, bool optedOut)
{
    uint32 low = guid.GetCounter();

    if (optedOut)
    {
        if (_optedOut.insert(low).second)
            CharacterDatabase.Execute("INSERT IGNORE INTO mod_bg_auto_queue_optout (guid) VALUES ({})", low);
    }
    else
    {
        if (_optedOut.erase(low) > 0)
            CharacterDatabase.Execute("DELETE FROM mod_bg_auto_queue_optout WHERE guid = {}", low);
    }
}

bool BgAutoQueue::IsLevelEligible(uint8 level) const
{
    return level >= _minLevel && level <= _maxLevel;
}

BattlegroundTypeId BgAutoQueue::ResolveBattlegroundFor(Player* player) const
{
    if (!player)
        return BATTLEGROUND_TYPE_NONE;

    auto canEnter = [player](BattlegroundTypeId bgTypeId) -> bool
    {
        Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgt)
            return false;

        if (!player->GetBGAccessByLevel(bgTypeId))
            return false;

        // The PvPDifficulty.dbc brackets are independent of the
        // battleground_template MinLvl/MaxLvl columns. A BG without a
        // bracket entry for the player's level cannot be queued — the
        // queue handler will reject it with a null bracket. Treat such
        // cases as ineligible so the WSG fallback can take over.
        return GetBattlegroundBracketByLevel(bgt->GetMapId(), player->GetLevel()) != nullptr;
    };

    BattlegroundTypeId desired = BATTLEGROUND_WS;
    switch (_defaultChoice)
    {
        case BG_AUTO_QUEUE_RANDOM: desired = BATTLEGROUND_RB; break;
        case BG_AUTO_QUEUE_WSG:    desired = BATTLEGROUND_WS; break;
        case BG_AUTO_QUEUE_AB:     desired = BATTLEGROUND_AB; break;
        case BG_AUTO_QUEUE_EY:     desired = BATTLEGROUND_EY; break;
        case BG_AUTO_QUEUE_AV:     desired = BATTLEGROUND_AV; break;
        case BG_AUTO_QUEUE_SA:     desired = BATTLEGROUND_SA; break;
        case BG_AUTO_QUEUE_IC:     desired = BATTLEGROUND_IC; break;
        case BG_AUTO_QUEUE_MAX:    break;
    }

    if (canEnter(desired))
        return desired;

    // Fallback: Warsong Gulch is the lowest-level standard battleground.
    if (desired != BATTLEGROUND_WS && canEnter(BATTLEGROUND_WS))
        return BATTLEGROUND_WS;

    return BATTLEGROUND_TYPE_NONE;
}

void BgAutoQueue::AutoQueuePlayer(Player* player) const
{
    if (!_enabled || !player)
        return;

    std::string const& name = player->GetName();

    if (IsOptedOut(player->GetGUID()))
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: opted out.", name);
        return;
    }

    if (!IsLevelEligible(player->GetLevel()))
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: level {} outside [{}-{}].",
            name, player->GetLevel(), _minLevel, _maxLevel);
        return;
    }

    if (player->InBattleground())
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: already in a battleground.", name);
        return;
    }

    if (player->InBattlegroundQueue())
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: already in a battleground/arena queue.", name);
        return;
    }

    if (!player->HasFreeBattlegroundQueueId())
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: no free battleground queue slot.", name);
        return;
    }

    BattlegroundTypeId bgTypeId = ResolveBattlegroundFor(player);
    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: no battleground available for level {} (choice={}).",
            name, player->GetLevel(), static_cast<uint32>(_defaultChoice));
        return;
    }

    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: no template for bgTypeId {}.",
            name, static_cast<uint32>(bgTypeId));
        return;
    }

    PvPDifficultyEntry const* bracketEntry =
        GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
    if (!bracketEntry)
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: no PvP bracket for map {} level {}.",
            name, bgTemplate->GetMapId(), player->GetLevel());
        return;
    }

    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
    if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: no queue type id for bgTypeId {}.",
            name, static_cast<uint32>(bgTypeId));
        return;
    }

    if (player->InBattlegroundQueueForBattlegroundQueueType(bgQueueTypeId))
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: already queued for this BG type.", name);
        return;
    }

    if (!player->CanJoinToBattleground(bgTemplate))
    {
        LOG_INFO("module", "mod-bg-auto-queue: skip {}: CanJoinToBattleground=false (deserter?).", name);
        return;
    }

    LOG_INFO("module", "mod-bg-auto-queue: queuing {} into bgTypeId {}.",
        name, static_cast<uint32>(bgTypeId));

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
    GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, bgTypeId, bracketEntry, 0, false, false, 0, 0);

    uint32 avgWaitTime = bgQueue.GetAverageQueueWaitTime(ginfo);
    uint32 queueSlot = player->AddBattlegroundQueueId(bgQueueTypeId);

    if (WorldSession* session = player->GetSession())
    {
        WorldPacket data;
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgTemplate, queueSlot,
            STATUS_WAIT_QUEUE, avgWaitTime, 0, 0, TEAM_NEUTRAL);
        session->SendPacket(&data);
    }

    sScriptMgr->OnPlayerJoinBG(player);

    sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
}

void BgAutoQueue::Update(uint32 diff)
{
    if (!_enabled || _intervalMs == 0)
        return;

    _elapsedMs += diff;

    // Heads-up broadcast 2 minutes before the queue pass.
    if (!_warningSent
        && _intervalMs > BG_AUTO_QUEUE_WARNING_LEAD_MS
        && _intervalMs - _elapsedMs <= BG_AUTO_QUEUE_WARNING_LEAD_MS)
    {
        BroadcastWarning();
        _warningSent = true;
    }

    if (_elapsedMs < _intervalMs)
        return;

    _elapsedMs = 0;
    _warningSent = false;

    auto const& players = ObjectAccessor::GetPlayers();
    LOG_INFO("module", "mod-bg-auto-queue: interval elapsed, scanning {} player(s).", players.size());

    uint32 queued = 0;
    for (auto const& [guid, player] : players)
    {
        if (!player || !player->IsInWorld())
            continue;

        bool wasInQueue = player->InBattlegroundQueue();
        AutoQueuePlayer(player);
        if (!wasInQueue && player->InBattlegroundQueue())
            ++queued;
    }

    LOG_INFO("module", "mod-bg-auto-queue: queued {} player(s) this pass.", queued);
}

void BgAutoQueue::BroadcastWarning() const
{
    if (_broadcastMessage.empty())
        return;

    uint32 sent = 0;
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (!player || !player->IsInWorld())
            continue;

        if (IsOptedOut(player->GetGUID()))
            continue;

        if (!IsLevelEligible(player->GetLevel()))
            continue;

        ChatHandler(player->GetSession()).SendSysMessage(_broadcastMessage);
        ++sent;
    }

    LOG_INFO("module", "mod-bg-auto-queue: broadcast warning sent to {} player(s).", sent);
}
