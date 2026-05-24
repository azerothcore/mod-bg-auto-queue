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
            { "on",     HandleBgAutoQueueOnCommand,     SEC_PLAYER,        Console::No  },
            { "off",    HandleBgAutoQueueOffCommand,    SEC_PLAYER,        Console::No  },
            { "status", HandleBgAutoQueueStatusCommand, SEC_PLAYER,        Console::No  },
            { "run",    HandleBgAutoQueueRunCommand,    SEC_GAMEMASTER,    Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "bgevents", bgAutoQueueTable },
        };

        return commandTable;
    }

    static bool HandleBgAutoQueueOnCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
        {
            handler->SendErrorMessage("This command must be used in-game.");
            return false;
        }

        sBgAutoQueue->SetOptOut(player->GetGUID(), false);
        handler->SendSysMessage("Battleground events enabled for your character.");
        return true;
    }

    static bool HandleBgAutoQueueOffCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
        {
            handler->SendErrorMessage("This command must be used in-game.");
            return false;
        }

        sBgAutoQueue->SetOptOut(player->GetGUID(), true);
        handler->SendSysMessage("Battleground events disabled for your character. You will not be auto-queued. Use .bgevents on to opt back in.");
        return true;
    }

    static bool HandleBgAutoQueueStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
        {
            handler->SendErrorMessage("This command must be used in-game.");
            return false;
        }

        if (sBgAutoQueue->IsOptedOut(player->GetGUID()))
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
        sBgAutoQueue->RunQueuePass();
        handler->SendSysMessage("Battleground event queue pass executed.");
        return true;
    }
};

void AddSC_bg_auto_queue_commandscript()
{
    new bg_auto_queue_commandscript();
}
