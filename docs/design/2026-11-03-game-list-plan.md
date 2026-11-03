# Game list — implementation plan

Steps use checkboxes (`- [ ]`). Branch `feat/game-list` (on top of `feat/audit-log`; the pull
request targets `main` once the audit log is merged), one commit per task, one pull request
(`feat: game list of every image on a source, with checks, rename and trash`).

**Goal:** a screen listing every game image on a chosen source with format and status, filters, a
summary, a detail view, multi-disc grouping, duplicates, broken sets, LibCrypt marks, jump by
letter, a check against Redump from the file (one or all), rename with the file name screen, and a
trash — every change through the audit log.

**Architecture:** a new module `source/library.cpp` / `include/library.h` scans a source's
`PSXISO` folder into game sets and their status (pure logic over an injected directory/file
interface, so the host tests run it on real folders). `main.cpp` gets the screen, the jobs (check
one / check all) and the file actions; file actions go through new reversible `move` rows in the
audit module. `/games.json` on the remote view's server carries the list until MCP exists.

**Tech stack:** C++ (gnu++14), PSL1GHT, the app's own UI, host tests in `tests/host`.

**Spec:** `docs/design/2026-11-03-game-list-design.md`.

## Global constraints

- Every UI text in English and Swedish, new `StrId`s before `STR_COUNT`, same order in both tables.
  Code, comments and log lines in English.
- No AI attribution anywhere. Conventional Commits.
- `make -C tests/host` before every commit that touches the UI; a scenario per new screen; look at
  the frames. PS3 build without warnings. The package stays well under 1 MB.
- Own UI only, plain words, right increases, X opens, O goes back, L2/R2 page with arrows at the
  right edge, details in the small font, no overlapping text.
- Building the list must never stall a rip: scanning folders and reading logs runs in a
  background thread at low priority; the screen shows "Reading …" until it is ready.
- Never delete a file except when the trash is emptied (after a yes/no dialog, logged).

## Decisions this plan fixes

- **Where the list is opened:** a new first row in the settings, "Games" / "Spel" (X opens). The
  home screen is unchanged.
- **Sources:** "Hard disk" (`/dev_hdd0/PSXISO`), "USB n (LABEL)" for each `/dev_usbNNN/PSXISO`
  and each exFAT volume (`exfatN:/PSXISO`) — the same candidates as `next_target` — and "All".
  LABEL: exFAT via FatFs `f_getlabel`; FAT32 sticks the PS3 mounts: read the label from the
  volume's boot sector/root directory if reachable, otherwise no label. The chosen source is a
  config key `games_source` (`hdd`, `usbN`, `all`), remembered.
- **A game set:** files in `PSXISO` sharing a base name: `.bin`/`.cue`/`.log`/`.md5`/`.jpg`
  (cover), plus single images `.iso`/`.img` (other tools). A leftover `<base>.bin.part` is a
  broken set. The cue's `FILE` lines decide which `.bin` belongs to which cue.
- **Format:** 2352 when the image size is a multiple of 2352 (and the cue says MODE2/2352 or
  AUDIO), 2048 when it is a multiple of 2048 and not of 2352 (or the cue says MODE1/2048 /
  MODE2/2048); tracks from the cue (1 without a cue).
- **Status** (one of, in this order of precedence): `broken` (missing cue/bin, cue names a missing
  file, `.part` left), `rerip` (2048), `errors` (log: unreadable or bad sectors listed; shows the
  count), `verified` (log `redump=verified`), `nomatch` (`redump=no_match` / `not_found`),
  `unchecked` (no log or no redump line). Shown with plain words, not only icons.
- **Filters:** All, With errors (`errors`), Not verified (`unchecked`, `nomatch`), Re-rip
  (`rerip`), Verified, Broken (decided by the builder to include broken sets in "All" and in their
  own filter), Trash. Config key `games_filter`.
- **Grouping:** sets whose title (log `title=` or name without `[ID]` and region) differs only by
  "(Disc N)" are one row "N discs", status = the worst of its discs, the detail view lists each.
- **Duplicates:** two sets with the same disc ID and the same image size, or the same Redump name,
  anywhere in the scanned sources: both rows get "Duplicate" and the detail view names the other.
- **LibCrypt:** for PAL IDs on the app's LibCrypt list (`is_libcrypt_disc`): "LibCrypt: .sbi
  present" when `/dev_hdd0/tmp/sbi/<ID>.sbi` or `<base>.sbi` exists, else "LibCrypt: .sbi missing".
- **Detail view (X):** tracks with their MD5, the errors list (count and the first few sectors),
  Redump name, date, drive (log), size, place, cover yes/no, last checked (log date of the redump
  line, or the log's date), duplicate / LibCrypt notes.
- **Buttons on the list:** X detail; □ check against Redump (this game); L1/R1 previous/next filter;
  SELECT next source; L2/R2 previous/next first letter (jump); △ move to trash (in the Trash
  filter: restore); START "Check all" (all unchecked shown games) — in the Trash filter START
  "Empty the trash". In the detail view: □ check, △ trash, X "Rename" (opens the file name screen).
- **Check from the file:** the app's rips use `check_image`; a 2352 `.iso`/`.img` without cue is
  one MODE2/2352 track: its MD5 is compared with Redump the same way, and a log next to it
  (`<base>.log`, the app's rip-log format with `title=`, `serial=` if known from the image,
  `track=01`, `redump=`) records the result. 2048 images are never checked (status `rerip`).
  Redump off → the check is not offered and the screen says to turn it on (online or local copy).
  "Check all" is one background job with progress and O to cancel; each game's check is an audit
  `check` job row with its Redump before/after.
- **Rename:** the file name screen opened from the detail view shows the selected game as its
  preview (instead of the discs in the drives) and, on confirm, renames the whole set and the
  `FILE` lines in its `.cue` (and the `title=` line stays). Refused when a set with the new name
  exists.
- **Trash:** `<source>/PSXISO/.trash/` (created on first use). Moving and restoring move the whole
  set. Emptying deletes everything in it after a yes/no dialog.
- **Audit:** a new reversible row kind `move` (domain `files`, `what` = the set's display name,
  `before` = old path of each file, `after` = new path; one row per file, all in one audit job
  `rename` / `trash` / `restore`) — undo moves the file back if the old path is free and the new one
  exists; refused otherwise (new reason codes with en/sv texts). Emptying the trash is one job
  `empty` with a `record` row per deleted file (not reversible). Rename/trash/restore/empty appear
  under the "Files" domain of the audit screen, grouped by disc where the set has a disc ID.
- **`/games.json`** (when status.json is served): the chosen source's list (name, ID, format, tracks,
  status, error count, Redump name, place, flags), the summary, and the sources with labels.

## Review focus

1. A source removed while the list is open (USB stick pulled): the list says the source is gone;
   no crash; actions on it refused. Test with a scan over a folder deleted mid-way.
2. A rename or trash move where one file of the set cannot be moved: everything already moved goes
   back, and the user sees why. Test with a blocked destination.
3. Big folders (500 sets): the scan does not block the UI and the screen stays responsive; the
   list is built once and cached until a change or a source switch. Test the scan on 500 dummy sets
   (time and memory bounded).
4. Names with brackets, dots and non-ASCII (é, ü, Japanese titles in logs): grouping, sorting
   (alphabetical, case-insensitive, ASCII letters first, then the rest) and the cue rewrite keep
   them intact. Test cases with such names.
5. Undo of a trash move after the trash was emptied: refused with a clear reason, nothing created.

---

### Task 1: library scan and status (module)

**Files:** create `include/library.h`, `source/library.cpp`, `tests/host/library_test.cpp`;
modify `tests/host/Makefile` (build and run `library_test`; link `library.cpp` into ui_test and
remote_sim too).

**Interfaces (produces):**
```c
enum GameFmt { GF_UNKNOWN, GF_2352, GF_2048 };
enum GameStatus { GS_BROKEN, GS_RERIP, GS_ERRORS, GS_UNCHECKED, GS_NOMATCH, GS_VERIFIED };
struct GameDisc { char base[200], image[220], id[16], title[128], rdname[160], date[24], drive[32];
                  uint64_t size; GameFmt fmt; int tracks, errors, bad; GameStatus st; int has_cover,
                  has_log, libcrypt /*0 n/a, 1 sbi present, -1 missing*/, dup /*index of the other, -1*/; };
struct Game { char name[128]; int first, ndiscs; GameStatus st; };   // a row: one disc or a group
struct LibIo { int (*list)(const char *dir, void (*cb)(const char *, void *), void *);
               int (*stat_size)(const char *path, uint64_t *size);
               int (*read)(const char *path, char *buf, int max);   // whole small text files
               int (*exists)(const char *path); };
int lib_scan(const char *dir, const LibIo *io, GameDisc *discs, int max);       // sets of one folder
int lib_group(GameDisc *discs, int n, Game *games, int max);                    // sorted rows
void lib_mark_dups(GameDisc *discs, int n);
int lib_filter(const Game *g, int n, const GameDisc *d, int filter, int *out);  // indices shown
void lib_summary(const GameDisc *d, int n, int *total, int *verified, int *errors, int *rerip, int *broken);
int lib_first_letter_next(const Game *g, const int *shown, int n, int from, int dir);
```

- [ ] **Step 1: tests** (real temp folders): sets grouped by base name; format 2352/2048 from size
  and cue; status precedence; the app's log parsed (errors, bad, redump, title, date, drive);
  `.part` → broken; cue naming a missing file → broken; multi-disc grouping and worst status;
  duplicates (same ID+size, same Redump name); LibCrypt present/missing; sorting
  (case-insensitive, non-ASCII after ASCII); jump-by-letter; summary counts; 500 sets in bounded
  time.
- [ ] **Step 2:** run, fail. **Step 3:** implement. **Step 4:** pass; PS3 build clean.
- [ ] **Step 5:** commit `feat: game list scanning and status`.

### Task 2: sources and labels, the background scan

**Files:** `source/main.cpp` (source list, labels, `games_source`/`games_filter` config keys in
`cfg_apply_line`/`cfg_text`, a background scan thread at low priority with a cached result and a
"changed" counter), `source/vfs.cpp` (exFAT label), `tests/host/ui_test.cpp`.

- [ ] Tests: the source list from the same candidates as `next_target` (host stubs), labels
  shown when available, "All" merges sources and keeps the place per disc, config keys
  round-trip, a source that disappears gives "gone".
- [ ] Implement, pass, PS3 build clean, commit `feat: game list sources with their labels`.

### Task 3: the game list screen and detail view

**Files:** `source/main.cpp` (`games_screen()`, the settings row "Games"/"Spel" first,
renumbering the rows that follow), `include/lang.h`, `source/lang.cpp`, `tests/host/ui_test.cpp`.

- [ ] Scenarios (sv and en): reading state, the list with summary and statuses, each filter,
  multi-disc row, duplicate, broken set, LibCrypt marks, jump by letter, the detail view of an
  app rip and of a 2048 `.iso`, an empty source, a gone source, the settings with the new row.
- [ ] Implement, pass, look at the frames, PS3 build clean, commit `feat: game list screen`.

### Task 4: check from the file (one and all)

**Files:** `source/main.cpp` (a check job for a game set incl. `.iso`/`.img` without cue, its log,
"Check all" job with progress and cancel, audit `check` jobs), `tests/host/ui_test.cpp`.

- [ ] Tests: a 2352 `.iso` without cue gets a log with track MD5 and a redump line (local Redump
  stub); a 2048 image is refused; "Check all" checks only unchecked shown games and can be
  cancelled; each check writes audit rows; Redump off → not offered.
- [ ] Implement, pass, PS3 build clean, commit `feat: check games against Redump from their files`.

### Task 5: file actions through the audit log (rename, trash, restore, empty)

**Files:** `include/audit.h`, `source/audit.cpp` (row kind `move`, undo by moving back, reasons),
`source/main.cpp` (rename via the file name screen with the selected game as preview incl. the
`.cue` FILE lines; trash/restore/empty; the audit screen shows the Files domain), `include/lang.h`,
`source/lang.cpp`, `tests/host/audit_test.cpp`, `tests/host/ui_test.cpp`.

- [ ] Tests: move rows and their undo/redo (refused when the old path is taken or the file is gone);
  rename of a set incl. cue lines and its undo; a partial failure rolls back what moved; trash and
  restore and their undo; emptying is final and logged; undo after emptying refused; the file name
  screen preview shows the selected game.
- [ ] Implement, pass, look at the frames, PS3 build clean, commit `feat: rename and trash games, undoable in the audit log`.

### Task 6: `/games.json`, docs, console check, pull request

**Files:** `source/remote.cpp` / `source/main.cpp` (`/games.json`), `docs/user-guide.md`,
`docs/configuration.md` (keys), `README.md`.

- [ ] Test: `/games.json` in the host simulator returns valid JSON with the summary and rows.
- [ ] Docs. Console check (controller, with the user): open the list on the hard disk and a USB
  stick, filters, jump, detail, check one 2352 `.iso` and "check all" a few, rename one and undo
  it, trash one and restore it, empty the trash with one test file.
- [ ] Commit `docs: game list`, push, open the pull request.
