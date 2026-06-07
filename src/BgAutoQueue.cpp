/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BgAutoQueue.h"

#include "AreaDefines.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Chat.h"
#include "Config.h"
#include "Containers.h"
#include "DBCStores.h"
#include "DisableMgr.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace
{
    // Used as reference (Warsong Gulch spans for every eligible level)
    constexpr uint32 BG_BRACKET_REFERENCE_MAP = MAP_WARSONG_GULCH; // 489

    // Normal (non-arena, non-random) battleground types
    constexpr BattlegroundTypeId BG_NORMAL_TYPES[] =
    {
        BATTLEGROUND_AV,
        BATTLEGROUND_WS,
        BATTLEGROUND_AB,
        BATTLEGROUND_EY,
        BATTLEGROUND_SA,
        BATTLEGROUND_IC
    };

    // default for BgAutoQueue.BroadcastMessage config
    constexpr char const* BG_AUTO_QUEUE_DEFAULT_BROADCAST =
        "A Battleground event is starting shortly. Type .bgevents off to opt "
        "out, or .bgevents on to opt back in. You can also join manually by "
        "pressing H.";
}

BgAutoQueue* BgAutoQueue::instance()
{
    static BgAutoQueue instance;
    return &instance;
}

void BgAutoQueue::LoadConfig()
{
    _enabled  = sConfigMgr->GetOption<bool>("BgAutoQueue.Enable", true);
    _levelMin = sConfigMgr->GetOption<uint32>("BgAutoQueue.Level.Min", 10);
    _levelMax = sConfigMgr->GetOption<uint32>("BgAutoQueue.Level.Max", 79);

    if (_levelMin > _levelMax)
    {
        LOG_WARN("module", "BgAutoQueue level range is inverted ({} > {}), swapping.", _levelMin, _levelMax);
        std::swap(_levelMin, _levelMax);
    }

    // Only parse the type ids here; battleground templates do not exist yet at
    // config-load time, so template/arena validation is left to ResolvePool.
    _poolRaw.clear();
    std::string const poolStr = sConfigMgr->GetOption<std::string>("BgAutoQueue.Pool", "2,3,7");
    for (std::string_view token : Acore::Tokenize(poolStr, ',', false))
    {
        Optional<uint32> value = Acore::StringTo<uint32>(token);
        if (!value)
        {
            LOG_WARN("module", "BgAutoQueue.Pool entry '{}' is not a valid number, ignoring.", token);
            continue;
        }

        BattlegroundTypeId bgTypeId = static_cast<BattlegroundTypeId>(*value);
        if (bgTypeId == BATTLEGROUND_RB)
        {
            LOG_WARN("module", "BgAutoQueue.Pool entry {} is Random Battleground (unsupported), ignoring.", *value);
            continue;
        }

        _poolRaw.push_back(bgTypeId);
    }

    if (_poolRaw.empty())
        LOG_WARN("module", "BgAutoQueue.Pool is empty; the random pick is disabled (only live-BG reinforcement can queue players).");

    uint32 const intervalMin = sConfigMgr->GetOption<uint32>("BgAutoQueue.Interval", 45);
    _intervalMs = intervalMin * 60u * 1000u;

    uint32 const initialDelaySec = sConfigMgr->GetOption<uint32>("BgAutoQueue.InitialDelay", 0);
    _initialDelayMs = initialDelaySec * 1000u;

    uint32 const warningLeadSec = sConfigMgr->GetOption<uint32>("BgAutoQueue.WarningLeadTime", 60);
    _warningLeadMs = warningLeadSec * 1000u;

    if (_intervalMs > 0 && _warningLeadMs >= _intervalMs)
        LOG_WARN("module", "BgAutoQueue.WarningLeadTime ({} s) >= Interval ({} min); the warning will not fire.", warningLeadSec, intervalMin);

    _crossFaction     = sConfigMgr->GetOption<bool>("BgAutoQueue.CrossFaction", true);
    _skipGameMasters  = sConfigMgr->GetOption<bool>("BgAutoQueue.SkipGameMasters", true);
    _skipAfk          = sConfigMgr->GetOption<bool>("BgAutoQueue.SkipAFK", true);
    _broadcastMessage = sConfigMgr->GetOption<std::string>("BgAutoQueue.BroadcastMessage", BG_AUTO_QUEUE_DEFAULT_BROADCAST);

    _skipAuras.clear();
    std::string const skipAurasStr = sConfigMgr->GetOption<std::string>("BgAutoQueue.SkipAuras", "");
    for (std::string_view token : Acore::Tokenize(skipAurasStr, ',', false))
    {
        Optional<uint32> value = Acore::StringTo<uint32>(Acore::String::Trim(std::string(token)));
        if (!value)
        {
            LOG_WARN("module", "BgAutoQueue.SkipAuras entry '{}' is not a valid number, ignoring.", token);
            continue;
        }

        _skipAuras.push_back(*value);
    }

    // Reset timing on (re)load. Reload re-applies InitialDelay — accepted.
    _elapsedMs   = 0;
    _warningSent = false;
    _firstPass   = true;

    LOG_INFO("module", "mod-bg-auto-queue: enabled={}, levels=[{}-{}], pool size={}, interval={} min, initialDelay={} s, warningLead={} s, crossFaction={}, skipGM={}, skipAFK={}, skipAuras={}.",
        _enabled, _levelMin, _levelMax, _poolRaw.size(), intervalMin, initialDelaySec, warningLeadSec, _crossFaction, _skipGameMasters, _skipAfk, _skipAuras.size());

    // Opt-out is stored via the core PlayerSettings system, which only persists
    // across logins when EnablePlayerSettings is on. Without it, .bgevents
    // opt-out still works but only for the current session.
    if (!sWorld->getBoolConfig(CONFIG_PLAYER_SETTINGS_ENABLED))
        LOG_WARN("module", "mod-bg-auto-queue: EnablePlayerSettings is 0; "
            ".bgevents opt-out works only for the current session and will not "
            "persist across logins until an administrator sets EnablePlayerSettings = 1.");
}

void BgAutoQueue::ResolvePool()
{
    _pool.clear();
    for (BattlegroundTypeId bgTypeId : _poolRaw)
    {
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
        {
            LOG_DEBUG("module", "BgAutoQueue.Pool entry {} has no battleground template, ignoring.", static_cast<uint32>(bgTypeId));
            continue;
        }

        if (bgTemplate->isArena())
        {
            LOG_DEBUG("module", "BgAutoQueue.Pool entry {} is an arena (unsupported), ignoring.", static_cast<uint32>(bgTypeId));
            continue;
        }

        _pool.push_back(bgTypeId);
    }

    if (_pool.empty() && !_poolRaw.empty())
        LOG_DEBUG("module", "BgAutoQueue.Pool has no usable battlegrounds against the current templates; the random pick is disabled.");
}

bool BgAutoQueue::IsOptedOut(Player* player) const
{
    // GetPlayerSetting is non-const (it lazily creates a zero-default entry),
    // but the constness here is on BgAutoQueue, not on the Player* argument.
    return player->GetPlayerSetting("mod-bg-auto-queue", BG_AUTO_QUEUE_SETTING_OPT_OUT).IsEnabled();
}

void BgAutoQueue::SetOptOut(Player* player, bool optedOut)
{
    player->UpdatePlayerSetting("mod-bg-auto-queue", BG_AUTO_QUEUE_SETTING_OPT_OUT, optedOut ? 1u : 0u);
}

bool BgAutoQueue::IsLevelEligible(uint8 level) const
{
    return level >= _levelMin && level <= _levelMax;
}

char const* BgAutoQueue::GetSkipReasonLabel(SkipReason reason)
{
    switch (reason)
    {
        case SkipReason::NotInWorld:          return "not in world";
        case SkipReason::OptedOut:            return "opted out (.bgevents off)";
        case SkipReason::Level:               return "level outside configured range";
        case SkipReason::Dungeon:             return "in a dungeon/raid";
        case SkipReason::InBattleground:      return "already in a battleground";
        case SkipReason::AlreadyQueued:       return "already queued / no free queue slot";
        case SkipReason::Deserter:            return "deserter or cannot join";
        case SkipReason::Lfg:                 return "using the LFG system";
        case SkipReason::DeathKnightEbonHold: return "Death Knight locked to Ebon Hold";
        case SkipReason::GameMaster:          return "game master";
        case SkipReason::Aura:                return "has a configured skip aura";
        case SkipReason::Afk:                 return "flagged AFK";
        case SkipReason::NoBracket:           return "no PvP bracket for level";
        default:                              return "unknown";
    }
}

bool BgAutoQueue::IsEligible(Player* player, SkipReason* reason) const
{
    auto fail = [reason](SkipReason value)
    {
        if (reason)
            *reason = value;
        return false;
    };

    if (!player || !player->IsInWorld())
        return fail(SkipReason::NotInWorld);

    std::string const& name = player->GetName();

    if (IsOptedOut(player))
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: opted out.", name);
        return fail(SkipReason::OptedOut);
    }

    if (!IsLevelEligible(player->GetLevel()))
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: level {} outside [{}-{}].", name, player->GetLevel(), _levelMin, _levelMax);
        return fail(SkipReason::Level);
    }

    if (player->GetMap()->IsDungeon())
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: in a dungeon/raid.", name);
        return fail(SkipReason::Dungeon);
    }

    if (player->InBattleground())
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: already in a battleground.", name);
        return fail(SkipReason::InBattleground);
    }

    if (player->InBattlegroundQueue() || !player->HasFreeBattlegroundQueueId())
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: already queued or no free queue slot.", name);
        return fail(SkipReason::AlreadyQueued);
    }

    // Deserter check via any standard template (WSG).
    if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_WS))
    {
        if (!player->CanJoinToBattleground(bgTemplate))
        {
            LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: CanJoinToBattleground=false (deserter?).", name);
            return fail(SkipReason::Deserter);
        }
    }

    // LFG: mirror the core BG join handler rule.
    lfg::LfgState lfgState = sLFGMgr->GetState(player->GetGUID());
    if (lfgState > lfg::LFG_STATE_NONE
        && (lfgState != lfg::LFG_STATE_QUEUED || !sWorld->getBoolConfig(CONFIG_ALLOW_JOIN_BG_AND_LFG)))
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: using the LFG system.", name);
        return fail(SkipReason::Lfg);
    }

    // Death Knights still locked to Ebon Hold cannot be teleported to a BG yet.
    if (player->IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_TELEPORT)
        && player->GetMapId() == MAP_EBON_HOLD
        && !player->IsGameMaster()
        && !player->HasSpell(50977))
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: Death Knight not yet allowed to leave Ebon Hold.", name);
        return fail(SkipReason::DeathKnightEbonHold);
    }

    if (_skipGameMasters && player->IsGameMaster())
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: game master.", name);
        return fail(SkipReason::GameMaster);
    }

    if (_skipAfk && player->isAFK())
    {
        LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: flagged AFK.", name);
        return fail(SkipReason::Afk);
    }

    for (uint32 auraId : _skipAuras)
    {
        if (player->HasAura(auraId))
        {
            LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: has skip aura {}.", name, auraId);
            return fail(SkipReason::Aura);
        }
    }

    return true;
}

bool BgAutoQueue::CanEnter(Player* player, BattlegroundTypeId bgTypeId) const
{
    if (!player)
        return false;

    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    if (!player->GetBGAccessByLevel(bgTypeId))
        return false;

    // A BG without a PvP bracket entry for the player's level cannot be queued.
    return GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel()) != nullptr;
}

bool BgAutoQueue::BucketHasAnyFit(BattlegroundTypeId bgTypeId, BracketBucket const& bucket) const
{
    for (ObjectGuid guid : bucket.players)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (player && CanEnter(player, bgTypeId))
            return true;
    }

    return false;
}

bool BgAutoQueue::BucketFitsLiveBg(Battleground* bg, BracketBucket const& bucket) const
{
    BattlegroundTypeId const bgTypeId = bg->GetBgTypeID();
    for (ObjectGuid guid : bucket.players)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->GetBGAccessByLevel(bgTypeId))
            continue;

        PvPDifficultyEntry const* entry = GetBattlegroundBracketByLevel(bg->GetMapId(), player->GetLevel());
        if (entry && entry->GetBracketId() == bg->GetBracketId())
            return true;
    }

    return false;
}

BgAutoQueue::QueuedWaiters BgAutoQueue::CountUninvitedWaiters(BattlegroundTypeId bgTypeId,
    BattlegroundBracketId bracketId) const
{
    QueuedWaiters waiters;

    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
    if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
        return waiters;

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

    static constexpr BattlegroundQueueGroupTypes WAITER_GROUP_TYPES[] =
    {
        BG_QUEUE_PREMADE_ALLIANCE,
        BG_QUEUE_PREMADE_HORDE,
        BG_QUEUE_NORMAL_ALLIANCE,
        BG_QUEUE_NORMAL_HORDE,
        BG_QUEUE_CFBG
    };

    for (BattlegroundQueueGroupTypes groupType : WAITER_GROUP_TYPES)
    {
        for (GroupQueueInfo const* gInfo : bgQueue.m_QueuedGroups[bracketId][groupType])
        {
            if (gInfo->IsInvitedToBGInstanceGUID != 0)
                continue;

            uint32 const count = static_cast<uint32>(gInfo->Players.size());
            waiters.total += count;
            if (gInfo->teamId == TEAM_ALLIANCE)
                waiters.alliance += count;
            else
                waiters.horde += count;
        }
    }

    return waiters;
}

bool BgAutoQueue::IsViable(Battleground* bgTemplate, BracketBucket const& bucket,
    BattlegroundBracketId bracketId) const
{
    uint32 const minPerTeam = bgTemplate->GetMinPlayersPerTeam();
    QueuedWaiters const waiters = CountUninvitedWaiters(bgTemplate->GetBgTypeID(), bracketId);

    if (_crossFaction)
        return (bucket.alliance + bucket.horde + waiters.total) >= (2u * minPerTeam);

    return (bucket.alliance + waiters.alliance) >= minPerTeam
        && (bucket.horde + waiters.horde) >= minPerTeam;
}

BattlegroundTypeId BgAutoQueue::SelectBattlegroundForBracket(BattlegroundBracketId bracketId, BracketBucket const& bucket) const
{
    // (a) Live-BG reinforcement (priority; not limited to the pool).
    std::vector<BattlegroundTypeId> liveTypes;
    for (BattlegroundTypeId bgTypeId : BG_NORMAL_TYPES)
    {
        if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId, nullptr))
            continue;

        for (Battleground* bg : sBattlegroundMgr->GetBGFreeSlotQueueStore(bgTypeId))
        {
            if (!(bg->GetStatus() > STATUS_WAIT_QUEUE && bg->GetStatus() < STATUS_WAIT_LEAVE))
                continue;

            if (!bg->HasFreeSlots())
                continue;

            // Match each live game by its own map-relative bracket, reinforced by
            // whichever subset of bucket players fits it (off-boundary players are
            // skipped at queue time, not blocking the whole game).
            if (!BucketFitsLiveBg(bg, bucket))
                continue;

            liveTypes.push_back(bgTypeId);
            break;
        }
    }

    if (!liveTypes.empty())
        return Acore::Containers::SelectRandomContainerElement(liveTypes);

    // (a2) Prefer a not-yet-running BG that already has uninvited queuers, so a
    // manual queuer's chosen BG is reinforced instead of bypassed. Among viable
    // candidates pick the one closest to popping (most waiters); ties at random.
    std::vector<BattlegroundTypeId> waiterLeaders;
    uint32 bestWaiters = 0;
    for (BattlegroundTypeId bgTypeId : BG_NORMAL_TYPES)
    {
        if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId, nullptr))
            continue;

        if (!BucketHasAnyFit(bgTypeId, bucket))
            continue;

        QueuedWaiters const waiters = CountUninvitedWaiters(bgTypeId, bracketId);
        if (waiters.total == 0)
            continue;

        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate || !IsViable(bgTemplate, bucket, bracketId))
            continue;

        if (waiters.total > bestWaiters)
        {
            bestWaiters = waiters.total;
            waiterLeaders.clear();
            waiterLeaders.push_back(bgTypeId);
        }
        else if (waiters.total == bestWaiters)
            waiterLeaders.push_back(bgTypeId);
    }

    if (!waiterLeaders.empty())
        return Acore::Containers::SelectRandomContainerElement(waiterLeaders);

    // (b) Random pick from the configured pool.
    std::vector<BattlegroundTypeId> candidates;
    for (BattlegroundTypeId bgTypeId : _pool)
    {
        if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId, nullptr))
            continue;

        if (BucketHasAnyFit(bgTypeId, bucket))
            candidates.push_back(bgTypeId);
    }

    if (candidates.empty())
        return BATTLEGROUND_TYPE_NONE;

    if (candidates.size() == 1)
        return candidates.front();

    std::vector<BattlegroundTypeId> viable;
    for (BattlegroundTypeId bgTypeId : candidates)
    {
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (bgTemplate && IsViable(bgTemplate, bucket, bracketId))
            viable.push_back(bgTypeId);
    }

    if (!viable.empty())
        return Acore::Containers::SelectRandomContainerElement(viable);

    // None viable: pick the smallest by MinPlayersPerTeam (ties -> lowest id).
    BattlegroundTypeId best = BATTLEGROUND_TYPE_NONE;
    uint32 bestMin = std::numeric_limits<uint32>::max();
    for (BattlegroundTypeId bgTypeId : candidates)
    {
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
            continue;

        uint32 const minPerTeam = bgTemplate->GetMinPlayersPerTeam();
        if (minPerTeam < bestMin || (minPerTeam == bestMin && bgTypeId < best))
        {
            bestMin = minPerTeam;
            best = bgTypeId;
        }
    }

    return best;
}

uint32 BgAutoQueue::QueueBucket(BattlegroundTypeId bgTypeId, BracketBucket const& bucket, uint32* skippedAtQueueTime)
{
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return 0;

    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
    if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
        return 0;

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

    uint32 queued = 0;
    BattlegroundBracketId scheduledBracket = BG_BRACKET_ID_FIRST;

    for (ObjectGuid guid : bucket.players)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player)
            continue;

        // Re-validate: state may have changed since the bucket was gathered.
        if (!IsEligible(player) || player->InBattlegroundQueueForBattlegroundQueueType(bgQueueTypeId))
        {
            if (skippedAtQueueTime)
                ++*skippedAtQueueTime;
            continue;
        }

        // BG-specific veto (can only run at queue time).
        GroupJoinBattlegroundResult err = ERR_BATTLEGROUND_NONE;
        if (!sScriptMgr->OnPlayerCanJoinInBattlegroundQueue(player, ObjectGuid::Empty, bgTypeId, 0, err))
        {
            LOG_DEBUG("module", "mod-bg-auto-queue: skip {}: OnPlayerCanJoinInBattlegroundQueue veto.", player->GetName());
            if (skippedAtQueueTime)
                ++*skippedAtQueueTime;
            continue;
        }

        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
        if (!bracketEntry)
        {
            if (skippedAtQueueTime)
                ++*skippedAtQueueTime;
            continue;
        }

        GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, bgTypeId, bracketEntry, 0, false, false, 0, 0);
        uint32 avgWaitTime = bgQueue.GetAverageQueueWaitTime(ginfo);
        uint32 queueSlot = player->AddBattlegroundQueueId(bgQueueTypeId);

        if (WorldSession* session = player->GetSession())
        {
            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgTemplate, queueSlot, STATUS_WAIT_QUEUE, avgWaitTime, 0, 0, TEAM_NEUTRAL);
            session->SendPacket(&data);
        }

        sScriptMgr->OnPlayerJoinBG(player);

        scheduledBracket = bracketEntry->GetBracketId();
        ++queued;

        LOG_DEBUG("module", "mod-bg-auto-queue: queued {} into bgTypeId {}.", player->GetName(), static_cast<uint32>(bgTypeId));
    }

    // Schedule a single queue update for the bracket, not once per player.
    if (queued > 0)
        sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, scheduledBracket);

    return queued;
}

BgAutoQueue::QueuePassResult BgAutoQueue::RunQueuePass()
{
    // Rebuild the pool against the templates that exist right now, so every
    // pass (periodic or .bgevents run) reflects the current battlegrounds.
    ResolvePool();

    std::unordered_map<BattlegroundBracketId, BracketBucket> buckets;

    QueuePassResult result;

    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        ++result.considered;

        SkipReason reason = SkipReason::NotInWorld;
        if (!IsEligible(player, &reason))
        {
            ++result.skipped[static_cast<size_t>(reason)];
            continue;
        }

        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(BG_BRACKET_REFERENCE_MAP, player->GetLevel());
        if (!bracketEntry)
        {
            ++result.skipped[static_cast<size_t>(SkipReason::NoBracket)];
            continue;
        }

        BracketBucket& bucket = buckets[bracketEntry->GetBracketId()];
        bucket.minLevel = bracketEntry->minLevel;
        bucket.maxLevel = bracketEntry->maxLevel;
        bucket.players.push_back(player->GetGUID());
        if (player->GetTeamId() == TEAM_ALLIANCE)
            ++bucket.alliance;
        else
            ++bucket.horde;
    }

    for (auto const& [bracketId, bucket] : buckets)
    {
        BattlegroundTypeId bgTypeId = SelectBattlegroundForBracket(bracketId, bucket);
        if (bgTypeId == BATTLEGROUND_TYPE_NONE)
        {
            LOG_DEBUG("module", "mod-bg-auto-queue: bracket {} has no eligible battleground, skipping.", static_cast<uint32>(bracketId));
            ++result.bracketsWithoutBg;
            continue;
        }

        uint32 const queued = QueueBucket(bgTypeId, bucket, &result.skippedAtQueueTime);
        if (queued > 0)
        {
            result.players += queued;
            result.brackets.push_back({ bucket.minLevel, bucket.maxLevel, queued });
        }
    }

    std::sort(result.brackets.begin(), result.brackets.end(),
        [](QueuePassResult::BracketCount const& a, QueuePassResult::BracketCount const& b)
        {
            return a.minLevel < b.minLevel;
        });

    LOG_INFO("module", "mod-bg-auto-queue: queue pass queued {} player(s) across {} bracket(s).", result.players, result.brackets.size());
    for (QueuePassResult::BracketCount const& bracket : result.brackets)
        LOG_INFO("module", "mod-bg-auto-queue:   bracket {}-{}: {} player(s).", bracket.minLevel, bracket.maxLevel, bracket.players);

    return result;
}

void BgAutoQueue::Update(uint32 diff)
{
    if (!_enabled || _intervalMs == 0)
        return;

    _elapsedMs += diff;

    uint32 const target = (_firstPass && _initialDelayMs > 0) ? _initialDelayMs : _intervalMs;

    if (!_warningSent && target > _warningLeadMs && (target - _elapsedMs) <= _warningLeadMs)
    {
        BroadcastWarning();
        _warningSent = true;
    }

    if (_elapsedMs >= target)
    {
        RunQueuePass();
        _elapsedMs = 0;
        _warningSent = false;
        _firstPass = false;
    }
}

uint32 BgAutoQueue::GetTimeUntilNextPass() const
{
    if (!_enabled || _intervalMs == 0)
        return 0;

    uint32 const target = (_firstPass && _initialDelayMs > 0) ? _initialDelayMs : _intervalMs;
    return target > _elapsedMs ? (target - _elapsedMs) : 0;
}

void BgAutoQueue::BroadcastWarning() const
{
    if (_broadcastMessage.empty())
        return;

    uint32 sent = 0;
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (!IsEligible(player))
            continue;

        WorldSession* session = player->GetSession();
        if (!session)
            continue;

        ChatHandler(session).SendSysMessage(_broadcastMessage);
        ++sent;
    }

    LOG_DEBUG("module", "mod-bg-auto-queue: broadcast warning sent to {} player(s).", sent);
}
