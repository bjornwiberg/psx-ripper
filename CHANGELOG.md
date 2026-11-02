# Changelog

All notable changes are listed here. The format follows the release notes that
[release-please](https://github.com/googleapis/release-please) writes from
[Conventional Commits](https://www.conventionalcommits.org/), and versions follow
[Semantic Versioning](https://semver.org/).

## 1.0.0 (2026-11-02)

First public release.

### Features

* Bit-perfect rips of PS1 discs (all tracks, pregaps from the Q subchannel) from the internal
  Blu-ray drive and up to two Apple USB SuperDrives, in parallel, one tab per drive
* Sector checks (EDC/ECC), slow re-reads, a verify pass, and a rip log listing unreadable sectors
* Repair from the rip log, also automatically when an already ripped disc with known errors is inserted
* Redump verification (online or local copy), Redump names, and status icons on the tabs
* LibCrypt `.sbi` creation for the known PAL LibCrypt discs
* Destinations on the hard disk and USB drives (FAT32, exFAT), file name patterns, detection of existing rips
* Remote view with a pad, `/status.json`, and webhooks
* English and Swedish UI
