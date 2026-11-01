# Ripping from several drives at once (plan)

Goal: rip up to three discs at the same time, from the PS3's own drive and up to two Apple USB
SuperDrives. Each drive has its own destination; with two SuperDrives connected the USB ports are
taken, so everything goes to the internal hard disk.

## Where we are (2026-11-01)

- `usbodd.cpp` drives one SuperDrive (05AC:1500) over Bulk-Only Transport: unlock, TEST UNIT
  READY with spin-up wait, TOC, READ CD with formatted Q, eject. The setting *Läsare* chooses the
  PS3 drive or the SuperDrive; one drive at a time.
- All rip state lives in globals in `main.cpp` (about 100: TOC, buffers, Q calibration, LibCrypt
  candidates, hard/suspect sector lists, the writer thread, progress, the result lines).

## Steps

1. **usbodd, several devices.** Replace the single set of `g_dev/g_in/g_out/g_done` with an array
   of `UsbDrive` (pipes, endpoints, tag, transfer memory, completion flags, own mutex). The done
   callback gets the drive as its `arg`. Probe/attach fills the first free slot; detach clears it.
   API takes a drive index: `usbodd_cmd(int d, ...)`, `usbodd_count()`, `usbodd_port(d)`.
2. **Drive abstraction.** `struct Drive { int kind; int usb; ... }` with `drive_cmd()`,
   `drive_read_cd()`, `drive_eject()`, `drive_ready()`. `atapi()`, `atapi_out()` and
   `read_cd_sub()` take a `Drive *` instead of looking at `g_src`. lv1 patch and BD auth only for
   the PS3 drive.
3. **Rip context.** Move the per-disc globals into `struct Rip` (one per drive): TOC, buffers,
   Q state, LibCrypt, hard/suspect lists, `g_base`, counters, progress, result, the writer thread
   and its queue, the rip log. `rip_thread`, `verify_pass`, `second_pass`, `confirm_read`,
   `check_subq`, `write_sbi`, `write_riplog`, repair take `Rip *`. Shared, read-only: EDC/ECC
   tables, the title database, the LibCrypt list, settings.
   Tests: a rip from the PS3 drive must give the same image and log as before (Alundra, NHL
   MD5 against Redump) before anything runs in parallel.
4. **Drive settings.** Settings view *Läsare* (✕ opens it, like *Webhooks*): one row per known
   drive (PS3-läsaren, SuperDrive 1, SuperDrive 2 by USB port) with on/off and *Spara till*
   (←→ cycles the destinations). A destination on a USB port used by a SuperDrive is not offered.
   cfg: `drive_bd=1`, `drive_usb1=1`, `outdir_bd=`, `outdir_usb1=` ... (old `outdir=` / `source=`
   are read as the defaults).
5. **Scheduler.** One identify/rip thread per active drive. Shared resources:
   - the hard disk: each rip has its own writer; writes are 1 MB blocks so they interleave well.
     Free-space check counts the discs already being ripped to the same volume.
   - the USB bus: both SuperDrives share the PS3's USB 2.0 controller (~25-30 MB/s); a CD at
     24x is ~3.5 MB/s, so there is room.
   - webhooks: events get a `drive` field (`bd`, `usb1`, `usb2`).
   - the same disc in two drives at once (same serial and size): the second waits or is refused.
6. **Compact UI** (below), status.json with a `drives` array (the old top-level fields keep
   describing the first busy drive so the HA automation keeps working).

## Compact UI when more than one drive is in use

With one active drive the screens stay as they are now. With two or three the home screen and the
progress screen become one screen with a card per drive:

```
 PSX Ripper                                             Hårddisk (/PSXISO, 812 GB ledigt)

 ┌ PS3-läsaren ───────────────────────────────────────────────────────────────────────┐
 │ Alundra · SLES_011.35                                                   Rippar 42 % │
 │ ███████████████████░░░░░░░░░░░░░░░░░░░░  2× · 4:10 kvar · lagade 1                  │
 ├ SuperDrive 1 (USB vänster) ────────────────────────────────────────────────────────┤
 │ NHL Rock the Rink · SLES_027.94                                         Rippar 17 % │
 │ ███████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  8× · 9:30 kvar                             │
 ├ SuperDrive 2 (USB höger) ──────────────────────────────────────────────────────────┤
 │ Croc 2 · SLES_029.20 · redan rippad, utan kända fel                           Klar  │
 └────────────────────────────────────────────────────────────────────────────────────┘

 ↑↓ Välj läsare   ✕ Rippa   △ Mata ut   □ Laga   ○ Avbryt / Tillbaka   START Inställningar
```

- ↑↓ selects a card; the buttons act on the selected drive (same meaning as today's home screen).
- A card is two lines: title · serial · state, and a progress bar with speed, time left and
  fixed/unreadable counts. Events (re-read sectors) are not shown per card; the selected card's
  last event can go in the footer.
- A finished rip shows its result in the card (green / yellow / red), ✕ on it opens today's full
  result screen.
- Auto-rip (setting *Rippa direkt*) works per drive: a disc inserted in any drive starts at once.
