/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#ifndef _MOD_BG_AUTO_QUEUE_H_
#define _MOD_BG_AUTO_QUEUE_H_

#include "DBCEnums.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

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

    // Outcome of a queue pass, reported back to .bgevents run so an operator
    // can see whether anyone was actually queued, broken down per bracket.
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
    // Queue veto (that runs only at queue time).
    bool IsEligible(Player* player) const;

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
    // queue update for the bracket. Returns the number of players queued.
    uint32 QueueBucket(BattlegroundTypeId bgTypeId, BracketBucket const& bucket);

    void BroadcastWarning() const;

    bool _enabled = true;
    uint32 _levelMin = 10;
    uint32 _levelMax = 79;
    std::vector<BattlegroundTypeId> _pool;
    uint32 _intervalMs = 45u * 60u * 1000u;
    uint32 _initialDelayMs = 0;
    uint32 _warningLeadMs = 60u * 1000u;
    bool _crossFaction = true;
    bool _skipGameMasters = true;
    std::string _broadcastMessage;

    uint32 _elapsedMs = 0;
    bool _warningSent = false;
    bool _firstPass = true;
};

#define sBgAutoQueue BgAutoQueue::instance()

#endif // _MOD_BG_AUTO_QUEUE_H_
