<div align="center">

# mod-guild-message-history

**Guild chat history for AzerothCore 3.3.5a — the guild conversation is still there when you log back in.**

[![Build Status](https://github.com/OWNER/mod-guild-message-history/actions/workflows/core-build.yml/badge.svg)](https://github.com/OWNER/mod-guild-message-history/actions/workflows/core-build.yml)
[![Codestyle](https://github.com/OWNER/mod-guild-message-history/actions/workflows/core_codestyle.yml/badge.svg)](https://github.com/OWNER/mod-guild-message-history/actions/workflows/core_codestyle.yml)
![License](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-AzerothCore%20%7C%20WotLK%203.3.5a-orange)

</div>

<!-- Maintainers: replace OWNER in the two workflow badge URLs with your GitHub user or organisation. -->

---

## What it does

Every guild chat message is archived in the `characters` database while the module is
running. When a character logs in, the messages it missed while offline are replayed as
chat lines coloured like guild chat, a few seconds after the client has finished loading,
one line at a time:

```
[Guild History] 3 archived guild message(s)
[Guild History] Thrall: LFM Ulduar 25, need 2 healers
[Guild History] Jaina: count me in
[Guild History] Thrall: nice, 23/25
```

No more "what did I miss?" after a disconnect, a night offline or a realm restart.

## Features

- **Archives guild chat** (`/g`) and, optionally, officer chat (`/o`).
- **Replays what the character missed** on login, oldest first, capped by count and age.
- **Guild-coloured output** — green sender names (`|cff40bf40`) and white text instead of
  the usual yellow system spam.
- **Configurable delay** after login plus a per-line delay, so the archive scrolls in
  naturally instead of dumping on top of the login messages.
- **Two delivery modes**: normal system chat lines, or a dedicated channel so the
  archive can live in its own chat window.
- **Self-maintaining**: old rows are purged in daily bounded batches, checkpoints of
  deleted characters are removed with the character.
- **No client modifications, no custom DBC, no core patches** — a plain AzerothCore module.

## Requirements

- [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) (`master`, WotLK 3.3.5a)
- `MODULES=static` (the default CMake setting)

## Installation

1. Drop the `mod-guild-message-history` folder into `azerothcore/modules/`.
2. Re-run CMake configuration and rebuild `worldserver`.
3. Start the worldserver. That is all:
   - `data/sql/db-characters/` is applied automatically by the built-in DB updater;
   - `conf/mod_guild_message_history.conf.dist` is copied next to your other configs and
     generated as `mod_guild_message_history.conf` on first start.

Check the startup log for the module in the `Modules configuration` block, and the
`sql.updates` section for the `2026_10_16_00_guild_message_history.sql` entry.

## Configuration

All options live in `mod_guild_message_history.conf` (created from the `.dist`):

| Option | Default | Description |
| --- | --- | --- |
| `GuildMessageHistory.Enable` | `1` | Master switch. When `0`, nothing is stored and nothing is replayed. |
| `GuildMessageHistory.IncludeOfficerChat` | `0` | Also archive officer chat. Off by default because officer chat can hold internal guild information. |
| `GuildMessageHistory.MaxMessages` | `100` | Maximum number of lines replayed on login (1–200). |
| `GuildMessageHistory.MaxAgeDays` | `30` | Messages older than this are never replayed (1–3650). |
| `GuildMessageHistory.RetentionDays` | `60` | Archived rows older than this are deleted by the daily maintenance pass (0–3650, `0` = keep forever). |
| `GuildMessageHistory.DelaySeconds` | `4` | Wait this long after login before the archive is shown (0–60). |
| `GuildMessageHistory.LineDelayMs` | `120` | Delay between two replayed lines in milliseconds (0–5000). |
| `GuildMessageHistory.ChatMode` | `system` | `system` = system chat lines, `channel` = channel chat lines (see below). |
| `GuildMessageHistory.ChannelName` | `GuildHistory` | Channel name used when `ChatMode = channel`. |

Changes can be applied without a restart: `.reload config`.

### A separate chat window for the archive

**Mode `system` (default).** The lines are system messages, so you can move them with the
built-in chat settings: right-click the chat tab → *Settings* → *Message Types*, then in a
new chat window enable only *System Messages*. Other system messages (loot errors, etc.)
will follow them, so most people prefer the channel mode below.

**Mode `channel`.** Set:

```ini
GuildMessageHistory.ChatMode = channel
GuildMessageHistory.ChannelName = GuildHistory
```

Each replayed line is then sent as a real channel chat line of `GuildHistory`. In game:

1. `/join GuildHistory` (once — the client remembers it and rejoins automatically).
2. Right-click the chat tab → *Settings* → *Channels*, enable `GuildHistory` only in the
   window where you want the archive.

The archive then lives in its own window, styled by the client like any other channel,
and can be coloured/positioned independently.

> Colour tuning: in `system` mode the sender name uses `|cff40bf40` (the client's guild
> green) and the text uses `|cffffffff`. Both are constants at the top of
> `src/mod_guild_message_history.cpp` (`GuildColor`, `TextColor`) if you want a different tint.

## Behaviour and limitations

- **Archived lines are chat output, not real guild packets.** The module never forges
  guild chat history as if it had been said while you were online; it is a clearly
  labelled replay.
- **Only messages sent while the module is installed can be replayed.** The first login
  of each character creates its checkpoint, so there is no history to show yet.
- **Addon traffic is ignored** — guild messages with `LANG_ADDON` are machine data.
- **Your own messages are not replayed**; you were online when you wrote them.
- **Checkpoints are written when a replay finishes, and on logout.** An interrupted replay
  (relog, crash) is retried on the next login instead of being lost, which can show a few
  duplicates — never losses.
- **The archive is per guild, not per character.** Each character has its own checkpoint,
  but the replay selects the messages of the character's *current* guild, so a character
  that just switched guilds can see up to `MaxMessages`/`MaxAgeDays` of that guild's
  older messages.
- **Server-side guild announcements are not archived** — only player chat that goes
  through `PlayerScript::OnPlayerCanUseChat`.
- **Reads at login are synchronous** (two indexed queries). Writes are asynchronous.
  The replay itself is delivered by a `WorldScript` timer, never from the login hook.
- **Keep an eye on `RetentionDays`** for your guild chat volume; the archive is purged
  in batches of 20 000 rows, four batches per day, so a very busy realm can outrun it.

## Uninstalling

Delete the folder, rebuild, then remove the tables:

```sql
DROP TABLE IF EXISTS `guild_message_history`;
DROP TABLE IF EXISTS `guild_message_history_state`;
```

## Development notes

- **Entry point naming.** AzerothCore generates the module script loader from the module
  folder name, replacing every `-` with `_`, and calls `Add<folder>Scripts()`. Here that is
  `Addmod_guild_message_historyScripts()` in `src/mod_guild_message_history_loader.cpp`.
  Renaming the folder means renaming that function.
- **No prepared statements.** Modules cannot register prepared statements because the
  statement ids live in a core-side enum (`CharacterDatabaseStatements`). Values are
  escaped with `DatabaseWorkerPool::EscapeString` before being embedded in the
  interpolated SQL — keep that pattern if you extend the queries.
- **Timestamps are Unix epoch seconds** (`GameTime::GetGameTime().count()`) stored as
  `BIGINT UNSIGNED`, so the MySQL session timezone can never skew the replay window.
- **Config is cached** in a `WorldScript` and refreshed on `OnAfterConfigLoad`.
- **The delayed replay** is a queue of `PendingReplay` entries drained by
  `WorldScript::OnUpdate`; a player that disappears for ~10 seconds drops its queue.

## Testing checklist

1. Two characters in the same guild: `A` sends a message, `B` logs out.
2. `A` keeps sending messages while `B` is offline.
3. `B` logs in: after `DelaySeconds` the archive appears, oldest first, capped by
   `MaxMessages`/`MaxAgeDays`.
4. `B` logs out and back in: nothing is replayed twice.
5. `IncludeOfficerChat = 1` archives `/o` as well; `0` does not.
6. `ChatMode = channel`: `/join GuildHistory`, relog, the archive appears in that channel
   and can be pinned to its own chat window.
7. `RetentionDays = 1` with an artificially old row (`UPDATE guild_message_history SET
   sent_at = 1000 WHERE id = 1;`) — the row disappears after the next maintenance run.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| Nothing is replayed | First login after install (checkpoint initialisation), or the character was not in a guild while messages were sent. |
| Nothing is stored at all | `GuildMessageHistory.Enable = 0`, or the module is not in the `Modules configuration` list of CMake. |
| Lines appear too early / too high in chat | Increase `GuildMessageHistory.DelaySeconds`. |
| Lines appear all at once | Increase `GuildMessageHistory.LineDelayMs`. |
| Channel mode shows nothing | The chat window does not have that channel enabled — `/join GuildHistory` and enable it in the chat settings. |

## License

Released under the **GNU AGPL v3**, like AzerothCore itself. See [LICENSE](LICENSE).
