# Audit log — implementation plan

Steps use checkboxes (`- [ ]`). Work on branch `feat/audit-log`, one commit per task, one pull
request at the end (title `feat: audit log of every change, with undo`).

**Goal:** every change the app makes — sectors written into an image, settings, rips and repairs,
Redump results — is written to an audit log with where it came from and its state before and
after, can be undone (one row, one job, or everything on a disc since a time), and can be browsed
and undone on the console.

**Architecture:** a new module `source/audit.cpp` / `include/audit.h` owns the log: an
append-only text file of rows plus one binary file of old/new sector bytes per job. It does its
file I/O through an injected interface, so the host tests run it against real files. `main.cpp`
calls it at the few places where changes happen (`cfg_save`, the sector writes, job start and
end), tells it where an action came from (pad, remote view's pad, the app itself), and gets a new
screen for browsing and undoing.

**Tech stack:** C++ (gnu++14), PSL1GHT, the app's own UI (`ui.h`), host tests in `tests/host`.

**Spec:** `docs/design/2026-11-03-mcp-design.md`, section "Phase 0 — audit log".

## Global constraints

- Every UI text in English and Swedish (`source/lang.cpp`), new `StrId`s appended before
  `STR_COUNT` in the same order in both tables. Code, comments and log lines in English.
- No AI attribution anywhere (commits, PR, code). Conventional Commits.
- `make -C tests/host` before every commit that touches the UI; a scenario per new screen; look at
  the frames in `tests/host/out/`.
- PS3 build without warnings: `docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src ps3dev-psl1ght make pkg`.
- Never install on a console while it rips (`tools/deploy.sh` checks it).
- The package stays well under 1 MB.
- Secrets never go into the log: webhook header values are recorded only as "set"/"cleared".

## Decisions this plan fixes

- **Files:** `/dev_hdd0/tmp/psxrip_audit/audit.log` (rows, text) and
  `/dev_hdd0/tmp/psxrip_audit/job<N>.bin` (sector bytes of job N, 4704 bytes per sector row:
  2352 old, then 2352 new). Size cap: a **setting** (decided by the user), "Audit log sector
  data" / "Ändringsloggens sektordata", values 1, 2, 4, 8, 16, 32, 64, 128, 256 MB, default
  256 MB, config key `audit_mb`. When the `.bin` files together exceed it, the oldest job files are
  deleted (never the file of a job that is running); their rows stay and are no longer
  reversible. Lowering the setting prunes at once.
- **Row format**, one line, tab-separated `key=value`, values with `\t`, `\n` and `\\` escaped:
  `id`, `t` (unix time), `src` (`pad` | `remote` | `app` | `mcp` | `undo`), `dom` (`rips` |
  `settings` | `files` | `mcp`), `disc` (disc key, may be empty), `job` (0 = none), `kind`,
  `what`, `before`, `after`, and for sectors `file`, `lba`, `blob` (row index in `job<N>.bin`).
  Kinds in this plan: `job` (a rip/repair/check began: `what` = `rip` | `repair` | `check`),
  `sector`, `result` (`after` = `finished` | `cancelled` | `failed` | `disc_lost`), `redump`
  (`before`/`after` = `unchecked` | `verified <name>` | `no_match` | `not_found`), `setting`
  (`what` = config key), `undo` (`what` = the undone id, job or disc).
- **Disc key:** `<disc ID or title>:<sectors>`, e.g. `SCES_002.55:319250`; the display name is the
  rip's file name without extension. A rename (later phase) keeps the key.
- **Reversible:** `sector` rows (if their blob still exists and the image's sector still equals
  the row's new bytes) and `setting` rows (if the setting still has the row's `after` value).
  `job`, `result`, `redump` and `undo` rows are not reversible themselves; undoing a job undoes its
  sector rows. Undo goes newest first and stops at the first row that cannot be undone, reporting
  which and why; rows already undone stay undone (each is logged).
- **After undoing sectors**, the touched rip is checked again from its file (MD5 per track from
  its .cue, its `.md5` and `.log` rewritten, Redump compared against the local copy if there is
  one, otherwise its Redump line becomes `unchecked`), the same code the "check against Redump"
  job uses.
- **Where actions come from:** a press that `pad_pressed()` takes from the remote view's pad
  (`g_remote_btn`) is `remote`, any other press `pad`; automatic rips and repairs on insert are
  `app`. A job remembers the source of what started it.
- **Console UI:** a new row "Audit log" / "Ändringslogg" in the settings, just above "About". It
  opens a list screen with four levels: domains → discs (rips domain) or events (other domains) →
  events of a disc → rows of an event. X opens, O goes back, △ undoes the selected item after a
  yes/no dialog, L2/R2 page. Same look as the settings and webhook lists (rows, arrows at the
  right edge).

## Review focus

1. A crash or power loss in the middle of a job: the log must stay readable (append whole lines,
   flush per row); a half-written last line is ignored on load. Test: load a log whose last line
   has no newline.
2. Undoing a sector when the image was changed since (re-ripped, repaired again): refuse, never
   overwrite newer data. Test: change the sector after logging, undo is refused with a reason.
3. Two drives writing rows at the same time (two repairs): rows and ids must not interleave or
   repeat. Test: two threads writing 1000 rows each on the host, every id unique, every line whole.
4. The size cap removing a job file while that job is still running: never delete the running
   job's file. Test: cap reached during a job, its own file survives.
5. A setting row for a webhook header must not contain the header's value. Test: change a header,
   the row says `set`.

---

### Task 1: audit core — rows, persistence, queries

**Files:**
- Create: `include/audit.h`, `source/audit.cpp`
- Create: `tests/host/audit_test.cpp`
- Modify: `tests/host/Makefile` (build `audit_test` and run it in `run`, after `ui_test`), `Makefile`
  (nothing if `source/*.cpp` is picked up automatically; check)

**Interfaces (produces):**
```c
enum AuditSrc { AU_PAD, AU_REMOTE, AU_APP, AU_MCP, AU_UNDO };
enum AuditDom { AU_RIPS, AU_SETTINGS, AU_FILES, AU_MCPDOM };
struct AuditIo {   // the image files; main passes the vf_* functions, the host test stdio
	void *(*open)(const char *path, const char *mode);   // "rb", "r+b"
	size_t (*read)(void *f, void *p, size_t n);
	size_t (*write)(void *f, const void *p, size_t n);
	int (*seek)(void *f, uint64_t pos);
	int (*close)(void *f);
};
struct AuditRow { uint32_t id, job, lba, blob; uint32_t t; AuditSrc src; AuditDom dom;
                  char disc[96], kind[12], what[160], before[192], after[192], file[256]; int undone, reversible; };
void audit_init(const char *dir, const AuditIo *io);          // creates dir, reads next id/job
uint32_t audit_job_begin(AuditSrc src, const char *disc, const char *name, const char *what);  // -> job id
void audit_job_end(uint32_t job, const char *result);          // a "result" row
void audit_note(uint32_t job, AuditDom dom, const char *disc, const char *kind,
                const char *what, const char *before, const char *after);
void audit_sector(uint32_t job, const char *file, uint32_t lba, const uint8_t *old2352, const uint8_t *new2352);
void audit_setting(AuditSrc src, const char *key, const char *before, const char *after);
// queries for the screen (and later MCP); each returns how many it wrote into out
int audit_discs(char (*keys)[96], char (*names)[96], int *events, uint32_t *last_t, int max);
int audit_events(AuditDom dom, const char *disc, AuditRow *out, int max);   // job/setting rows, newest first
int audit_rows(uint32_t job, AuditRow *out, int first, int max);           // the rows of one job
```
Thread safety: one internal `sys_mutex` (host stub exists) around every write and query.

- [ ] **Step 1: tests** in `audit_test.cpp` (own `main`, prints `ok`/`FAIL` lines like ui_test, exit 1 on failure), using `./out/audit` and stdio-backed `AuditIo`:
  - `rows_survive_reload`: job_begin + 3 sector rows + setting row; `audit_init` again; ids continue (next id = last + 1), `audit_rows(job)` returns the 3 sectors with `lba` and `file` intact.
  - `half_line_ignored`: append `id=99\tt=` without newline to `audit.log`; reload; no row 99, next id correct.
  - `escaping`: a `what` with a tab, a newline and a backslash round-trips.
  - `blob_layout`: after 2 sector rows, `job<N>.bin` is 9408 bytes; bytes 2352..4703 are row 0's new bytes.
  - `threads`: two threads × 1000 `audit_note` rows; 2000 unique ids, every line parses.
  - `discs_grouping`: rows for two discs; `audit_discs` returns both with event counts and the newest time first.
- [ ] **Step 2:** run `make -C tests/host`; expect the new tests to fail (no module).
- [ ] **Step 3:** implement `audit.cpp` (stdio for `audit.log` and `job<N>.bin`, they live on the hard disk; `fflush` after every row).
- [ ] **Step 4:** run `make -C tests/host`; all pass. PS3 build without warnings.
- [ ] **Step 5:** commit `feat: audit log module (rows, sector bytes, queries)`.

### Task 2: undo in the core

**Files:** `source/audit.cpp`, `include/audit.h`, `tests/host/audit_test.cpp`

**Interfaces (produces):**
```c
// 0 = done; -1 = refused, why[] says which row and why. Each undone row gets an "undo" row.
// set_setting applies a config key/value the way the app does (main passes it; NULL in tests).
int audit_undo_row(uint32_t id, char *why, size_t n);
int audit_undo_job(uint32_t job, char *why, size_t n);
int audit_undo_disc_since(const char *disc, uint32_t since_t, char *why, size_t n);
void audit_set_hooks(int (*set_setting)(const char *key, const char *value),
                     void (*recheck)(const char *file));   // after sectors of a file were undone
void audit_set_cap(uint64_t bytes);   // the audit_mb setting; prunes at once when lowered
```

- [ ] **Step 1: tests:**
  - `undo_sector`: write a sector into a test image through the audit (old A, new B); `audit_undo_row` → the image has A again, an `undo` row exists, the row reads `undone`.
  - `undo_refused_when_changed`: after logging A→B, overwrite the sector with C; undo returns -1, image still C, `why` names the row.
  - `undo_job_newest_first`: a job writes the same sector twice (A→B, B→C); undo job → A.
  - `undo_missing_blob`: delete `job<N>.bin`; the row is `reversible = 0`; undo refused.
  - `undo_setting`: `audit_setting("speed","0","3")` with a test hook; undo calls the hook with `speed`,`0`.
  - `recheck_called_once_per_file`: undo a job with sectors in one file → `recheck` called once with that file.
  - `cap_keeps_running_job`: cap set low with `audit_set_cap`, a running job's file survives, an older job's file is removed.
  - `cap_lowered_prunes_now`: three finished jobs' files, `audit_set_cap` below their total → the oldest are removed at once until under the cap; their rows read `reversible = 0`.
- [ ] **Step 2:** tests fail. **Step 3:** implement. **Step 4:** pass, PS3 build clean.
- [ ] **Step 5:** commit `feat: undo of audit log rows, jobs and discs`.

### Task 3: where actions come from, and settings

**Files:** `source/main.cpp` (`pad_pressed`, `cfg_save`, `cfg_load`), `tests/host/ui_test.cpp`

**Interfaces:**
- Produces in `main.cpp`: `static AuditSrc g_input_src;` set by `pad_pressed()` for the presses it returns (`AU_REMOTE` when any bit came from `g_remote_btn`, else `AU_PAD`); `static int cfg_apply_line(const char *line);` (the per-line parsing taken out of `cfg_load`, which now calls it); `static int set_setting_hook(const char *key, const char *value);` (= `cfg_apply_line("key=value")`, then `cfg_save` with source `AU_UNDO`).
- `cfg_save()` keeps the text it wrote last; after writing, every line that changed becomes `audit_setting(src, key, before, after)` with `src = g_cfg_src` (defaults to `g_input_src`; set to `AU_UNDO` by the hook). Lines `webhook_header1..3` record `set`/`cleared` instead of values.
- `audit_init("/dev_hdd0/tmp/psxrip_audit", &vf_io)` in `main()` before `cfg_load`; the first `cfg_save` after start records nothing (it only primes the last text).
- New config key `audit_mb` (1, 2, 4, 8, 16, 32, 64, 128, 256; default 256; anything else → 256), read in `cfg_apply_line`, written by `cfg_save`, applied with `audit_set_cap(mb << 20)` at start and on change.

- [ ] **Step 1: tests** in `ui_test.cpp` (host `audit_init` into `out/audit`): change `g_std.speed` and call `cfg_save()` → one `setting` row `speed` before/after; a header change → row with `set`; the remote pad path: set `g_remote_btn`, call `pad_pressed()`, `g_input_src == AU_REMOTE`.
- [ ] **Step 2:** fail. **Step 3:** implement. **Step 4:** pass; PS3 build clean.
- [ ] **Step 5:** commit `feat: settings changes and their source in the audit log`.

### Task 4: jobs and sectors

**Files:** `source/main.cpp` (`rip_job_start`, `repair_job_start`, `check_job_start`, the automatic start in `home_screen`, `verify_pass`, `second_pass`, the end of `rip_disc_inner` and `repair_disc_inner`, `check_job_thread`), `tests/host/ui_test.cpp`

**Interfaces:**
- `Card` gets `uint32_t audit_job; AuditSrc job_src;`. The three `*_job_start` take the source from the caller (`g_input_src`, or `AU_APP` from the automatic start) and call `audit_job_begin(src, disc_key, file_name, "rip"|"repair"|"check")`.
- `static void disc_key(char *out, size_t n);` in the card context: `<serial or title>:<g_leadout>`.
- `static int write_sector_audited(uint32_t lba, const uint8_t *sec);` replaces the three `vf_seek`+`vf_write` patch sites in `verify_pass`/`second_pass`: reads the old 2352 bytes from `g_out`, writes, calls `audit_sector`. Returns 0 on success like before.
- End of a rip: `audit_job_end(job, "finished"|"cancelled"|"failed"|"disc_lost")`; Redump result of a rip, repair or check: `audit_note(job, AU_RIPS, key, "redump", "", before, after)` with the rip's previous Redump line as `before`.
- `static int check_image(const char *base);` the file-checking part of `check_job_thread` taken out (MD5 per track from the .cue, `.md5` and `.log` written, local Redump compare); `check_job_thread` calls it; it is the `recheck` hook of Task 2 (run on the UI thread with a progress dialog).

- [ ] **Step 1: tests:** in `ui_test.cpp` with a small test image file on the host: `write_sector_audited` on an image opened `r+b` logs a `sector` row with the old bytes and writes the new; `check_image` on a 2-track test image writes `.md5` with both tracks' MD5 (compare with values computed in the test).
- [ ] **Step 2:** fail. **Step 3:** implement. **Step 4:** pass; PS3 build clean.
- [ ] **Step 5:** on the console (idle): repair a disc with known errors (Tekken 2 or Croc 2 in USB 1 or the internal drive); `/dev_hdd0/tmp/psxrip_audit/audit.log` over FTP has a `job` row with `src=app`, `sector` rows with the right lbas, a `redump` and a `result` row.
- [ ] **Step 6:** commit `feat: rips, repairs, checks and their sector writes in the audit log`.

### Task 5: the audit log screen

**Files:** `source/main.cpp` (`settings_screen`: two new rows before About — "Audit log" (X opens the screen) and "Audit log sector data" (←→ steps through 1 … 256 MB, right increases) — `NROWS` 11 → 13; new `audit_screen()`), `include/lang.h`, `source/lang.cpp`, `tests/host/ui_test.cpp`

**Interfaces:** consumes the queries and undo functions of Tasks 1–2 and `check_image` of Task 4.

Texts (en / sv): row `Audit log` / `Ändringslogg`; row `Audit log sector data` / `Ändringsloggens sektordata`, value `%d MB`; domains `Rips` / `Rippningar`, `Settings` / `Inställningar`; sources `pad` / `handkontroll`, `remote view` / `fjärrvisning`, `the app` / `appen`, `undo` / `ångrad`; hint `Undo` / `Ångra`; dialog `Undo %s?` / `Ångra %s?`; refused `Could not undo: %s` / `Kunde inte ångra: %s`; empty `Nothing has changed yet` / `Inget har ändrats än`; a sector row `Sector %u: changed` / `Sektor %u: ändrad`; an undone mark `(undone)` / `(ångrad)`.

- [ ] **Step 1: scenarios** in `ui_test.cpp` with a prepared log: `audit-domains`, `audit-discs` (two discs), `audit-events` (a rip and a repair with 1017 rows), `audit-rows` (paged sector list), `audit-undo-dialog`, `audit-empty`, and `settings` with the new row; the overlap/off-screen checks pass in both languages.
- [ ] **Step 2:** fail (no screen). **Step 3:** implement. **Step 4:** pass; look at the frames; PS3 build clean.
- [ ] **Step 5:** commit `feat: audit log screen with undo`.

### Task 6: documentation and the pull request

**Files:** `docs/user-guide.md` (the screen, what is logged, undo), `docs/configuration.md` (where the files are; the `audit_mb` key in the table), `README.md` (one line in the feature list).

- [ ] **Step 1:** write the docs. **Step 2:** `make -C tests/host`, PS3 build.
- [ ] **Step 3:** on the console (idle, `tools/deploy.sh`): change a setting with the pad and one with the remote view's pad → both in the screen with the right source; undo one → value back, an undo row; undo a repair's job on a disc → the image's MD5 back to its log's earlier value and its Redump line updated.
- [ ] **Step 4:** commit `docs: audit log`, push `feat/audit-log`, open the pull request (Conventional Commits title, body: what is logged, where, undo, how it was tested).
