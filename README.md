# PSX Ripper

[![CI](../../actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

A PS3 homebrew app that rips your own PlayStation 1 discs to bit-perfect `.bin`/`.cue` images,
ready to play with webMAN MOD / Cobra. It reads from the PS3's own Blu-ray drive and from up to
two Apple USB SuperDrives, all three at the same time, and checks every rip against
[Redump](http://redump.org/).

> PSX Ripper is not affiliated with or endorsed by Sony Interactive Entertainment. "PlayStation"
> is a trademark of Sony Interactive Entertainment. Only rip discs you own.

## Features

- **Bit-perfect rips**: raw 2352-byte sectors, all tracks including CD audio, with pregaps read
  from the Q subchannel. Every data sector is checked (sync, header, EDC/ECC); damaged sectors
  are re-read slowly, and a verify pass confirms uncertain sectors with identical reads.
- **Several drives in parallel**: the internal drive and up to two Apple USB SuperDrives, each
  on its own tab with its own progress, destination and settings.
- **Redump verification**: per-track MD5s compared with Redump, online or from a local copy of
  the dat. Discs are named by Redump, and icons on the tabs show which rips match.
- **Repair**: the rip log records every sector that could not be read. *Repair* re-reads only
  those sectors, so a disc can be cleaned and tried again later without a full re-rip. With
  automatic start on, inserting such a disc repairs it by itself.
- **LibCrypt**: `.sbi` files are created from the disc's subchannel for the PAL discs known to
  use LibCrypt; Cobra reads them when the image is mounted. No `.sbi` files are shipped.
- **Destinations**: the internal hard disk or USB drives (FAT32 and exFAT), a file name pattern
  (`%T` title, `%R` region, `%D` disc ID, `%N` Redump name) and detection of discs already
  ripped there.
- **Automatic mode**: rip (or repair) when a disc is inserted, eject when done.
- **Remote view**: the screen as a web page on port 8090 with a pad whose buttons press the
  console's, `/status.json`, and **webhooks** (for example to Home Assistant) for every event.
- Its own on-screen UI in English and Swedish, picked from the system language. Works fully
  offline once installed.

## Requirements

- A PS3 with custom firmware and Cobra (developed and tested on **Evilnat 4.93**)
- PS3MAPI (part of Evilnat/Cobra), used for the lv1 patch while ripping
- Optional: webMAN MOD (unmounting images, refreshing the game list) and Apple USB SuperDrives
  (USB ID 05AC:1500)

## Installation

1. Download `psxripper-<version>.pkg` from the [latest release](../../releases/latest).
2. Copy it to the PS3 (USB stick, FTP, or webMAN *Install PKG*) and install it with
   *Package Manager → Install Package Files*.
3. Start **PSX Ripper** from the Game column.

## Usage

Insert a disc. The tab of its drive names the disc (Redump's name when known), its serial and
size, and whether it is already ripped to the destination.

| Button | On a drive's tab |
|---|---|
| ✕ | Rip (also over an existing rip) |
| □ | Repair the sectors the rip log lists, or check an earlier rip against Redump |
| △ | Eject |
| ← → | Change the destination |
| L1 / R1 | Previous / next tab |
| L2 / R2 | Scroll the page when it is taller than the screen |
| SELECT | This drive's own settings |
| START | Settings |
| ○ | Cancel the rip on this tab, or exit when nothing is ripping |

A full CD takes about four minutes on the internal drive. Each rip writes `<name>.bin`, `.cue`,
`.md5` (MD5 per track) and `.log` (the rip log, which *Repair* uses). The app's own log is
`/dev_hdd0/tmp/psxrip.log`.

More: [configuration, remote view and webhooks](docs/configuration.md) ·
[how it works](docs/how-it-works.md) · [parallel ripping](docs/PARALLEL.md)

## Building

The build runs in Docker with a prebuilt PSL1GHT toolchain:

```sh
docker build --platform linux/amd64 -t ps3dev-psl1ght docker/
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src ps3dev-psl1ght make pkg
```

This produces `psxripper.pkg`. The UI tests need only a C++ compiler:

```sh
make -C tests/host     # draws every scenario to tests/host/out/*.png and checks the layout
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow, the tests and how releases
are made.

## Credits

- [IRISMAN](https://github.com/aldostools/IRISMAN): lv1 storage patch, BD reset/auth and the raw
  read method
- [sacd-ripper](https://github.com/sacd-ripper/sacd-ripper): lv2 storage/ATAPI wrappers
- [webMAN MOD](https://github.com/aldostools/webMAN-MOD) and Evilnat's Cobra: ATAPI structures,
  PS3MAPI calls and the LibCrypt `.sbi` handling this app writes for
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) and bucanero's prebuilt toolchain
- Third-party code and data in this repository: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## License

GPL-3.0-only, see [LICENSE](LICENSE).
