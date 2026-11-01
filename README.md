# PSX Ripper

A PS3 homebrew app that dumps your own PlayStation 1 discs from the PS3's Blu-ray drive
straight to the internal hard disk, ready to play with webMAN MOD / Cobra.

- Raw 2352-byte sectors to `/dev_hdd0/PSXISO/<Title> [<SERIAL>].bin` + `.cue`
  (all tracks, including CD audio)
- Game title looked up offline from a built-in database (11,500+ PS1 serials), so webMAN shows
  e.g. *Alundra* instead of the disc's volume label
- LibCrypt support for the 197 PAL discs known to use it (built-in list): writes
  `/dev_hdd0/tmp/sbi/<SERIAL>.sbi`, which Cobra 8.x reads when the image is mounted
  (see [Status](#status)). Other discs get no LibCrypt check result and no `.sbi`
- Unmounts any image mounted by webMAN before reading, and offers to refresh webMAN's game
  list when you exit
- Detects disc insertion while the app is open
- Automatic mode: after a rip the disc is ejected, and the next disc you insert is ripped
  without asking. Press O to leave automatic mode
- Own on-screen UI: the start screen identifies the disc in the drive (title, serial, size) with
  ✕ rip · △ eject · □ settings · ○ exit, and a settings menu
  (↑↓ select, ←→ change and save, ○ back) with destination (internal HDD or a connected USB
  drive), re-reads on read errors, read speed (Max/8×/4×/2×), eject on/off and "rip
  immediately when a disc is inserted", saved in `/dev_hdd0/tmp/psxrip.cfg`
- English and Swedish UI, picked from the PS3 system language
- Works fully offline once installed

## Requirements

- PS3 with CFW and Cobra (developed and tested on **Evilnat 4.93**)
- PS3MAPI available (it is on Evilnat/Cobra); used for lv1 peek/poke
- webMAN MOD is optional (unmount and game-list refresh)

## Usage

1. Install `psxripper.pkg` (Package Manager → Install Package Files, or webMAN *Install PKG*).
2. Start **PSX Ripper** from the Game column.
3. Insert a PS1 disc. The start screen identifies it (title, serial, size).
4. ✕ rips, △ ejects, □ opens the settings, ○ exits. A full CD takes about 3.5–4 minutes.
5. When done the disc is ejected; insert the next disc to rip it automatically.

Settings (saved as soon as they are changed): destination (internal HDD or a USB drive),
re-reads on read errors, read speed, eject when done, rip immediately on insert, and the
Redump check described below.

Each rip gets a `.md5` file with the MD5 of every track. A log is written to
`/dev_hdd0/tmp/psxrip.log`.

## Verification against Redump

PSX Ripper can compare a rip with the [Redump](http://redump.org/) database. No Redump data
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

## Configuration file

All settings are stored in `/dev_hdd0/tmp/psxrip.cfg` (written with the defaults on the first
start), one `key=value` per line. It can be edited over FTP instead of in the app, which is
handy for the webhook address and headers. **The app reads the file only when it starts:**
edit it while the app is closed, or restart the app afterwards. Changing a setting in the
app writes the whole file again.

| Key | Values | Setting |
|---|---|---|
| `source` | `bd` = the PS3's drive, `usb` = Apple USB SuperDrive | Drive |
| `outdir` | `/dev_hdd0/PSXISO`, `/dev_usbNNN/PSXISO`, `exfatN:/PSXISO` | Save to |
| `retries` | 1, 2, 4, 8, 16, 32 | Re-reads on read errors |
| `speed` | 0 = max, 1 = 8x, 2 = 4x, 3 = 2x | Read speed |
| `eject` | 0 / 1 | Eject the disc when done |
| `autostart` | 0 / 1 | Rip immediately when a disc is inserted |
| `redump` | 0 = off, 1 = online, 2 = local copy | Check against Redump |
| `check` | 0 = off, 1 = check and re-read, 2 = bit-perfect | Sector check |
| `remote` | 0 / 1 | Remote view (port 8090) |
| `webhook_on` | 0 / 1 | Webhooks |
| `webhook` | `http://host:port/path` | Webhook address |
| `webhook_headers_on` | 0 / 1 | Send the extra headers |
| `webhook_header1` … `3` | `Name: value` | Extra headers (e.g. authentication) |
| `webhook_events` | bit mask, bit n = event n below | Events that are sent |
| `webhook_step` | 1, 2, 5, 10, 15, 25, 50 | Progress every N percent |

Webhook event bits: 0 `disc_identified`, 1 `rip_started`, 2 `rip_progress`, 3 `rip_finished`,
4 `rip_cancelled`, 5 `rip_failed`, 6 `repair_finished`, 7 `disc_ejected`,
8 `disc_not_identified`, 9 `app_started`.

## Remote view and webhooks

- **Remote view** (setting): a small web server on port 8090. `http://<ps3>:8090/` shows the
  app's screen as an MJPEG stream (about 2 frames per second), `/shot.jpg` a single frame and
  `/status.json` the current state (title, serial, progress, unreadable/fixed sectors, Redump
  result). System dialogs are drawn by the PS3 on top of the app and are not in the stream.
  JPEG encoding uses [stb_image_write](https://github.com/nothings/stb) (public domain / MIT).
- **Webhooks** (setting): the events above are sent as an HTTP POST with a JSON body, for
  example to a Home Assistant webhook (`http://homeassistant.local:8123/api/webhook/<id>`).
  X on the Webhooks row opens their settings: the address (PS3 on-screen keyboard), up to three
  extra headers, which events are sent, how often progress is sent, and a test. Only
  `http://` is supported. `disc_identified` / `disc_not_identified` are sent for a disc inserted
  while the app runs; a disc already in the drive at start is part of `app_started`.

Example payload:

```json
{"event":"rip_finished","app":"PSX Ripper","title":"Alundra","serial":"SLES_011.35",
 "sectors_done":257659,"sectors_total":257659,"unreadable":0,"fixed":1,"bad":0,
 "redump":"verified","redump_name":"Alundra (Europe)","seconds":221,
 "path":"/dev_hdd0/PSXISO/Alundra [SLES_011.35].bin"}
```

## Damaged discs

Every data sector is checked (sync, address, EDC) while reading; the drive can return a
damaged sector as a successful read. Bad sectors are re-read. The first pass keeps moving
through scratches (the drive's own retries are set low, a damaged area gets one try per
sector); afterwards a second pass retries the failed sectors at 2x with the drive's normal
retries and patches them into the image. O during the second pass keeps the image as it is.

## How it works

On 4.2x+ firmwares a homebrew process is not allowed to read raw sectors from the BD drive
until the drive has been re-authenticated, and the lv1 storage manager refuses that by
default. PSX Ripper does the same as IRISMAN's PS1 copy:

1. Applies IRISMAN's lv1 storage-manager patch (4 instructions at `0x16fa60`, `0x16fa84`,
   `0x16fafc`, `0x16fb04`) via PS3MAPI. The current values are checked first; if they don't
   match the expected code the app does **not** patch and stops. The originals are saved to
   `/dev_hdd0/tmp/psxrip_lv1.bak` and written back when ripping finishes or the app exits.
   The patch is RAM-only; a reboot also removes it.
2. Resets and authenticates the drive (syscall 864, as IRISMAN's `Reset2_BDVD`).
3. Reads 1024 sectors per `READ CD` (2352 bytes + 16 bytes formatted Q subchannel each,
   syscall 604, DMA) while a second thread writes the previous block to disk.
4. Learns the drive's Q subchannel offset from the first 512 sectors. For PAL discs, sectors
   whose Q differs from the expected value are re-read 4 times; only stable mismatches that
   come in pairs 5 sectors apart are kept. A `.sbi` is only written when the serial is on the
   known LibCrypt list (`db/libcrypt_serials.txt`): normal discs also have stable Q errors,
   which must not end up in an `.sbi`.

The image is written as `.bin.part` and renamed when complete, so an existing copy survives
a cancelled re-rip.

## Status

| Feature | Status |
|---|---|
| Raw rip, CUE, title lookup | Tested (Evilnat 4.93, ~2.7 MB/s). MD5 identical to Redump for Final Fantasy VIII (USA) disc 1 and Final Fantasy IX (Europe) disc 1 |
| Rip boots in webMAN/Cobra | Tested |
| LibCrypt detection on protected PAL discs | Tested on an original disc: the `.sbi` for Final Fantasy IX (Europe) disc 1 matches the disc's known LibCrypt sectors byte for byte |
| Multi-track discs (CD audio) | Implemented, not yet verified |

LibCrypt protection data lives in the disc's subchannel. A disc that is not an original
(for example a CD-R burned from an image) usually lacks it. The ripper then reports that no
protection data was found, and the game may not start. The `.sbi` is only ever created from
the disc in your drive; PSX Ripper does not ship or download `.sbi` files.

Verify your rips against [Redump](http://redump.org) (webMAN can compute MD5 with
`/md5.ps3<path>`).

## Ideas

- **Several drives in parallel** (the PS3 drive and up to two Apple SuperDrives, three discs at
  once): see [docs/PARALLEL.md](docs/PARALLEL.md).

## USB SuperDrive

An Apple USB SuperDrive (05AC:1500) can be used instead of the PS3's drive: *Settings → Drive*.
The PS3 system lists USB optical drives but passes no commands to them, so the app has its own
USB driver (`source/usbodd.cpp`, USB Mass Storage Bulk-Only Transport). It sends Apple's unlock
command so the slot takes a disc, waits for the drive to spin up (up to a minute on the PS3's bus
power, no Y-cable needed), reads with the same READ CD and sector checks as the internal drive,
and ejects with START STOP UNIT (the drive has no button). Its Q subchannel trails by one sector;
the ripper's Q calibration accounts for that.

## Building

The build runs in Docker with a prebuilt PSL1GHT toolchain:

```sh
docker build --platform linux/amd64 -t ps3dev-psl1ght docker/
docker run --rm --platform linux/amd64 -v "$PWD":/src ps3dev-psl1ght make pkg
```

This produces `psxripper.pkg`. To regenerate the title table after updating
`db/PSX.titles.json`:

```sh
python3 tools/gen_titles.py db/PSX.titles.json source/titles.inc
python3 tools/gen_libcrypt.py db/libcrypt_serials.txt source/libcrypt.inc
python3 tools/make_icon.py pkgfiles/ICON0.PNG
python3 tools/gen_font.py fonts source/font.inc
```

## Credits

- [IRISMAN](https://github.com/aldostools/IRISMAN): lv1 storage patch, BD reset/auth and
  the raw read method
- [sacd-ripper](https://github.com/sacd-ripper/sacd-ripper): lv2 storage/ATAPI wrappers
- [webMAN MOD](https://github.com/aldostools/webMAN-MOD) and Evilnat's Cobra: ATAPI
  structures, PS3MAPI calls and the LibCrypt `.sbi` handling this app writes for
- [GameDB-PSX](https://github.com/niemasd/GameDB-PSX) (GPL-3.0): serial → title database
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) and bucanero's prebuilt toolchain
- [DejaVu fonts](https://dejavu-fonts.github.io/) (see `fonts/DejaVu-LICENSE`) for the on-screen UI
- [zlib](https://zlib.net/) (zlib license, from the PSL1GHT portlibs) to unpack the Redump dat
- [Redump](http://redump.org/): disc data used for verification, downloaded at runtime only

## License

GPL-3.0, see [LICENSE](LICENSE). Only rip discs you own.
