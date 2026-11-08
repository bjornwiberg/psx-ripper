# MCP: an AI agent controls the app

With **MCP** on, an AI agent such as Claude Code can read what PSX RiPPER shows and control it
over your home network: start a rip, follow it, read the log, change a setting, check and tidy
the game list. MCP is the [Model Context Protocol](https://modelcontextprotocol.io/); the app is an
MCP server at `http://<the PS3's address>:8090/mcp`.

- [Turning it on](#turning-it-on)
- [Connecting an agent](#connecting-an-agent)
- [How the tools work](#how-the-tools-work)
- [Reading tools](#reading-tools)
- [Screens and buttons](#screens-and-buttons)
- [Drive actions](#drive-actions)
- [Game list tools](#game-list-tools)
- [Screen names](#screen-names)
- [Rules](#rules)
- [Security](#security)
- [Limits](#limits)

## Turning it on

START opens the settings. On **Remote control**, **✕** opens a menu with three switches; ← → (or ✕)
turn the selected one on or off:

| Row | What it does |
|---|---|
| Remote view | The screen and a pad in a web browser: `http://<ps3>:8090/` |
| MCP | An agent can read and control the app: `http://<ps3>:8090/mcp` |
| status.json | The drives' state as JSON for other programs: `http://<ps3>:8090/status.json` |

The line at the bottom shows the console's address for the selected row. MCP is off until you turn
it on. While it is on, **[MCP]** is shown in yellow at the left of the title on every screen, so
you can always see that an agent may be at work. Every setting change, rip, repair, check and
game-list change an agent makes is in the [audit log](user-guide.md#audit-log) with the source
*MCP*; settings, renames and moves to and from the trash can be undone there, as when you make
them. Emptying the trash cannot be undone, and a rip is logged as a job, not undone. `cancel`,
`skip_rereads` and `eject` are only in the app's log (`psxrip.log`), as they are from the pad.

## Connecting an agent

In Claude Code:

```sh
claude mcp add --transport http psx-ripper http://192.168.1.50:8090/mcp
```

Use your console's address (the Remote control menu shows it). Other MCP clients work the same
way: the transport is *Streamable HTTP*, one JSON request per `POST`, no login.

If MCP is off on the console, the agent gets the answer *MCP is off on the console. Turn it on in
PSX RiPPER: Settings → Remote control → MCP.*

Under the hood every call is one JSON-RPC 2.0 request, so `curl` works too:

```sh
curl -s -X POST http://192.168.1.50:8090/mcp -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
  "params":{"name":"get_drives","arguments":{}}}'
```

The examples below show the `arguments` and the text the tool answers with. They come from the
simulator (`make -C tests/host remote`) and are shortened where `…` stands.

## How the tools work

- **Reading tools** (`get_*`, `list_settings`, `read_log`, `list_games`, `get_game`) never touch the
  screen.
- **Actions** (`rip`, `check`, … and the game tools) do the work the way the pad would, with the
  same checks, and answer with the screen state afterwards, or with the reason they were refused
  (in English, the app's own text). `set_setting` answers with the setting's new value.
- **`show_games`** shows the game list on the screen with a source and filter (until the app
  restarts), and answers with what it shows and the screen state.
- **`go_screen`, `go_tab` and `press_button`** move around the app as the pad does. The console's
  screen moves with them: whoever is in front of the TV sees what the agent does.

A refusal comes back as a tool error (`isError: true`) with the text; a wrong argument (an unknown
drive name, a missing argument) as a JSON-RPC error that names the argument.

## Reading tools

### get_state

What the screen shows: the screen's name, the selected row, the tab on the home screen, an open
dialog, whether the on-screen keyboard is up, the buttons in the hint bar, `busy` (a drive is in
use) and every drive as `get_drives` gives it. No arguments.

```json
{"screen":"home","row":null,"tab":"internal","dialog":null,"keyboard":false,
 "buttons":[{"button":"cross","label":"Rip"},{"button":"triangle","label":"Eject"},
            {"button":"select","label":"Device settings"}],
 "busy":false,"drives":[{"drive":"internal","id":"internal","model":"PS3 Blu-ray drive","state":"ready",…}]}
```

With a dialog open, `screen` is `dialog` and `dialog` says what it asks and over which screen:

```json
{"screen":"dialog","row":0,"tab":"internal",
 "dialog":{"text":"Reset all settings to their defaults?\n\nThe webhook address and the local Redump copy are kept.",
           "question":true,"over":"settings"},
 "keyboard":false,"buttons":[{"button":"cross","label":"Yes"},{"button":"circle","label":"No"}],…}
```

### get_drives

Every drive with its disc and job, as [`/status.json`](configuration.md#statusjson) has them. `id`
is the name the other tools take: `internal`, or `usbN` with the USB port. No arguments.

```json
{"busy":false,"drives":[
 {"drive":"internal","id":"internal","model":"PS3 Blu-ray drive","state":"ready",
  "title":"Final Fantasy IX (Disc 1)","serial":"SLES_029.65","size_mb":706,
  "redump_name":"Final Fantasy IX (Europe) (Disc 1)","redump":"unchecked","already_ripped":false,
  "known_errors":0,"destination":"/dev_hdd0/PSXISO","file":"Final Fantasy IX (Disc 1) [SLES_029.65]"},
 {"drive":"usb","id":"usb0","port":0,"model":"Apple SuperDrive","state":"ready","title":"Croc 2",…}]}
```

### get_screenshot

A JPEG of the app's screen at half size, as the remote view shows it, given as an MCP image
(`image/jpeg`). System dialogs and the on-screen keyboard are drawn by the console on top of the
app and are not in it. No arguments.

### read_log

Lines of the app's log, `/dev_hdd0/tmp/psxrip.log`.

| Argument | |
|---|---|
| `from_line` | The first line, 0 = the log's first. Left out: the last lines |
| `max_lines` | At most 500, default 100 |

`{"max_lines":6}`:

```json
{"lines":["mcp: setting speed=2","mcp: drive usb0: eject=0","mcp: setting webhook set",
          "game list all: 3 disc(s), 3 row(s)","eject pressed on tab 1 (USB) by mcp",
          "eject: card 1 -> usb drive 0 (port 0)"],
 "from":0,"next":6,"total":6}
```

`next` is the `from_line` for the next call. Where the current webhook address, its path or a
header value appears in a line, the line shows `[secret]` instead. The webhook's host is not secret
and is shown as is (the app logs *webhook address set (host)*). Only the current values are known:
a line written with an earlier address is not changed (the app never logs the whole address). A line is cut after 1023 bytes, and when
the lines do not fit in one answer it ends early with `"truncated":true`.

### list_settings

Every setting with its `key`, its label in English and Swedish, its `value`, the allowed `values`
(each with its labels) and its `scope`: `standard`, or a drive (`internal`, `usb0`, …) for that
drive's own settings. Free-text settings (`name_pattern`, the webhook address and headers) have
`"values":null`. A secret setting has `"secret":true` and `is_set` instead of its value. No
arguments.

```json
{"settings":[
 {"key":"speed","scope":"standard","label":{"en":"Read speed","sv":"Läshastighet"},"secret":false,"value":"0",
  "values":[{"value":"3","label":{"en":"2×","sv":"2×"}},{"value":"2","label":{"en":"4×","sv":"4×"}},
            {"value":"1","label":{"en":"8×","sv":"8×"}},{"value":"0","label":{"en":"Max","sv":"Max"}}]},
 {"key":"mcp","scope":"standard","label":{"en":"Remote control · MCP","sv":"Fjärrstyrning · MCP"},"secret":false,"value":"1",…},
 {"key":"webhook","scope":"standard","label":{"en":"Webhooks · Address","sv":"Webhooks · Adress"},"secret":true,"is_set":false,"values":null},
 {"key":"eject","scope":"usb0","label":{"en":"Eject the disc when done",…},"secret":false,"value":"-1",
  "values":[{"value":"-1","label":{"en":"Standard (On)","sv":"Standard (På)"}},…]},
 …]}
```

The keys are those of [the config file](configuration.md), except the webhook events
(`webhook_event_rip_finished`, …, one on/off each), `webhook_progress` and a drive's
`destination`. The game list's source and filter are not settings here: the game tools take them
as arguments.

## Screens and buttons

### set_setting

Changes a setting as its screen would: the same checks and side effects, saved at once, logged in
the audit log. Answers with the setting as `list_settings` gives it.

| Argument | |
|---|---|
| `key` | The key, as `list_settings` gives it |
| `value` | One of the allowed values, or any text for a free-text setting. `4` and `"4"` are the same; `true`/`false` are `1`/`0` |
| `drive` | For a drive's own setting; left out: the standard |

`{"key":"speed","value":2}` → `{"key":"speed","scope":"standard",…,"value":"2",…}`

`{"key":"eject","value":false,"drive":"usb0"}` → `{"key":"eject","scope":"usb0",…,"value":"0",…}`

`{"key":"speed","value":"5"}` → refused: *Not an allowed value for speed: "5". Allowed: 3 (2×), 2
(4×), 1 (8×), 0 (Max).*

A secret (the webhook address, a header) can be set but is never shown:
`{"key":"webhook","value":"http://192.0.2.10:8123/api/webhook/example-id"}` →
`{"key":"webhook",…,"secret":true,"is_set":true,"values":null}`.

Setting `mcp` to 0 turns MCP off: the call is answered, and the next one gets *MCP is off*. So
does △ (reset everything) in the settings: it turns all three Remote control switches off.

### go_screen

Opens one of the app's screens the way the pad goes there, and answers with the state.

| Argument | |
|---|---|
| `screen` | `home`, `settings`, `drive_settings`, `filename`, `webhooks`, `about`, `audit_log`, `remote_control`, `games` |
| `drive` | Only with `drive_settings`, and required there |

`{"screen":"drive_settings","drive":"usb0"}`:

```json
{"screen":"drive_settings","row":0,"tab":"usb0","drive":"usb0","dialog":null,"keyboard":false,
 "buttons":[{"button":"updown","label":"Select"},{"button":"leftright","label":"Change"},
            {"button":"triangle","label":"All standard"},{"button":"circle","label":"Back"}],…}
```

### go_tab

Shows a drive's tab on the home screen, going there first. `{"drive":"internal"}` →
`{"screen":"home","row":null,"tab":"internal",…}`. A drive that is not connected: *No drive "usb3"
is there now; get_drives lists the drives.*

### press_button

Presses one button, as the pad would, and answers with the state. It is the fallback: for answering
a dialog and moving around a screen. Use the actions and the game tools for the work itself.

| Argument | |
|---|---|
| `button` | `cross`, `circle`, `triangle`, `square`, `up`, `down`, `left`, `right`, `start`, `select`, `l1`, `r1`, `l2`, `r2` |

`{"button":"r1"}` on the home screen → `{"screen":"home","row":null,"tab":"usb0",…}`

`{"button":"circle"}` on the home screen is refused: *Circle on the home screen exits the app;
MCP does not exit it. Use the controller.*

## Drive actions

`rip`, `repair`, `check`, `cancel`, `skip_rereads` and `eject` each take one argument, `drive`
(`internal` or `usbN`), and do what the button on that drive's tab does, with the same checks. The
app shows the drive's tab first. Each answers with the state, or why not.

| Tool | Does | Pad |
|---|---|---|
| `rip` | Rips the disc. It replaces an existing rip at the destination without asking, as ✕ does | ✕ |
| `repair` | Re-reads the sectors the rip log lists as bad | □ |
| `check` | Checks the drive's earlier rip against Redump from the file | □ |
| `cancel` | Cancels the drive's running job. The call is the confirmation the pad asks for | ○ |
| `skip_rereads` | Ends the slow re-reads; the rip keeps what it has | □ |
| `eject` | Ejects the disc (not while a job runs on it) | △ |

`rip` `{"drive":"internal"}` → the state, `{"screen":"home","row":null,"tab":"internal",…}`; the
drive's `state` is then `ripping` and its `job` shows the progress.

Refusals, for example:

- `repair` with no rip: *There is no rip of this disc at the drive's destination.*
- `cancel` with nothing running: *Nothing runs on this drive.*
- `rip` while a dialog is open: *A dialog is open: "…". Answer it with press_button: cross = yes,
  circle = no.*

Follow a rip with `get_drives` or `get_state`: `job` has the phase and percentages, `last` the
result when it is done.

## Game list tools

These work on the [game list](user-guide.md#game-list). A game's **id** is its source and its
place: `hdd:/dev_hdd0/PSXISO/Croc 2 (Europe) [SLES_020.88]`, plus the image's extension for an
`.iso` or `.img` (`hdd:/dev_hdd0/PSXISO/Castlevania - Symphony of the Night (USA).iso`), and with
`/.trash/` for a game in the trash. A game of several discs has the id of its first disc, and each
disc has its own id in `discs`; any of them finds the game. An id stays the same until the game is
renamed or moved; the tools that move a game answer with the new ids.

A **source** is `hdd`, `usbN` (the USB drive's port) or `all`. The **filter** is the filter
panel's, each criterion left out is *any*:

| Criterion | Values |
|---|---|
| `format` | `any`, `2352`, `2048` |
| `redump` | `any`, `verified`, `unchecked`, `no_match`, `not_verified` |
| `errors` | `any`, `with`, `without` |
| `other` | `any`, `broken`, `duplicates`, `sbi_missing` |
| `show` | `games` (default), `trash` |

For `list_games` and the actions the source and filter are only for the call. When a game tool
needs another source than the list on the screen shows, the screen switches to it, but the
source the list returns to (SELECT in the list) is not changed. Only [`show_games`](#show_games) sets the
list's own source and filter, as SELECT and the filter panel do. Neither is saved: each start of the
app shows the hard disk with no filter.

### list_games

The game list, read on its own without touching the screen.

| Argument | |
|---|---|
| `source` | Default `all` |
| `filter` | See above |
| `query` | Only games whose name has this text, any case (at most 95 bytes) |
| `offset` | The first game to give, default 0 |
| `limit` | 1 to 200, default 50 |

`{"source":"hdd"}`:

```json
{"source":"hdd","trash":false,
 "sources":[{"key":"hdd","label":"Hard disk","discs":5},{"key":"all","label":"All","discs":null}],
 "summary":{"text":"4 games · 1 re-rip","games":4,"verified":0,"errors":0,"rerip":1,"broken":0},
 "filter":{"format":"any","redump":"any","errors":"any","other":"any","show":"games"},"query":"",
 "matching":4,"offset":0,"limit":50,"more_than_limit":false,
 "games":[
  {"id":"hdd:/dev_hdd0/PSXISO/Croc 2 (Europe) [SLES_020.88]","name":"Croc 2 (Europe)","disc_id":"SLES_020.88",
   "format":"2352","tracks":1,"status":"unchecked","status_text":"Not checked","errors":0,"redump_name":"",
   "place":{"source":"hdd","label":"Hard disk","folder":"/dev_hdd0/PSXISO"},
   "duplicate":false,"duplicate_of":null,"sbi_missing":false,"in_trash":false,"ndiscs":1},
  {"id":"hdd:/dev_hdd0/PSXISO/Final Fantasy IX (Europe) (Disc 1) [SLES_029.65]","name":"Final Fantasy IX (Europe)",…,
   "ndiscs":2,"discs":[{"id":"hdd:/dev_hdd0/PSXISO/Final Fantasy IX (Europe) (Disc 1) [SLES_029.65]","disc_id":"SLES_029.65",…},
                      {"id":"hdd:/dev_hdd0/PSXISO/Final Fantasy IX (Europe) (Disc 2) [SLES_129.65]","disc_id":"SLES_129.65",…}]},
  …],
 "next_offset":null,"cut":false,"check":{"running":false,"last":null}}
```

- `status`: `verified`, `no_match`, `unchecked`, `errors`, `rerip` (a 2048 image) or `broken`;
  `status_text` is the list's own words.
- `matching` counts the games the filter and query let through; `summary` counts the whole source.
- **Paging**: `next_offset` is the `offset` of the next page, `null` when there is no more. When a
  page would be too large for one answer it ends early with `"cut":true`, and `next_offset` says
  where to go on. `more_than_limit` is true when the source has more than 1000 discs (the list
  shows the first 1000).
- `check`: a running game-list check (`running`, with its progress) or the last one's result in
  `last`:
  `{"running":false,"last":{"discs":1,"checked":0,"failed":0,"not_compared":0,"cancelled":false,"unreachable":true}}`.

`{"query":"croc","limit":1}` → one game, Croc 2; `{"limit":2}` → two games and `"next_offset":2`.

### get_game

One game as its detail view shows it, with every disc's `files` and `details` (the detail view's
lines in English).

`{"id":"hdd:/dev_hdd0/PSXISO/Croc 2 (Europe) [SLES_020.88]"}`:

```json
{"id":"hdd:/dev_hdd0/PSXISO/Croc 2 (Europe) [SLES_020.88]","name":"Croc 2 (Europe)",…,"ndiscs":1,
 "discs":[{"id":"hdd:/dev_hdd0/PSXISO/Croc 2 (Europe) [SLES_020.88]","disc_id":"SLES_020.88",…,
   "files":["Croc 2 (Europe) [SLES_020.88].cue","Croc 2 (Europe) [SLES_020.88].bin"],
   "details":[{"label":"Status","value":"Not checked"},{"label":"Format","value":"2352"},
              {"label":"Disc ID","value":"SLES_020.88"},{"label":"Size","value":"0 MB"},
              {"label":"Place","value":"Hard disk · /dev_hdd0/PSXISO"},{"label":"Cover picture","value":"No"},
              {"label":"Last checked","value":"Never"},{"label":"MD5","value":"Not computed yet"},…]}]}
```

An unknown id: *No game with the id "…" is there now: list_games gives the ids.*

### show_games

Shows the game list on the screen with a source and filter, set in one step as SELECT and the
filter panel set them. They last until the app restarts: neither is saved or in the audit log, and
each start shows the hard disk with no filter (the answer's `note` says so). Use it to show the
agent's findings on the TV; `list_games` reads the games.

| Argument | |
|---|---|
| `source` | `hdd`, `usbN` or `all`. Left out: the source shown now |
| `filter` | The criteria to set (as above); each one left out keeps its value. `"clear"` (or `{}`) clears them, as L1 does. `show` (`games` or `trash`) is the panel's Show: it switches only the view, as in the panel |

The answer is what the list shows, its summary line and how many games the filter shows
(`shown_text`, as the list's criteria line has it), and the screen state.

Duplicates on all sources, `{"source":"all","filter":{"other":"duplicates"}}`:

```json
{"result":{"source":"all","label":"All","trash":false,
  "filter":{"format":"any","redump":"any","errors":"any","other":"duplicates","show":"games"},
  "summary":{"text":"22 games · 1 verified · 2 with errors · 1 re-rip · 1 broken","games":22,"verified":1,"errors":2,"rerip":1,"broken":1},
  "shown":4,"shown_text":"4 of 22",
  "note":"The source and filter last until the app restarts; they are not saved (each start shows the hard disk with no filter)."},
 "state":{"screen":"games","row":0,…}}
```

Then `{"filter":{"redump":"verified"}}` shows the verified duplicates (the source and `other` stay),
and `{"filter":"clear"}` shows every game again.

Refusals: a source that is not connected (*USB 5 is not connected; list_games gives the sources.*,
nothing is changed), an unknown criterion or value (a parameter error that lists the allowed
ones), while the game list checks, and while a dialog is open. The list cannot filter by name, so
`show_games` has no `query`.

### check_game, check_games, cancel_check

`check_game` `{"id":…}` checks one game against Redump from its files, as □ in the list does.
`check_games` `{"source":…, "filter":…}` (both optional; source default `all`) checks every game
that has no Redump result yet and can be checked, as START does. Both need Redump on (online or a
local copy), answer at once, and the check runs on:

```json
{"result":{"started":true,"discs":1},"state":{"screen":"games",…}}
```

While it runs, `get_state` shows the screen `game_check`; when it has ended, `list_games` shows the
statuses and `check.last`. `cancel_check` (no arguments) stops it as ○ does: the game being checked
and those not reached keep the result they had. While a check runs, the other game tools are
refused; the reading tools still work.

Refusals, for example: *Redump is off: turn it on in the settings (online or a local copy) to check
games*, and *No game list check runs: there is nothing to cancel.*

### rename_game

Renames every file of the game (all its discs, the `.cue`'s `FILE` lines too), as START in the
rename view does. Logged in the audit log and undoable there.

| Argument | |
|---|---|
| `id` | The game |
| `pattern` | `%T` title, `%N` Redump's name, `%D` disc ID, `%R` region, other characters as they are (at most 95 bytes). Empty: `%T [%D]` |

`{"id":"hdd:/dev_hdd0/PSXISO/Alundra (Europe) [SLES_011.35]","pattern":"%T"}`:

```json
{"result":{"renamed":"Alundra (Europe)","note":"Renamed: Alundra",
           "discs":[{"id":"hdd:/dev_hdd0/PSXISO/Alundra","name":"Alundra"}]},
 "state":{"screen":"games",…}}
```

Refused when the name stays the same (*The name stays the same*), a game with the new name is
there, a drive or a check uses the game, the new name starts with a dot, or the pattern has a
control character.

### trash_game, restore_game

`trash_game` `{"id":…}` moves the game, every file of it, to `.trash` in its `PSXISO` folder, as △
does. `restore_game` `{"id":…}` (an id with `/.trash/`) moves it back. Both are logged in the audit
log and undoable there, and answer with the new ids:

```json
{"result":{"trashed":"Final Fantasy IX (Europe)","note":"Moved to the trash: Final Fantasy IX (Europe)",
  "discs":[{"id":"hdd:/dev_hdd0/PSXISO/.trash/Final Fantasy IX (Europe) (Disc 1) [SLES_029.65]","name":"Final Fantasy IX (Europe) (Disc 1) [SLES_029.65]"},
           {"id":"hdd:/dev_hdd0/PSXISO/.trash/Final Fantasy IX (Europe) (Disc 2) [SLES_129.65]","name":"Final Fantasy IX (Europe) (Disc 2) [SLES_129.65]"}]},
 "state":{…}}
```

`restore_game` on a game that is not in the trash: *The game is not in the trash: there is nothing
to restore.*

### empty_trash

Deletes every game in the trash of **one** source, as START and *yes* in the trash do. This is
final: the files cannot be restored, not even from the audit log.

| Argument | |
|---|---|
| `source` | Required: `hdd` or `usbN`. `all` is refused: a trash is emptied one source at a time |
| `confirm` | Must be `true`; without it nothing is deleted |
| `expect` | Optional: what you saw in that trash, either the number of games, or the id of **every disc** in it (a game's id and, for a game of several discs, each id in its `discs`), in any order |

List the trash first (`list_games` with the source and `"filter":{"show":"trash"}`) and pass what
you saw as `expect`: if the trash holds anything else by then, nothing is deleted.

`{"source":"hdd","confirm":true,"expect":["hdd:/dev_hdd0/PSXISO/.trash/Final Fantasy IX (Europe) (Disc 2) [SLES_129.65]","hdd:/dev_hdd0/PSXISO/.trash/Final Fantasy IX (Europe) (Disc 1) [SLES_029.65]"]}`:

```json
{"result":{"emptied":true,"note":"The trash was emptied: 4 files deleted"},"state":{…}}
```

Refusals:

- Without `confirm`: *Not emptied: emptying the trash deletes its games' files for good (they
  cannot be restored, not even from the audit log). Call empty_trash again with confirm: true to
  empty it.*
- `expect` does not match: *Not emptied: the trash of Hard disk has 1 game now, not the 2 expected.
  Nothing was deleted: list_games (this source, filter show: trash) shows what is there.* (With ids:
  *… has 2 discs now, not the 1 expected.*)
- An empty trash: *The trash is empty: there is nothing to delete.*

## Screen names

`get_state` names the screen:

| `screen` | |
|---|---|
| `home` | The drives' tabs |
| `settings` | The settings |
| `drive_settings` | A drive's own settings (SELECT on its tab); `drive` says which |
| `filename` | The file name pattern |
| `webhooks`, `webhook_headers` | The webhook settings and their headers |
| `about` | About PSX RiPPER |
| `audit_log` | The audit log |
| `remote_control` | The Remote control menu |
| `redump_download` | Downloading the local Redump copy |
| `games` | The game list |
| `game_filter` | The game list's filter panel |
| `game_detail` | A game's details |
| `game_rename` | The rename view |
| `game_check` | The progress of a game-list check |
| `dialog` | A dialog of the app's is open; `dialog.over` names the screen under it |

`go_screen` opens the screens in its list; the others are reached from them (with `press_button`)
or by the app itself.

## Rules

- **Actions first, buttons as a fallback.** The actions and the game tools do the work directly,
  with the pad's checks. `press_button` is for answering a dialog (✕ yes or OK, ○ no) and for
  moving around a screen; it never presses into the console's on-screen keyboard.
- **○ on the home screen is refused**: it would exit the app, and MCP with it.
- **A cancel over MCP is the confirmation.** Where the pad asks *Cancel the rip?*, the `cancel` call
  itself is the yes, and the log says so.
- **`rip` replaces an existing rip** of the disc at the destination without asking, like ✕.
- **Dialogs and the keyboard.** While the app's own dialog is open, the actions, the game tools,
  `go_screen` and `go_tab` are refused with the dialog's text and the buttons that answer it: answer
  it with `press_button` first. While the console's on-screen keyboard is open, these and
  `press_button` are refused: it is answered on the console. The reading tools and `set_setting`
  work all the time.
- **Busy after 5 s.** An action waits at most 5 seconds for the app to take it and finish it. If
  the app is in the middle of something long, or the console's own menu is open, the answer is
  *Busy: …* with the screen it is on; a request the app took but did not finish in time answers
  *The app took the request but did not finish it within 5 s*. Look at `get_state` and try again.
- **One request at a time.** Two agents (or two calls) are served one after the other.
- **The game list's source and filter are not saved.** A game tool may switch the list on the
  screen to another source, but the source the list returns to stays the one chosen with SELECT
  or `show_games` (which sets the source and filter as SELECT and the filter panel do). Both last
  until the app restarts; each start shows the hard disk with no filter.

## Security

- **Your home network only.** MCP has no password and no login: anyone on the network who can reach
  the console can use it while it is on. Never forward port 8090 to the internet. Turn MCP off when
  you do not use it.
- **Web pages cannot use it.** A request from a web browser carries an `Origin`; the app answers
  only requests with no `Origin` (programs such as MCP clients) or from its own pages, exactly
  `http://<the console's address>:8090` (not a host name for it, not `localhost`). Anything else
  gets *Origin not allowed*, so a web page you visit cannot use your browser to control the
  console. The remote view's pad has the same check: open its page by the console's address.
- **Secrets are never shown.** The webhook address and the headers can be set over MCP but never
  read: `list_settings` and `set_setting` say only whether they are set, `read_log` shows
  `[secret]` in their place, and the audit log notes only *set* or *cleared*.
- **Nothing else is reachable.** The app's debug commands are not part of MCP.
- **Connection limits**, so a slow or stuck client cannot hold the app: at most 4 requests at once
  (one more gets *503, try again*), a request must arrive whole within 10 seconds and be at most
  16 KB, a client that reads nothing for 10 seconds is dropped, and an answer is at most 512 KB.
  The web server runs at a low priority: a rip does not slow down because MCP is on.

## Limits

- `list_games` has no time limit. The first call reads the source's folders (and with `all`, every
  source), which on a large USB drive can take a while; the scan is kept, so the next call, and a
  retry after a client gave up waiting, is fast. A new scan is made when the app changed files,
  when the hard disk's folder changed, or after a minute for a USB drive.
- A game action on another source than the one on the screen first reads that source; on a large
  USB drive this can take longer than the 5 seconds and answer *Busy*. The screen stays on the new
  source, so trying again works. So does `show_games`: its source and filter are set before the
  read, and stay set.
- A page of `list_games` with many large games can be cut (`cut`); a smaller `limit` helps. If not
  even one game fits: *The answer is too large: use a smaller limit.*
- A game whose file name is not valid UTF-8 can be listed but not acted on by id: its id shows
  `?` for the invalid bytes and matches no file, so `get_game`, `check_game`, `rename_game`,
  `trash_game` and `restore_game` answer that no such game is there, and `empty_trash`'s `expect`
  with ids never matches it. Use the console for it, and empty such a trash with a count.
- `get_screenshot` does not show the console's own dialogs or keyboard.
