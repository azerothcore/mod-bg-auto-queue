/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BgAutoQueue.h"

#include "Chat.h"
#include "Player.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;

class bg_auto_queue_commandscript : public CommandScript
{
public:
    bg_auto_queue_commandscript() : CommandScript("bg_auto_queue_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable bgAutoQueueTable =
        {
            { "on",     HandleBgAutoQueueOnCommand,     SEC_PLAYER, Console::No },
            { "off",    HandleBgAutoQueueOffCommand,    SEC_PLAYER, Console::No },
            { "status", HandleBgAutoQueueStatusCommand, SEC_PLAYER, Console::No },
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
        handler->SendSysMessage("Battleground auto-join enabled. You will be queued automatically on next login.");
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
        handler->SendSysMessage("Battleground auto-join disabled. You will not be queued automatically on login.");
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

        bool optedOut = sBgAutoQueue->IsOptedOut(player->GetGUID());
        if (optedOut)
            handler->SendSysMessage("Battleground auto-join is currently DISABLED for your character.");
        else
            handler->SendSysMessage("Battleground auto-join is currently ENABLED for your character.");

        if (!sBgAutoQueue->IsEnabled())
            handler->SendSysMessage("Note: the auto-join feature is globally disabled by the server configuration.");

        return true;
    }
};

void AddSC_bg_auto_queue_commandscript()
{
    new bg_auto_queue_commandscript();
}
