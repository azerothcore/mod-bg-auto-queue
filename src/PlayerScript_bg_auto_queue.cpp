/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BgAutoQueue.h"

#include "ScriptMgr.h"

class mod_bg_auto_queue_playerscript : public PlayerScript
{
public:
    mod_bg_auto_queue_playerscript() : PlayerScript("mod_bg_auto_queue_playerscript", {
        PLAYERHOOK_ON_DELETE_FROM_DB
    }) { }

    // guid is the character GUID-low; drop any opt-out row so the table does
    // not accumulate orphans when a character is deleted. The DELETE joins the
    // character-deletion transaction so it commits atomically with it.
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        sBgAutoQueue->DeleteOptOut(trans, guid);
    }
};

void AddSC_bg_auto_queue_playerscript()
{
    new mod_bg_auto_queue_playerscript();
}
