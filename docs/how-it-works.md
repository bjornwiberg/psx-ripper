# How it works

On 4.2x+ firmwares a homebrew process is not allowed to read raw sectors from the BD drive
until the drive has been re-authenticated, and the lv1 storage manager refuses that by
default. PSX RiPPER does the same as IRISMAN's PS1 copy:

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

## Damaged discs

Every data sector is checked (sync, address, EDC) while reading; the drive can return a
damaged sector as a successful read. Bad sectors are re-read. The first pass keeps moving
through scratches (the drive's own retries are set low, a damaged area gets one try per
sector); afterwards a second pass retries the failed sectors at 2x with the drive's normal
retries and patches them into the image. O during the second pass keeps the image as it is.

## USB SuperDrive

An Apple USB SuperDrive (05AC:1500) can be used instead of the PS3's drive: *Settings → Drive*.
The PS3 system lists USB optical drives but passes no commands to them, so the app has its own
USB driver (`source/usbodd.cpp`, USB Mass Storage Bulk-Only Transport). It sends Apple's unlock
command so the slot takes a disc, waits for the drive to spin up (up to a minute on the PS3's bus
power, no Y-cable needed), reads with the same READ CD and sector checks as the internal drive,
and ejects with START STOP UNIT (the drive has no button). Its Q subchannel trails by one sector;
the ripper's Q calibration accounts for that.

## Status

| Feature | Status |
|---|---|
| Raw rip, CUE, title lookup | Tested (Evilnat 4.93, ~2.7 MB/s). MD5 identical to Redump for Final Fantasy VIII (USA) disc 1 and Final Fantasy IX (Europe) disc 1 |
| Rip boots in webMAN/Cobra | Tested |
| LibCrypt detection on protected PAL discs | Tested on an original disc: the `.sbi` for Final Fantasy IX (Europe) disc 1 matches the disc's known LibCrypt sectors byte for byte |
| Multi-track discs (CD audio) | Tested: single-file `.cue` with pregaps from the Q subchannel, tracks split as Redump splits them |
| Several drives in parallel (internal + two Apple USB SuperDrives) | Tested: bit-perfect against Redump on all three at once (Alundra, Final Fantasy IX discs 1–3) |

LibCrypt protection data lives in the disc's subchannel. A disc that is not an original
(for example a CD-R burned from an image) usually lacks it. The ripper then reports that no
protection data was found, and the game may not start. The `.sbi` is only ever created from
the disc in your drive; PSX RiPPER does not ship or download `.sbi` files.

Verify your rips against [Redump](http://redump.org) (webMAN can compute MD5 with
`/md5.ps3<path>`).

## UI tests

The screens can be checked without a PS3: `make -C tests/host` builds the app's own `main.cpp`
for the host against stubs (`tests/host/include`, `tests/host/stubs.cpp`), draws scenarios
(several drives, rips with many events, long names, one drive, English) and writes each frame
to `tests/host/out/*.png`. Every frame is checked for texts that overlap, text off the screen or
outside the content area, and characters the font does not have. The run fails when a check
fails; a self-test makes sure the checks find planted problems.
