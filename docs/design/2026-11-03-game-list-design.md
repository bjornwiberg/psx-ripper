# Game list — design

Status: decided with the user, 2026-11-03. Built after the audit log
(`docs/design/2026-11-03-mcp-design.md`, phase 0), before MCP.

## Intent

A screen in the app that lists **every game image on a source** (the hard disk or a USB stick), not
only the rips PSX RiPPER made, with its format and status: free of errors or not, verified against
Redump or not. When the user has time, they pick the games worth another try — put the disc back
in and let the app repair it, or check a file against Redump without the disc at all.

Two ways to use it, both real: at the TV with the pad, and through an agent (MCP, later) while the
TV is off and the console rips. Until MCP exists, the list is also in `status.json`.

## Goals

1. All game images in the `PSXISO` folder of the chosen source, in **alphabetical order**.
2. Per game: **format** (2352 raw or 2048 data-only, and for the app's rips the number of tracks),
   **status** (free of errors, with errors (how many), verified, does not match, not checked,
   re-rip needed) and **where** it is.
3. **Check against Redump from the file (□)**, without the disc, for the app's rips and for 2352
   images made by other tools; the result is written to a log next to the file, so the game shows
   its status like any other afterwards. **Check all** checks every unchecked game in one go. With
   Redump off, the list says that Redump must be turned on (online or a local copy) to check.
4. 2048 images (data only) cannot be compared with Redump: they are marked **"Re-rip to verify"**.
5. Everything the list changes goes through the audit log (source: pad, remote view, the app,
   later MCP) and can be undone there.

## Choosing the source

- One source at a time: **Hard disk**, **USB 0 (NAME)**, **USB 1 (NAME)** …, where NAME is the
  stick's volume label when the app can read it (exFAT sticks: the app reads them itself; FAT32
  sticks are read through the PS3's system — show the label if it can be found, else just "USB 0").
  "Internal" is not used for the hard disk: it already names the PS3's drive.
- **All**: every source at once; each row then shows its source.
- The choice is a button on the screen and is remembered.

## Filters

Changed with a button (and an argument over MCP):
- **All**
- **With errors** (unreadable or bad sectors left: worth a repair)
- **Not verified** (never checked, or checked without a match)
- **Re-rip** (2048 images)
- **Verified** (match Redump)
- **Trash** (see below)

## Version 1 contents (all decided)

1. **Summary at the top**: e.g. "142 games · 98 verified · 6 with errors · 12 re-rip".
2. **Detail view per game (X)**: tracks and their MD5, the list of errors, the Redump name, when and
   on which drive it was ripped, file size, place, whether a cover picture exists.
3. **Multi-disc games grouped**: e.g. Final Fantasy IX is one row "4 discs"; its status is the worst
   of its discs; the detail view lists each disc.
4. **Duplicates**: the same game on two sources, or as both `.iso` and `.bin` (seen: Castlevania
   Chronicles); shown as such, and either copy can be moved to the trash.
5. **Broken sets**: a `.bin` without `.cue`, a `.cue` pointing to a missing file, a leftover
   `.part` from a cancelled rip.
6. **LibCrypt mark for PAL games**: whether the protection data (`.sbi`) is there (some PAL games
   do not run in emulators without it).
7. **Jump by letter** (e.g. L2/R2 to the next letter) for long lists. No on-screen-keyboard search;
   searching is for MCP.
8. **Rename to a pattern**: reuses the file name screen (pattern at the top). From the game list its
   preview at the bottom shows **the selected image** instead of the discs in the drives, so the
   user sees what the file would be called (e.g. `%N`, Redump's name, or `%T [%D]`) and confirms.
   The rename covers the whole set and the file name inside the `.cue`, and goes through the audit
   log (undoable).
9. **When the game was last checked.**

## Trash (moved here from the MCP design's phase 5)

- **Delete** a game: its whole set (`.bin`, `.cue`, `.log`, `.md5`, cover picture) moves to
  `.trash` in the same source. Logged in the audit log, undoable there.
- The trash is a filter of the list: restore a game, or **empty the trash** (yes/no dialog; final;
  logged).

## Not in version 1

- On-screen-keyboard search.

## Constraints

As for the app: own UI only, plain words, right increases, X opens, O goes back, L2/R2 page with
arrows at the right edge; every text in English and Swedish; host UI test scenarios for every new
screen; small package; no blocking of rips while the list is built (reading folders and logs must
not stall the drives).

## Left to the builder

The exact buttons for source, filter and the summary's wording; how the list is built and cached
(folder scan plus each set's log; MD5 only when checking); where the per-game check log for
images from other tools lives (next to the file, same name, `.log`, in the app's rip-log format);
how duplicates are matched (disc ID and size, or Redump name); the screens' layout following the
existing settings and webhook lists.
