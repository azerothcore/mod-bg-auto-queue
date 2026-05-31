/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BgAutoQueue.h"

#include "Chat.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Util.h"

using namespace Acore::ChatCommands;

class bg_auto_queue_commandscript : public CommandScript
{
public:
    bg_auto_queue_commandscript() : CommandScript("bg_auto_queue_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable bgAutoQueueTable =
        {
            { "run", HandleBgAutoQueueRunCommand, SEC_GAMEMASTER, Console::Yes },
            { "",    HandleBgAutoQueueCommand,    SEC_PLAYER,     Console::No  },
        };

        static ChatCommandTable commandTable =
        {
            { "bgevents", bgAutoQueueTable },
        };

        return commandTable;
    }

    // Default subcommand: `.bgevents on`/`off` toggles opt state; `.bgevents`
    // with no argument prints the current state and time to the next event.
    static bool HandleBgAutoQueueCommand(ChatHandler* handler, Optional<bool> enable)
    {
        Player* player = handler->GetPlayer();
        if (!player)
        {
            handler->SendErrorMessage("This command must be used in-game.");
            return false;
        }

        if (enable.has_value())
        {
            // enable == true means opt IN, i.e. not opted out.
            sBgAutoQueue->SetOptOut(player, !*enable);

            if (*enable)
                handler->SendSysMessage("Battleground events enabled for your character.");
            else
                handler->SendSysMessage("Battleground events disabled for your character. You will not be auto-queued. Use .bgevents on to opt back in.");

            return true;
        }

        if (sBgAutoQueue->IsOptedOut(player))
            handler->SendSysMessage("Battleground events are currently DISABLED for your character.");
        else
            handler->SendSysMessage("Battleground events are currently ENABLED for your character.");

        uint32 msToNext = sBgAutoQueue->GetTimeUntilNextPass();
        if (sBgAutoQueue->IsEnabled() && msToNext > 0)
            handler->PSendSysMessage("Next scheduled event in {}.", secsToTimeString(msToNext / 1000));
        else
            handler->SendSysMessage("There are no scheduled events (an administrator may still trigger one).");

        return true;
    }

    static bool HandleBgAutoQueueRunCommand(ChatHandler* handler)
    {
        // Fires an immediate queue pass regardless of Enable/Interval without touching the periodic timer
        BgAutoQueue::QueuePassResult result = sBgAutoQueue->RunQueuePass();

        if (result.players == 0)
        {
            handler->SendSysMessage("Battleground event: no eligible players were queued.");
            return true;
        }

        handler->SendSysMessage("Battleground event queued players:");
        for (BgAutoQueue::QueuePassResult::BracketCount const& bracket : result.brackets)
        {
            if (bracket.minLevel == bracket.maxLevel)
                handler->PSendSysMessage("  {}: {} player(s)", bracket.minLevel, bracket.players);
            else
                handler->PSendSysMessage("  {}-{}: {} player(s)", bracket.minLevel, bracket.maxLevel, bracket.players);
        }

        handler->PSendSysMessage("Queued {} player(s) in total", result.players);
        return true;
    }
};

void AddSC_bg_auto_queue_commandscript()
{
    new bg_auto_queue_commandscript();
}
