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
- Settings (No on the start screen → Settings): destination (internal HDD or a connected USB
  drive), re-reads on read errors, read speed (Max/8×/4×/2×) and eject on/off, saved in
  `/dev_hdd0/tmp/psxrip.cfg`
- English and Swedish UI, picked from the PS3 system language
- Works fully offline once installed

## Requirements

- PS3 with CFW and Cobra (developed and tested on **Evilnat 4.93**)
- PS3MAPI available (it is on Evilnat/Cobra); used for lv1 peek/poke
- webMAN MOD is optional (unmount and game-list refresh)

## Usage

1. Install `psxripper.pkg` (Package Manager → Install Package Files, or webMAN *Install PKG*).
2. Start **PSX Ripper** from the Game column.
3. Insert a PS1 disc, or select *Yes* if it is already in the drive.
4. Confirm the detected title and serial, and wait. A full CD takes about 3.5–4 minutes.
5. The disc is ejected when done. Insert the next disc to rip it automatically, or press O to
   go back; select *No* on the start screen to exit and refresh webMAN's game list.

A log is written to `/dev_hdd0/tmp/psxrip.log`.

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
```

## Credits

- [IRISMAN](https://github.com/aldostools/IRISMAN): lv1 storage patch, BD reset/auth and
  the raw read method
- [sacd-ripper](https://github.com/sacd-ripper/sacd-ripper): lv2 storage/ATAPI wrappers
- [webMAN MOD](https://github.com/aldostools/webMAN-MOD) and Evilnat's Cobra: ATAPI
  structures, PS3MAPI calls and the LibCrypt `.sbi` handling this app writes for
- [GameDB-PSX](https://github.com/niemasd/GameDB-PSX) (GPL-3.0): serial → title database
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) and bucanero's prebuilt toolchain

## License

GPL-3.0, see [LICENSE](LICENSE). Only rip discs you own.
