# Changelog

All notable changes are listed here. The format follows the release notes that
[release-please](https://github.com/googleapis/release-please) writes from
[Conventional Commits](https://www.conventionalcommits.org/), and versions follow
[Semantic Versioning](https://semver.org/).

## [1.2.0](https://github.com/bjornwiberg/psx-ripper/compare/v1.1.0...v1.2.0) (2026-11-04)


### Features

* audit log of every change, with undo ([#25](https://github.com/bjornwiberg/psx-ripper/issues/25)) ([e8579af](https://github.com/bjornwiberg/psx-ripper/commit/e8579afbb893e10360d152c00cfae8a6bc4e78ce))


### Bug fixes

* no rip starts for a disc already in a USB drive at app start ([#27](https://github.com/bjornwiberg/psx-ripper/issues/27)) ([127eefd](https://github.com/bjornwiberg/psx-ripper/commit/127eefd9930bf3c5b274ae646c9ccb81a2cce21b))
* progress under 10 % with a decimal, and time left for the slow re-read pass ([#20](https://github.com/bjornwiberg/psx-ripper/issues/20)) ([eb43e22](https://github.com/bjornwiberg/psx-ripper/commit/eb43e221a2939233b9690c5e94fe548dbcc3f11d))
* take the disc ID from the volume name when a disc has no SYSTEM.CNF ([#26](https://github.com/bjornwiberg/psx-ripper/issues/26)) ([bd00f4c](https://github.com/bjornwiberg/psx-ripper/commit/bd00f4ca722d6f2ce184c7b00a11efb724a24025))


### Documentation

* design of the game list; trash and renaming move there from the MCP design ([#23](https://github.com/bjornwiberg/psx-ripper/issues/23)) ([6508aa3](https://github.com/bjornwiberg/psx-ripper/commit/6508aa31124d700e0c9d716d62a5593f0695e078))
* design of the MCP server ([#22](https://github.com/bjornwiberg/psx-ripper/issues/22)) ([97eb86d](https://github.com/bjornwiberg/psx-ripper/commit/97eb86dcfcb010a5c6091fb1d8911dcaa5263678))
* MCP design – screen state and answering without touching the screen ([#24](https://github.com/bjornwiberg/psx-ripper/issues/24)) ([1aa1e7d](https://github.com/bjornwiberg/psx-ripper/commit/1aa1e7d818c94a1f2088836909110d56b66917d3))

## [1.1.0](https://github.com/bjornwiberg/psx-ripper/compare/v1.0.2...v1.1.0) (2026-11-03)


### Features

* local builds show their build time; the version sits under the title ([#8](https://github.com/bjornwiberg/psx-ripper/issues/8)) ([acac095](https://github.com/bjornwiberg/psx-ripper/commit/acac095325fdf53de95bd3044dc970a87fd4f30f))
* webhook event disc_lost when a rip or repair stops because the disc left the drive ([#19](https://github.com/bjornwiberg/psx-ripper/issues/19)) ([0cccd5e](https://github.com/bjornwiberg/psx-ripper/commit/0cccd5ec7c7e092825872dd0c214423c3d9f6a14))


### Bug fixes

* a drive's page shows the last rip only for that disc, not for the next one put in ([#14](https://github.com/bjornwiberg/psx-ripper/issues/14)) ([d6ca98f](https://github.com/bjornwiberg/psx-ripper/commit/d6ca98f5471f76031ab03f3f3196edd196042dd0))
* a job on a SuperDrive stops when the disc is gone instead of re-reading every sector ([#17](https://github.com/bjornwiberg/psx-ripper/issues/17)) ([3d1ba51](https://github.com/bjornwiberg/psx-ripper/commit/3d1ba510f5ae7091e973d04221a56f65cb3849bb))
* a SuperDrive reads the pregap before the first audio track instead of re-reading it for an hour ([#11](https://github.com/bjornwiberg/psx-ripper/issues/11)) ([e0fd317](https://github.com/bjornwiberg/psx-ripper/commit/e0fd31787785958ef97600391193227934887691))
* Castlevania Chronicles (USA) is named right despite the Tennis ID on its disc ([#15](https://github.com/bjornwiberg/psx-ripper/issues/15)) ([728b3d4](https://github.com/bjornwiberg/psx-ripper/commit/728b3d487cf0ba4b1df07e789b7ea42def37e991))
* find the disc ID when SYSTEM.CNF writes it in lower case ([#10](https://github.com/bjornwiberg/psx-ripper/issues/10)) ([6afdbdb](https://github.com/bjornwiberg/psx-ripper/commit/6afdbdb5a37622ca5c527a29b4cdefb844446697))
* putting a disc in repairs an older rip's junk pregap sector ([#16](https://github.com/bjornwiberg/psx-ripper/issues/16)) ([44721f8](https://github.com/bjornwiberg/psx-ripper/commit/44721f8f699638769aed361a6195166152a2c24c))
* the first pregap sector before the first audio track is stored as silence, not as the drive's junk ([#13](https://github.com/bjornwiberg/psx-ripper/issues/13)) ([65376d5](https://github.com/bjornwiberg/psx-ripper/commit/65376d5d66e757f61ff76b4ad7ed07d103befd3e))
* time and time left count for every drive, not only from when its tab is opened ([#9](https://github.com/bjornwiberg/psx-ripper/issues/9)) ([33fb201](https://github.com/bjornwiberg/psx-ripper/commit/33fb201b3747768e765fb90ecdfe4ac9650b2c91))

## [1.0.2](https://github.com/bjornwiberg/psx-ripper/compare/v1.0.1...v1.0.2) (2026-11-03)


### Bug fixes

* remote view picture comes back after the phone has been away ([#5](https://github.com/bjornwiberg/psx-ripper/issues/5)) ([7182ac5](https://github.com/bjornwiberg/psx-ripper/commit/7182ac51da18b2c602ce24724fddbc96e51d1702))

## [1.0.1](https://github.com/bjornwiberg/psx-ripper/compare/v1.0.0...v1.0.1) (2026-11-02)


### Bug fixes

* the remote view's page follows the console's language ([#1](https://github.com/bjornwiberg/psx-ripper/issues/1)) ([32ad843](https://github.com/bjornwiberg/psx-ripper/commit/32ad84379e9d8a37252d8a1df02ac70fde80805c))

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
