/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BgAutoQueue.h"

#include "ScriptMgr.h"

class mod_bg_auto_queue_world : public WorldScript
{
public:
    mod_bg_auto_queue_world() : WorldScript("mod_bg_auto_queue_world", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sBgAutoQueue->LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        sBgAutoQueue->Update(diff);
    }
};

void AddSC_mod_bg_auto_queue()
{
    new mod_bg_auto_queue_world();
}
