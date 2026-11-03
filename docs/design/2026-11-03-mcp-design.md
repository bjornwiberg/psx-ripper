# MCP server in PSX RiPPER — design

Status: draft for review, 2026-11-03.

## Intent

An MCP (Model Context Protocol) server inside the app, so an AI agent (Claude Code, or any MCP
client) on the same network can **use the app the way a person with the pad does**, and, for
development and troubleshooting, **see and steer the drive at a low level while a job runs**.

Two kinds of use, both real:

- **Everyday control**: "show the USB 1 tab", "open the settings", "cancel the rip on USB 1 and
  eject the disc", "what are the choices for the Redump setting?" (answer: off, online, local
  copy), "change the file name pattern to …". Speech is not part of this: the user talks to
  their agent (for example by dictation in the Claude Code app); the agent talks MCP.
- **Troubleshooting and development**: "find out why the SuperDrive on USB 0 pushes out Tekken 2
  when the repair starts". The agent must be able to see exactly what happens during a read,
  rip or repair (sector by sector, try by try, what the drive answered), and to steer it: go back
  to the first bad sector, lower the speed, change the tries per sector, send its own commands to
  the drive. The developers (and the agent helping them) are meant to use this server
  themselves while building and debugging the app.

The server is for **every user** of the app, not a developer build.

## Goals

1. Everything the pad can do on the app's own screens can be done over MCP, under the **same
   rules**: what the pad may not do in a state (eject while ripping, change a ripping drive's
   settings), MCP may not do either, and gets the reason in the app's own words.
2. What MCP does is **visible on the TV**: "open the settings" opens them on the screen. MCP
   drives the app; it does not work beside it.
3. Settings **describe themselves**: an agent can list every setting, its current value and its
   allowed values with the app's labels (both languages), and change it.
4. Low-level insight and control of running jobs (phase 2) and raw drive commands (phase 3), so
   that drive problems can be diagnosed with the console as the lab.
5. File access to the rips (phase 4) and safe changes to them (phase 5): rename, delete into a
   trash, with an audit log that can roll any change back.

## Non-goals

- Speech, wake words, voice assistants.
- Starting the app when it is not running (the server lives inside the app). Starting it via
  webMAN is a separate matter.
- Running anything on the PC side: no bridge, no proxy; the agent connects straight to the PS3.
- Access from outside the home network.

## Anti-goals (it would be a failure even if it works)

- **Anything destructive to the console**: nothing that can write to the PS3's hard disk or flash
  outside the app's own files and folders. The `/dbg` debug operations `devcmd` and `scsi` to
  system storage (and anything like them) are never reachable over MCP.
- **Anything that can damage a drive or a disc in it**: no firmware updates, no writing or
  erasing discs (a SuperDrive is a writer).
- Ripping, repairing or settings ending up in a state the pad could not have produced.
- A noticeably bigger or slower app. The package stays well under 1 MB (about 690 KB now); a
  rip must not slow down because the server is on.
- Deleting a rip for real in one step (deleting goes to a trash; only emptying the trash is
  final, and that is logged).

## Constraints

- PS3 homebrew (PSL1GHT, C/C++), no new heavy libraries. A small JSON parser under a
  GPL-3.0-compatible license (for example jsmn, MIT) is fine; so is writing one.
- Everything goes through the app's existing small web server on port 8090
  (`source/remote.cpp`): a thread per connection, HTTP/1.0. MCP is one more path on it.
- The app is drawn on one UI thread; rips, repairs and checks run on per-drive job threads
  (cards); the pad is read by `pad_pressed()`, and the remote view's pad already injects buttons
  through `g_remote_btn`. Anything that changes the screen must happen on the UI thread.
- Every UI text exists in English and Swedish (`source/lang.cpp`). Code, comments and logs in
  English. The repository's rules in `CLAUDE.md` and `CONTRIBUTING.md` apply (no AI attribution,
  Conventional Commits, host UI tests for every screen change).
- No authentication (decided). Protection is: MCP is off until the user turns it on, an
  on-screen indicator while it is on, the operations blocklist/allowlist below, and the audit log.

## Decided by the user

- MCP is its own setting. The settings row "Remote view" becomes **"Remote control"**
  (Swedish "Fjärrstyrning"); X opens a new menu with three switches: **remote view** (the
  picture and pad in the browser, as today), **MCP**, **status.json**. status.json gets no link
  on the page: users know it exists and read it via an agent, FTP or the browser.
- While MCP is on, **[MCP]** is shown in the **top left** corner of the screen (the top right
  belongs to notifications).
- The server starts with the app and stops with it.
- Raw commands to the optical drives are allowed (they are instructions to the drive, not writes
  to the console); anything that writes to the console's storage is not.
- File access covers every place the app can read: the hard disk (`PSXISO`) and USB sticks or
  drives the app sees as destinations.
- Renaming rips is allowed. Deleting is allowed but moves the rip to a trash folder named
  **`.trash`** (English name) in the same place; the trash can be emptied **both on the console
  (a new Trash menu under the settings) and over MCP**.
- An **audit log** in the app records every change with its state before and after (for a sector:
  its old and new bytes; for a setting: old and new value; for a rename: old and new names), and
  any entry can be **rolled back**; the rollback is logged too.
- Phase order: phase 1 is app control (below). All other parts follow in the order of this
  document.

## Phases

### Phase 1 — app control

The agent can do everything the pad can on the app's own screens, and see the result.

- **State**: every drive (tab) with its disc, state, job and progress, the screen shown and the
  selected tab; the same information as status.json and more (which screen, which row is
  selected, the hint bar's buttons).
- **Screen**: a screenshot of the canvas as an image (the remote view already encodes JPEG), so
  the agent sees what the user sees.
- **Navigation**: select a tab ("show USB 1"), open the settings / a drive's settings / webhooks /
  file name / about / trash, go back. Implemented as requests the UI thread carries out, so they
  are drawn like a pad press.
- **Actions**, each with the same preconditions as on the pad: rip, repair, check against Redump,
  cancel, skip re-reads, eject. A refused action returns the reason (the same words the screen
  would use) and, where it helps, what is possible instead.
- **Buttons**: pressing a pad button, as the remote view's pad does, as a fallback for anything
  without its own tool (dialogs, system keyboard is out of reach).
- **Settings**: list all settings (general, per drive, webhooks, file name pattern, remote
  control) with current value, allowed values and labels in both languages, and set them. The
  same validation and the same side effects as the screens (`cfg_save`, per-drive store, the
  "-1 = standard" rule). Webhook header values are never returned (they may hold secrets); the
  agent sees whether one is set.
- **Remote control menu, [MCP] indicator, status.json switch** (UI changes above), with host UI
  test scenarios.
- **Log**: read the app's log (`psxrip.log`) from a given line on, so the agent can follow what
  happens.

### Phase 2 — insight into and control of running jobs

- **Live read trace** per drive: a ring buffer of read events (sector, count, try, the drive's
  answer or sense code, time taken, speed in use, phase), readable from a cursor so the agent can
  follow a job without missing events. Small and fixed in size; a job never waits for it.
- **Control a running job**: change the speed, the tries per sector and the re-read behaviour of
  that drive's job while it runs; pause and resume; go back to a given sector (or the first bad
  one) and read from there again; skip ahead. Each change is logged and shown on the drive's page.
- Changes made over MCP to a running job last for that job only, unless the agent also changes
  the setting.

### Phase 3 — raw drive commands

- Send a CDB to an optical drive (internal BD drive or a USB SuperDrive) and get status, sense data
  and the returned data. Allowed also while a job runs on that drive (the command goes in between
  the job's own commands; the USB driver already serialises commands per drive).
- **Only an allowlist of operation codes** is accepted (read, status and volatile settings), for
  example: TEST UNIT READY, REQUEST SENSE, INQUIRY, MODE SENSE, MODE SELECT with the save bit
  off, START STOP UNIT, PREVENT/ALLOW MEDIUM REMOVAL, READ CAPACITY, READ(10/12), SEEK, READ
  SUB-CHANNEL, READ TOC/PMA/ATIP, READ HEADER, GET CONFIGURATION, GET EVENT STATUS
  NOTIFICATION, READ DISC/TRACK INFORMATION, SET CD SPEED, SET STREAMING, MECHANISM STATUS, READ
  CD, READ CD MSF, GET PERFORMANCE, PLAY/PAUSE/STOP AUDIO. Everything else is refused, notably
  WRITE BUFFER (firmware), every WRITE/WRITE AND VERIFY, BLANK, FORMAT UNIT, RESERVE TRACK,
  CLOSE TRACK/SESSION, SEND CUE SHEET, SEND OPC and all vendor-specific codes (0xC0–0xFF).
- Never any command to system storage (hard disk, flash): those stay in `/dbg`, which is not
  reachable over MCP.

### Phase 4 — reading files

- List the destinations the app knows (hard disk, USB sticks/drives) and their `PSXISO` folders.
- List rips (the files that belong together: .bin, .cue, .log, .md5, and the app's .jpg if any)
  and read text files (logs, cue, md5) and byte ranges of images (for example a sector), and
  compute an MD5 of a range or a track.
- Paths are limited to the app's destinations and its own files (`/dev_hdd0/tmp/psxrip.*`).

### Phase 5 — changing files, audit log, rollback, trash

- **Rename a rip**: all its files, and the file name inside its .cue.
- **Delete a rip**: move all its files into `.trash` in the same destination (a move on the same
  disk, fast). **Restore** moves them back. **Empty the trash** deletes for real; possible on the
  console (new Trash menu under the settings: what is in it, restore one, empty) and over MCP.
- **Audit log** of every change, whoever made it (MCP, pad, remote view, the app itself during a
  repair): time, source, what, state before and after. Sector writes keep the sector's old bytes
  (2352 each), settings their old value, renames the old names, trash moves the old place.
- **Rollback** of any entry restores the state before it, and is itself an entry (so it can be
  rolled back). A rollback that no longer fits the current state (the file changed since) is
  refused with the reason.
- Readable over MCP and visible on the console (at least as a list in the Trash/Remote control
  area; the builder may choose where).

## Protocol and connection

- MCP over HTTP at `http://<ps3>:8090/mcp`, JSON-RPC 2.0, the "Streamable HTTP" transport in its
  simplest form: POST a request, get one JSON response (no server-sent events needed). Methods:
  `initialize`, `tools/list`, `tools/call`, `ping`, notifications accepted and ignored.
- When MCP is off, `/mcp` answers that it is off (and how to turn it on), nothing else.
- The agent is configured once, for example in Claude Code with
  `claude mcp add --transport http psx-ripper http://<ps3>:8090/mcp`. The user documentation
  says how.

## Calls made by the designer (not asked of the user; check these)

- **The internal drive and the lv1 patch (phase 3).** The PS3's drive only reads a PS1 disc
  while the lv1 storage patch is on. A raw command to the internal drive when no job holds it
  does what identification does today: patch, open, send, close, unpatch, within that one call;
  nothing is kept on between calls. While a job holds the drive, the command goes through the
  job's open drive. Only the fixed BD drive device is reachable; no other storage device id is
  ever accepted. The startup check that repairs a patch left on after a crash covers the rest.
- **Pad rules vs. phases 2 and 3.** Phase 1 follows the pad's rules exactly. Phases 2 and 3
  deliberately go further (changing speed, tries and position of a running job, raw commands
  during a job): that is their purpose. Two limits stay: while a job runs on a drive, raw START
  STOP UNIT (eject/load) and PREVENT/ALLOW MEDIUM REMOVAL are refused for that drive (cancel the
  job first), and MODE SELECT is only accepted with the save bit off, as everywhere.
- **Marking steered jobs.** A rip or repair that was changed over MCP while it ran (phase 2) or
  got raw commands on its drive (phase 3) says so in its `.log` (a line per change: what, when)
  and in its result line. Redump verification is unaffected (it compares the image's hashes).
- **A repair in the audit log** is one entry: every sector it wrote (old bytes) plus the rip's
  `.md5` and `.log` as they were before. Rolling it back restores all of them together, never a
  part. If the size cap has dropped any of that entry's old sectors, its rollback is refused as a
  whole (the entry stays in the log, marked as no longer reversible).

## Left to the builder

- The tool names, their argument and result shapes, and how they are grouped. Results should be
  plain and short, with the app's own words for states and settings.
- How UI requests reach the UI thread (a request queue next to `g_remote_btn` is the obvious
  path), and how action rules are shared between the pad handlers and MCP so they cannot drift
  apart.
- The JSON parser (library or own), size limits of requests and responses.
- Where the audit log and the sector blobs live and how they are bounded (a size cap with the
  oldest sector blobs dropped first is fine; the log entries themselves stay).
- How the read trace is stored and how long it reaches back.
- Host tests: the host simulator (`make -C tests/host remote`) runs the real web server, so MCP
  requests can be tested there against the app's real screens; UI test scenarios for the new
  menus and the indicator.
- User documentation (`docs/configuration.md`, `docs/user-guide.md`, a new `docs/mcp.md` listing
  the tools) and the README.
