# Configuration

All settings are stored in `/dev_hdd0/tmp/psxrip.cfg` (written with the defaults on the first
start), one `key=value` per line. It can be edited over FTP instead of in the app, which is
handy for the webhook address and headers. **The app reads the file only when it starts:**
edit it while the app is closed, or restart the app afterwards. Changing a setting in the
app writes the whole file again.

| Key | Values | Setting |
|---|---|---|
| `outdir` | `/dev_hdd0/PSXISO`, `/dev_usbNNN/PSXISO`, `exfatN:/PSXISO` | Destination of the internal drive |
| `outdir_usb0` … `7` | as `outdir` | Destination of the USB SuperDrive on that USB port |
| `name_pattern` | e.g. `%T [%D]` | File name: `%T` title, `%R` region, `%D` disc ID, `%N` Redump name |
| `retries` | 1, 2, 4, 8, 16, 32 | Re-reads on read errors (the standard for all drives) |
| `speed` | 0 = max, 1 = 8x, 2 = 4x, 3 = 2x | Read speed |
| `eject` | 0 / 1 | Eject the disc when done |
| `autostart` | 0 / 1 | Rip a disc when it is inserted while the app runs, not one already in a drive when the app starts or the drive connects (a disc already ripped there with known errors is repaired instead) |
| `redump` | 0 = off, 1 = online, 2 = local copy | Check against Redump |
| `check` | 0 = off, 1 = check and re-read, 2 = bit-perfect | Sector check |
| `drive_bd`, `drive_usb0` … `7` | `retries,speed,eject,autostart,check` | One drive's own settings instead of the standard (SELECT on its tab); `-1` = the standard |
| `remote` | 0 / 1 | Remote control → Remote view: the screen and the pad page on port 8090 |
| `mcp` | 0 / 1 | Remote control → MCP: an AI agent can read and control the app at `/mcp` (see [MCP](mcp.md)); default 0 |
| `status_json` | 0 / 1 | Remote control → status.json: `/status.json` on port 8090 |
| `webhook_on` | 0 / 1 | Webhooks |
| `webhook` | `http://host:port/path` | Webhook address |
| `webhook_headers_on` | 0 / 1 | Send the extra headers |
| `webhook_header1` … `3` | `Name: value` | Extra headers (e.g. authentication) |
| `webhook_events` | bit mask, bit n = event n below | Events that are sent |
| `webhook_step` | 1, 2, 5, 10, 15, 25, 50 | Progress every N percent |
| `audit_mb` | 1, 2, 4, 8, 16, 32, 64, 128, 256 | Size limit in MB of the audit log's sector copies (default 256) |

The web server on port 8090 runs while any of `remote`, `mcp` and `status_json` is on, and each
path answers only while its own switch is on. A config file from before these switches (with
`remote` but no `status_json` line) keeps what it had: `status_json` gets the value of `remote`,
and `mcp` stays 0.

The game list's source and filter are not settings: they last until the app restarts, and each
start shows the hard disk with no filter. Older versions saved them as `games_source` and
`games_filter`; such lines are ignored when the file is read and dropped at the next save.

Webhook event bits: 0 `disc_identified`, 1 `rip_started`, 2 `rip_progress`, 3 `rip_finished`,
4 `rip_cancelled`, 5 `rip_failed`, 6 `repair_finished`, 7 `disc_ejected`,
8 `disc_not_identified`, 9 `app_started`, 10 `disc_lost`.

`disc_lost` is sent instead of `rip_cancelled` / `repair_finished` when a rip or repair stopped
because the disc left the drive without O being pressed (a USB SuperDrive pushes out a disc it
cannot read; the drive then answers "medium not present"). A rip removes its unfinished file; a
repair keeps the sectors it already wrote.

## Audit log files

The audit log is in `/dev_hdd0/tmp/psxrip_audit/`:

| File | Contents |
|---|---|
| `audit.log` | One text row per change: time, source, disc, what, before and after. Settings that are secret (webhook address and headers) only say set or cleared |
| `job<N>.bin` | The old and new bytes (2 x 2352) of every sector job N changed; this is what a sector undo reads |

The `job<N>.bin` files together stay under `audit_mb`; past it the oldest are deleted (never
one of a running job), and their rows can no longer be undone.

## Remote view and webhooks

- **Remote control** (setting): a small web server on port 8090 with three switches. **Remote
  view**: `http://<ps3>:8090/` shows the app's screen as an MJPEG stream (about 2 frames per
  second) with a pad, and `/shot.jpg` a single frame. **status.json**: `/status.json`, the state of
  every drive (see below). **MCP**: `/mcp`, for an AI agent (see [MCP](mcp.md)). System dialogs are
  drawn by the PS3 on top of the app and are not in the stream.
  JPEG encoding uses [stb_image_write](https://github.com/nothings/stb) (public domain / MIT).
- **Webhooks** (setting): the events above are sent as an HTTP POST with a JSON body, for
  example to a Home Assistant webhook (`http://homeassistant.local:8123/api/webhook/<id>`).
  X on the Webhooks row opens their settings: the address (PS3 on-screen keyboard), up to three
  extra headers, which events are sent, how often progress is sent, and a test. Only
  `http://` is supported. `disc_identified` / `disc_not_identified` are sent for a disc inserted
  while the app runs; a disc already in the drive at start is part of `app_started`.

Example payload:

```json
{"event":"rip_finished","app":"PSX RiPPER","title":"Alundra","serial":"SLES_011.35",
 "sectors_done":257659,"sectors_total":257659,"unreadable":0,"fixed":1,"bad":0,
 "redump":"verified","redump_name":"Alundra (Europe)","seconds":221,
 "path":"/dev_hdd0/PSXISO/Alundra [SLES_011.35].bin"}
```


### `/status.json`

One object for the app and a `drives` array with every drive (the internal one first, then the
USB drives by port). `busy` is true while any drive is in use or the lv1 patch is on. It answers
only while the status.json switch (Settings → Remote control → status.json) is on; otherwise the
web server answers 404, or nothing when all three switches are off.

```json
{"app":"PSX RiPPER","version":"1.0.0","busy":true,"state":"ripping","drives":[
 {"drive":"internal","id":"internal","model":"PS3 Blu-ray drive","state":"ripping",
  "title":"Mega Man X4","serial":"SLUS_005.61","size_mb":564,"redump_name":"Mega Man X4 (USA)",
  "redump":"unchecked","already_ripped":false,"known_errors":0,
  "destination":"/dev_hdd0/PSXISO","file":"Mega Man X4 [SLUS_005.61]",
  "job":{"phase":"verifying","percent":29,"sectors_done":251793,"sectors_total":251793,
         "pass_done":12000,"pass_total":41237,"unreadable":0,"fixed":1,"bad":0}},
 {"drive":"usb","id":"usb0","port":0,"model":"Apple SuperDrive","state":"empty",
  "last":{"result":"done","disc":"Alundra (Europe) (SLES_011.35)","summary":"klar på 14:02",
          "redump":"verified","needs_repair":false}}]}
```

- `drive`: `internal` or `usb` (with its `port`); `id`: `internal` or `usbN` (N is the port), the
  name the [MCP](mcp.md) tools take for the drive.
- `state`: `empty`, `reading` (identifying), `ready`, `not_psx`, `no_answer` (a USB drive that
  stopped answering), `ripping`, `repairing`.
- `redump` (the disc here, and the last rip): `verified`, `no_match`, `unchecked`.
- `job` while a drive works: `phase` is `reading`, `verifying` (sectors without a checksum read
  again), `rereading` (slow re-reads of unreadable sectors), `finishing`, `checking` (an earlier
  rip against Redump) or `repairing`; `percent` is the phase's own progress.
- `last`: the last rip, repair or check on that drive (`done`, `failed`, `cancelled`); it stays
  after the disc is ejected. `summary` is the text the app shows, in its language.

## Verification against Redump

PSX RiPPER can compare a rip with the [Redump](http://redump.org/) database. No Redump data
is included in the app; it is fetched at runtime. Setting *Check against Redump*:

- **Off**: no network access.
- **Online**: the disc is looked up on redump.org by its serial. The start screen shows the
  Redump name; after ripping, the track MD5s are compared with Redump's.
- **Local copy**: select *Local Redump copy* in the settings and press ✕ to download the
  PlayStation dat (about 4 MB) to `/dev_hdd0/tmp/psxrip/`, where it is reduced to a compact
  index (about 1.4 MB). The start screen then names the disc by its size before ripping,
  and the MD5s are checked afterwards, without internet access.

Tracks are split the way Redump splits them: every track after the first starts at its
pregap (INDEX 00), which is read from the Q subchannel, so multi-track discs compare too.
