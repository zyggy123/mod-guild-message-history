/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Guild.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"

#include "mod_guild_message_history.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GuildMessageHistory
{
    struct ModuleConfig
    {
        bool Enabled = true;
        bool IncludeOfficerChat = false;
        bool UseChannel = false;
        uint32 MaxMessages = 100;
        uint32 MaxAgeDays = 30;
        uint32 RetentionDays = 60;
        uint32 LoginDelayMs = 4000;
        uint32 LineDelayMs = 120;
        std::string ChannelName = "GuildHistory";
    };

    ModuleConfig const& GetConfig();
    void RefreshConfig();

    namespace
    {
        // Hard limits that protect the world thread and the client from a bad config value.
        constexpr uint32 MaxReplayMessagesLimit = 200;
        constexpr uint32 MaxAgeDaysLimit = 3650;
        constexpr uint32 RetentionDaysLimit = 3650;
        constexpr uint32 MaxLoginDelaySeconds = 60;
        constexpr uint32 MaxLineDelayMs = 5000;

        // Old rows are deleted in bounded batches so the daily purge never locks
        // the characters database for long.
        constexpr uint32 PurgeBatchSize = 20000;
        constexpr uint32 PurgeBatchesPerRun = 4;

        constexpr uint32 MaintenanceInterval = DAY * IN_MILLISECONDS;

        // 0.25 / 0.75 / 0.25 is the green the client uses for guild chat,
        // so archived lines look like guild chat instead of yellow system spam.
        constexpr char const* GuildColor = "|cff40bf40";
        constexpr char const* TextColor = "|cffffffff";

        // A player that is not resolvable for this many ticks (~10 seconds)
        // is considered gone and its queued replay is dropped.
        constexpr uint32 MissingPlayerTickLimit = 200;

        ModuleConfig _config;

        struct HistoryLine
        {
            ObjectGuid SenderGuid;
            std::string SenderName;
            std::string Message;
        };

        struct PendingReplay
        {
            std::vector<HistoryLine> Lines;
            uint32 DelayMs = 0;
            uint32 MissingTicks = 0;
            std::size_t NextLine = 0;
            bool HeaderSent = false;
        };

        std::map<ObjectGuid, PendingReplay> _pendingReplays;

        uint64 GetEpochNow()
        {
            return static_cast<uint64>(GameTime::GetGameTime().count());
        }

        // Escapes a value so it can be safely embedded between quotes in an
        // interpolated query. Modules cannot register prepared statements (the
        // statement ids live in a core-side enum), so escaping stays necessary.
        std::string Escape(std::string const& value)
        {
            std::string escaped = value;
            CharacterDatabase.EscapeString(escaped);
            return escaped;
        }

        void StoreCheckpoint(uint32 playerGuid)
        {
            uint64 const now = GetEpochNow();
            CharacterDatabase.Execute(
                "INSERT INTO guild_message_history_state (guid, last_logout) VALUES ({}, {}) "
                "ON DUPLICATE KEY UPDATE last_logout = {}",
                playerGuid, now, now);
        }

        void SendSystemLine(Player* player, std::string const& line)
        {
            ChatHandler handler(player->GetSession());
            handler.SendSysMessage(line);
        }

        // Builds a real channel chat packet, exactly like Channel::Say does for
        // offline members: the client renders it with the channel colors and lets
        // the player filter it into its own chat window.
        void SendChannelLine(Player* player, HistoryLine const& line)
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, LANG_UNIVERSAL, line.SenderGuid,
                player->GetGUID(), line.Message, 0, line.SenderName, "", 0, false, GetConfig().ChannelName);

            player->GetSession()->SendPacket(&data);
        }

        void SendHistoryLine(Player* player, HistoryLine const& line)
        {
            if (GetConfig().UseChannel)
            {
                SendChannelLine(player, line);
                return;
            }

            SendSystemLine(player, Acore::StringFormat("{}[Guild History]|r {}{}|r: {}{}|r",
                GuildColor, GuildColor, line.SenderName, TextColor, line.Message));
        }

        void SendHistoryHeader(Player* player, std::size_t count)
        {
            if (GetConfig().UseChannel)
                return;

            SendSystemLine(player, Acore::StringFormat("{}[Guild History]|r {} archived guild message(s)",
                GuildColor, count));
        }

        void CancelPendingReplay(ObjectGuid const& playerGuid)
        {
            _pendingReplays.erase(playerGuid);
        }

        // Sends the queued lines one by one, spaced by LineDelayMs, so the archive
        // does not arrive while the client is still loading into the world.
        void ProcessPendingReplays(uint32 diff)
        {
            if (_pendingReplays.empty())
                return;

            for (auto it = _pendingReplays.begin(); it != _pendingReplays.end();)
            {
                Player* player = ObjectAccessor::FindConnectedPlayer(it->first);
                if (!player || !player->GetSession())
                {
                    if (++it->second.MissingTicks > MissingPlayerTickLimit)
                        it = _pendingReplays.erase(it);
                    else
                        ++it;

                    continue;
                }

                it->second.MissingTicks = 0;

                if (it->second.DelayMs > diff)
                {
                    it->second.DelayMs -= diff;
                    ++it;
                    continue;
                }

                PendingReplay& replay = it->second;

                if (!replay.HeaderSent)
                {
                    SendHistoryHeader(player, replay.Lines.size());
                    replay.HeaderSent = true;
                    replay.DelayMs = GetConfig().LineDelayMs;
                    ++it;
                    continue;
                }

                if (replay.NextLine < replay.Lines.size())
                {
                    SendHistoryLine(player, replay.Lines[replay.NextLine++]);
                    replay.DelayMs = GetConfig().LineDelayMs;
                    ++it;
                    continue;
                }

                // Everything was delivered: only now does the checkpoint move, so an
                // interrupted replay is retried on the next login instead of lost.
                StoreCheckpoint(it->first.GetCounter());
                it = _pendingReplays.erase(it);
            }
        }

        class GuildMessageHistoryPlayerScript final : public PlayerScript
        {
        public:
            GuildMessageHistoryPlayerScript() : PlayerScript("GuildMessageHistoryPlayerScript") { }

            void OnPlayerLogin(Player* player) override
            {
                if (!player || !player->GetGuildId())
                    return;

                ModuleConfig const& config = GetConfig();
                if (!config.Enabled)
                    return;

                uint32 const guildId = player->GetGuildId();
                ObjectGuid const playerGuid = player->GetGUID();
                uint64 const now = GetEpochNow();

                QueryResult state = CharacterDatabase.Query(
                    "SELECT last_logout FROM guild_message_history_state WHERE guid = {}", playerGuid.GetCounter());

                if (!state)
                {
                    // First login after installation: messages sent before the module
                    // was installed cannot be reconstructed, so the checkpoint starts now.
                    StoreCheckpoint(playerGuid.GetCounter());
                    return;
                }

                uint64 lastCheckpoint = state->Fetch()[0].Get<uint64>();

                uint64 const maxAge = static_cast<uint64>(config.MaxAgeDays) * DAY;
                uint64 const oldestAllowed = now > maxAge ? now - maxAge : 0;
                lastCheckpoint = std::max(lastCheckpoint, oldestAllowed);

                // Newest first, so a long offline period never hides the most recent
                // chat behind the message cap; the rows are reversed before queuing.
                QueryResult result = CharacterDatabase.Query(
                    "SELECT sender_guid, sender_name, message FROM guild_message_history "
                    "WHERE guild_id = {} AND sender_guid <> {} AND sent_at > {} "
                    "ORDER BY sent_at DESC, id DESC LIMIT {}",
                    guildId, playerGuid.GetCounter(), lastCheckpoint, config.MaxMessages);

                std::vector<HistoryLine> history;
                if (result)
                {
                    history.reserve(config.MaxMessages);
                    do
                    {
                        Field* fields = result->Fetch();
                        uint32 const senderGuid = fields[0].Get<uint32>();
                        std::string sender = fields[1].Get<std::string>();
                        std::string message = fields[2].Get<std::string>();

                        if (sender.empty() || message.empty())
                            continue;

                        history.push_back({ ObjectGuid::Create<HighGuid::Player>(senderGuid),
                            std::move(sender), std::move(message) });
                    } while (result->NextRow());
                }

                if (history.empty())
                {
                    StoreCheckpoint(playerGuid.GetCounter());
                    return;
                }

                // Displayed oldest -> newest.
                std::reverse(history.begin(), history.end());

                PendingReplay replay;
                replay.Lines = std::move(history);
                replay.DelayMs = config.LoginDelayMs;
                _pendingReplays[playerGuid] = std::move(replay);

                LOG_INFO("module", "GuildMessageHistory: queued {} archived message(s) for {} (guild {}).",
                    _pendingReplays[playerGuid].Lines.size(), player->GetName(), guildId);
            }

            void OnPlayerLogout(Player* player) override
            {
                if (!player)
                    return;

                // A replay that was still queued must not advance the checkpoint,
                // otherwise the not-yet-shown messages would be lost.
                if (_pendingReplays.erase(player->GetGUID()))
                    return;

                if (!GetConfig().Enabled)
                    return;

                // Stored for guildless characters too: it marks "offline since"
                // and keeps the replay window correct if the player joins a guild.
                StoreCheckpoint(player->GetGUID().GetCounter());
            }

            // The checkpoint of a deleted character would otherwise stay in the
            // database forever and could be inherited by a reused guid.
            void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
            {
                trans->Append("DELETE FROM guild_message_history_state WHERE guid = {}", guid);
            }

            bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg,
                                    Guild* guild) override
            {
                if (!player || !guild || msg.empty())
                    return true;

                // Addon payloads share the guild chat type but are machine data.
                if (language == LANG_ADDON)
                    return true;

                ModuleConfig const& config = GetConfig();
                if (!config.Enabled)
                    return true;

                bool const isGuildChat = type == CHAT_MSG_GUILD;
                bool const isWantedOfficerChat = type == CHAT_MSG_OFFICER && config.IncludeOfficerChat;
                if (!isGuildChat && !isWantedOfficerChat)
                    return true;

                // Escape copies only: the core still needs the original text to
                // deliver the live guild message.
                CharacterDatabase.Execute(
                    "INSERT INTO guild_message_history (guild_id, sender_guid, sender_name, message, sent_at) "
                    "VALUES ({}, {}, '{}', '{}', {})",
                    guild->GetId(),
                    player->GetGUID().GetCounter(),
                    Escape(player->GetName()),
                    Escape(msg),
                    GetEpochNow());

                return true; // Never block the normal guild chat delivery.
            }
        };

        class GuildMessageHistoryWorldScript final : public WorldScript
        {
        public:
            GuildMessageHistoryWorldScript() : WorldScript("GuildMessageHistoryWorldScript") { }

            void OnStartup() override
            {
                RefreshConfig();
                RunMaintenance();
            }

            void OnAfterConfigLoad(bool /*reload*/) override
            {
                RefreshConfig();
            }

            void OnUpdate(uint32 diff) override
            {
                ProcessPendingReplays(diff);

                _maintenanceTimer += diff;
                if (_maintenanceTimer < MaintenanceInterval)
                    return;

                _maintenanceTimer = 0;
                RunMaintenance();
            }

        private:
            static void RunMaintenance()
            {
                ModuleConfig const& config = GetConfig();
                if (!config.RetentionDays)
                    return;

                uint64 const now = GetEpochNow();
                uint64 const retention = static_cast<uint64>(config.RetentionDays) * DAY;
                if (now <= retention)
                    return;

                uint64 const cutoff = now - retention;
                for (uint32 i = 0; i < PurgeBatchesPerRun; ++i)
                    CharacterDatabase.Execute(
                        "DELETE FROM guild_message_history WHERE sent_at < {} LIMIT {}", cutoff, PurgeBatchSize);

                LOG_INFO("module", "GuildMessageHistory: purged archived messages older than {} day(s).",
                    config.RetentionDays);
            }

            uint32 _maintenanceTimer = 0;
        };
    }

    ModuleConfig const& GetConfig()
    {
        return _config;
    }

    void RefreshConfig()
    {
        _config.Enabled = sConfigMgr->GetOption<bool>("GuildMessageHistory.Enable", true);
        _config.IncludeOfficerChat = sConfigMgr->GetOption<bool>("GuildMessageHistory.IncludeOfficerChat", false);
        _config.MaxMessages = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>("GuildMessageHistory.MaxMessages", 100), 1, MaxReplayMessagesLimit);
        _config.MaxAgeDays = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>("GuildMessageHistory.MaxAgeDays", 30), 1, MaxAgeDaysLimit);
        _config.RetentionDays = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>("GuildMessageHistory.RetentionDays", 60), 0, RetentionDaysLimit);
        _config.LoginDelayMs = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>("GuildMessageHistory.DelaySeconds", 4), 0, MaxLoginDelaySeconds)
            * IN_MILLISECONDS;
        _config.LineDelayMs = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>("GuildMessageHistory.LineDelayMs", 120), 0, MaxLineDelayMs);

        std::string mode = sConfigMgr->GetOption<std::string>("GuildMessageHistory.ChatMode", "system");
        std::transform(mode.begin(), mode.end(), mode.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        _config.UseChannel = mode == "channel";

        std::string const channelName = sConfigMgr->GetOption<std::string>(
            "GuildMessageHistory.ChannelName", "GuildHistory");
        if (!channelName.empty())
            _config.ChannelName = channelName;
    }
}

void AddGuildMessageHistoryScripts()
{
    GuildMessageHistory::RefreshConfig();

    new GuildMessageHistory::GuildMessageHistoryPlayerScript();
    new GuildMessageHistory::GuildMessageHistoryWorldScript();
}
