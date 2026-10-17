/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "mod_guild_message_history.h"

// AzerothCore generates the module loader from the module folder name: every '-'
// is replaced with '_' and the entry point must be Add<folder>Scripts().
// Folder: mod-guild-message-history -> Addmod_guild_message_historyScripts()
void Addmod_guild_message_historyScripts()
{
    AddGuildMessageHistoryScripts();
}
