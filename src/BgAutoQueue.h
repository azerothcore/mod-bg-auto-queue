/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _MOD_BG_AUTO_QUEUE_H_
#define _MOD_BG_AUTO_QUEUE_H_

#include "DBCEnums.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <array>
#include <string>
#include <vector>

class Battleground;
class Player;

// PlayerSettings layout for this module. source = "mod-bg-auto-queue".
enum BgAutoQueueSetting
{
    BG_AUTO_QUEUE_SETTING_OPT_OUT = 0 // value: 0 = opted in (default), 1 = opted out
};

class BgAutoQueue
{
public:
    static BgAutoQueue* instance();

    void LoadConfig();

    bool IsEnabled() const { return _enabled; }

    // Opt-out is stored as a per-character core PlayerSetting; these are thin
    // wrappers over Player::GetPlayerSetting/UpdatePlayerSetting.
    bool IsOptedOut(Player* player) const;
    void SetOptOut(Player* player, bool optedOut);

    bool IsLevelEligible(uint8 level) const;

    // Why an online player was not queued during a pass. Reported by
    // .bgevents run so an operator can tell exactly which filter fired
    // (e.g. an opted-out character looks identical to a wrong-level one
    // from the outside).
    enum class SkipReason : uint8
    {
        NotInWorld = 0,
        OptedOut,
        Level,
        Dungeon,
        InBattleground,
        AlreadyQueued,
        Deserter,
        Lfg,
        DeathKnightEbonHold,
        GameMaster,
        Aura,               // carries one of the configured skip auras
        Afk,                // flagged AFK
        NoBracket,          // no PvP bracket for the level on the reference map
        Count
    };

    static char const* GetSkipReasonLabel(SkipReason reason);

    // Outcome of a queue pass, reported back to .bgevents run so an operator
    // can see whether anyone was actually queued, broken down per bracket,
    // plus why the rest were skipped.
    struct QueuePassResult
    {
        struct BracketCount
        {
            uint32 minLevel = 0;
            uint32 maxLevel = 0;
            uint32 players = 0;
        };

        uint32 players = 0;                 // total players queued across all brackets
        std::vector<BracketCount> brackets; // one entry per bracket that queued >= 1 player, sorted by level

        uint32 considered = 0;              // online players examined this pass
        std::array<uint32, static_cast<size_t>(SkipReason::Count)> skipped{}; // per-reason skip tally
        uint32 bracketsWithoutBg = 0;       // populated brackets with no eligible BG to queue into
        uint32 skippedAtQueueTime = 0;      // eligible players dropped by re-check/veto at queue time
    };

    // Runs a single per-bracket queue pass. Does NOT check _enabled — it is
    // invoked both by the periodic Update (enabled path) and by .bgevents run
    // (always), so the Enable/Interval gate lives only in Update.
    QueuePassResult RunQueuePass();

    // Drives the periodic queue pass. Call from WorldScript::OnUpdate.
    void Update(uint32 diff);

    // Milliseconds until the next automatic pass (0 when no pass is scheduled).
    uint32 GetTimeUntilNextPass() const;

private:
    BgAutoQueue() = default;

    // Per-bracket bucket of eligible players gathered during a pass. Players
    // are stored by GUID and re-resolved at queue time (never store a
    // long-lived Player*).
    struct BracketBucket
    {
        std::vector<ObjectGuid> players;
        uint32 alliance = 0;
        uint32 horde = 0;
        uint32 minLevel = 0; // bracket level range, used for logging purposes
        uint32 maxLevel = 0;
    };

    // Shared per-player eligibility used by both the queue pass and the
    // warning broadcast. Excludes the BG-specific OnPlayerCanJoinInBattleground
    // Queue veto (that runs only at queue time). When non-null, reason is set
    // to the first failing filter (only meaningful when this returns false).
    bool IsEligible(Player* player, SkipReason* reason = nullptr) const;

    // True when the player can be queued into bgTypeId at their level.
    bool CanEnter(Player* player, BattlegroundTypeId bgTypeId) const;

    // True when every player in the bucket passes CanEnter for bgTypeId.
    bool IsBracketEligible(BattlegroundTypeId bgTypeId, BracketBucket const& bucket) const;

    // Viability per CrossFaction: cross-faction => total >= 2*min; otherwise
    // each faction tally >= min.
    bool IsViable(Battleground* bgTemplate, BracketBucket const& bucket) const;

    // Selects the BG for a populated bracket: live-BG reinforcement first,
    // then a random pick from the configured pool with documented fallbacks.
    BattlegroundTypeId SelectBattlegroundForBracket(BattlegroundBracketId bracketId,
        BracketBucket const& bucket) const;

    // Queues every player in the bucket into bgTypeId, then schedules a single
    // queue update for the bracket. Returns the number of players queued. When
    // non-null, skippedAtQueueTime is incremented for each bucket player
    // dropped by the queue-time re-check or the BG-specific veto.
    uint32 QueueBucket(BattlegroundTypeId bgTypeId, BracketBucket const& bucket,
        uint32* skippedAtQueueTime = nullptr);

    void BroadcastWarning() const;

    // Rebuilds _pool from the configured _poolRaw against the battleground
    // templates that exist right now. Called at the start of every pass so a
    // pass always reflects the current templates (templates are not yet loaded
    // at config-load time, and may change on .reload). Cheap: _poolRaw is tiny.
    void ResolvePool();

    bool _enabled = true;
    uint32 _levelMin = 10;
    uint32 _levelMax = 79;
    std::vector<BattlegroundTypeId> _poolRaw; // type ids parsed from config, unvalidated
    std::vector<BattlegroundTypeId> _pool;    // _poolRaw filtered to usable templates
    uint32 _intervalMs = 45u * 60u * 1000u;
    uint32 _initialDelayMs = 0;
    uint32 _warningLeadMs = 60u * 1000u;
    bool _crossFaction = true;
    bool _skipGameMasters = true;
    bool _skipAfk = true;
    std::vector<uint32> _skipAuras; // aura ids that exclude a player from a pass
    std::string _broadcastMessage;

    uint32 _elapsedMs = 0;
    bool _warningSent = false;
    bool _firstPass = true;
};

#define sBgAutoQueue BgAutoQueue::instance()

#endif // _MOD_BG_AUTO_QUEUE_H_
