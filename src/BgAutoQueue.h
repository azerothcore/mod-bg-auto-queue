#ifndef _MOD_BG_AUTO_QUEUE_H_
#define _MOD_BG_AUTO_QUEUE_H_

#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <string>
#include <unordered_set>

class Player;

enum BgAutoQueueChoice : uint8
{
    BG_AUTO_QUEUE_RANDOM = 0,
    BG_AUTO_QUEUE_WSG    = 1,
    BG_AUTO_QUEUE_AB     = 2,
    BG_AUTO_QUEUE_EY     = 3,
    BG_AUTO_QUEUE_AV     = 4,
    BG_AUTO_QUEUE_SA     = 5,
    BG_AUTO_QUEUE_IC     = 6,
    BG_AUTO_QUEUE_MAX
};

class AC_GAME_API BgAutoQueue
{
public:
    static BgAutoQueue* instance();

    void LoadConfig();
    void LoadOptOutData();

    bool IsEnabled() const { return _enabled; }
    uint32 GetMinLevel() const { return _minLevel; }
    uint32 GetMaxLevel() const { return _maxLevel; }
    BgAutoQueueChoice GetDefaultChoice() const { return _defaultChoice; }
    uint32 GetIntervalMs() const { return _intervalMs; }

    bool IsOptedOut(ObjectGuid guid) const;
    void SetOptOut(ObjectGuid guid, bool optedOut);

    bool IsLevelEligible(uint8 level) const;

    // Pick the queue target for a player given the configured default,
    // falling back to Warsong Gulch when the chosen battleground is not
    // available for the player's level.
    BattlegroundTypeId ResolveBattlegroundFor(Player* player) const;

    // Auto-queue the player into the resolved battleground. Safe to call
    // for players that are already queued or otherwise ineligible — those
    // cases are silently skipped.
    void AutoQueuePlayer(Player* player) const;

    // Drives the periodic queue pass. Call from WorldScript::OnUpdate.
    void Update(uint32 diff);

private:
    BgAutoQueue() = default;

    void BroadcastWarning() const;

    bool _enabled = true;
    uint32 _minLevel = 10;
    uint32 _maxLevel = 80;
    BgAutoQueueChoice _defaultChoice = BG_AUTO_QUEUE_WSG;
    uint32 _intervalMs = 45 * 60 * 1000;
    uint32 _elapsedMs = 0;
    std::string _broadcastMessage;
    bool _warningSent = false;

    std::unordered_set<uint32> _optedOut; // characters guid::low values
};

#define sBgAutoQueue BgAutoQueue::instance()

#endif // _MOD_BG_AUTO_QUEUE_H_
