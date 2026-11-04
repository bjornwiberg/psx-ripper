// Audit log: every change the app makes (rips, settings, files), kept as text rows in audit.log and
// the sector bytes of each job in job<N>.bin, so a change can be shown and undone later.
#ifndef PSXRIP_AUDIT_H
#define PSXRIP_AUDIT_H

#include <stddef.h>
#include <stdint.h>

enum AuditSrc { AU_PAD, AU_REMOTE, AU_APP, AU_MCP, AU_UNDO };
enum AuditDom { AU_RIPS, AU_SETTINGS, AU_FILES, AU_MCPDOM };

// Why a row cannot be undone (or an undo was refused), as a code the UI says in its own language;
// why[] keeps the English text for logs.
enum AuditWhy {
	AW_NONE,              // can be undone / was undone
	AW_NO_ROW,            // no such row (row)
	AW_ALREADY,           // already undone (row)
	AW_RECORD,            // a job, result, redump or undo row: a record, not a change (row)
	AW_SECRET,            // a secret setting: its value was never logged (row, what = key)
	AW_SETTING_CHANGED,   // the setting changed since (row, what = key)
	AW_SECTOR_CHANGED,    // the image's sector changed since (row, lba, file)
	AW_DROPPED,           // the sector's bytes were dropped by the size cap (row, job)
	AW_UNFINISHED,        // the rip was not finished (row, job, what = its result or "")
	AW_RUNNING,           // the job is still running (row, job)
	AW_JOB_ON_DISC,       // another job runs on this disc (row, job = that one)
	AW_CANNOT_OPEN,       // the image cannot be opened (row, file)
	AW_CANNOT_WRITE,      // the image cannot be written (row, file)
	AW_LOG_WRITE,         // undone, but the log row could not be written (row)
	AW_NOT_APPLIED,       // the setting could not be applied (row, what = key)
	AW_NO_HOOK,           // settings cannot be changed here (row)
	AW_NOTHING,           // a job or disc with nothing (left) to undo
	AW_NO_DISC,           // no disc given
	AW_REPLACED,          // the file was ripped again since: the row's bytes belong to an older image (row, file)
	AW_STOPPED,           // the undo was stopped (the app is closing); the rows before stay undone (row)
	AW_MOVE_TAKEN,        // a file is in the way where a move would put one (row, file)
	AW_MOVE_GONE,         // the file a move would take is gone (row, file)
	AW_TRASH_EMPTIED,     // the file a move would take was deleted when the trash was emptied (row, file)
	AW_FILE_BUSY,         // a drive or a check works on the file now (row, file)
	AW_CANNOT_MOVE,       // the file could not be moved (row, file); the files moved before went back
	AW_TOO_LONG,          // a path is too long to be kept in the log (file)
	AW_MOVE_FAILED        // the move failed and was put back: it is never done again (row)
};
struct AuditReason {
	AuditWhy code;
	uint32_t row, job, lba;
	char what[96], file[96];   // file: the image's name without its directory
};

// The image files (undo reads and writes sectors through this): main passes the vf_* functions,
// the host test stdio. Modes: "rb", "r+b".
struct AuditIo {
	void *(*open)(const char *path, const char *mode);
	size_t (*read)(void *f, void *p, size_t n);
	size_t (*write)(void *f, const void *p, size_t n);
	int (*seek)(void *f, uint64_t pos);
	int (*close)(void *f);
	// moves (rename, trash): NULL = stdio rename / stat
	int (*rename)(const char *from, const char *to);   // 0 = moved; never over an existing file
	int (*exists)(const char *path);                    // 1 = there
};

struct AuditRow {
	uint32_t id, job, lba, blob;
	uint32_t t;
	AuditSrc src;
	AuditDom dom;
	char disc[96], kind[12], what[160], before[192], after[192], file[256];
	// an undo row, as the queries fill it: key = the setting its chain goes back to, or lba and
	// file = that sector's
	char key[48];
	int undone, reversible;
	int redo;    // undoing this row applies a change again: an undone sector or setting row, or an
	             // undo row whose change is undone now
	int norev;   // the row was written with rev=0: its value is not in the log, it can never be undone
	AuditWhy why;       // not reversible: why (AW_NONE when it is)
	uint32_t why_job;   // for AW_RUNNING / AW_JOB_ON_DISC / AW_UNFINISHED / AW_DROPPED: the job meant
};

// Creates the directory and reads the next row id and job id from the log. A half-written last
// line (power loss) is dropped. May be called again to reload.
void audit_init(const char *dir, const AuditIo *io);
// Starts a job (`what` = rip | repair | check in AU_RIPS, rename | trash | restore | empty in AU_FILES;
// `name` = display name, kept in the row's `before`). Its job and result rows are in `dom`.
uint32_t audit_job_begin(AuditSrc src, const char *disc, const char *name, const char *what, AuditDom dom = AU_RIPS);
// A finished rip names the image it made (`image`, kept in its result row): rows of older jobs on
// that file are then never undone, their bytes belong to the image it replaced.
void audit_job_end(uint32_t job, const char *result, const char *image = NULL);
void audit_note(uint32_t job, AuditDom dom, const char *disc, const char *kind,
                const char *what, const char *before, const char *after);
// Logs one changed sector: old and new bytes go to job<N>.bin (4704 bytes per row).
void audit_sector(uint32_t job, const char *file, uint32_t lba, const uint8_t *old2352, const uint8_t *new2352);
void audit_setting(AuditSrc src, const char *key, const char *before, const char *after);
// Same for a secret setting: `before`/`after` are only labels (set / cleared); the row is never reversible.
void audit_setting_private(AuditSrc src, const char *key, const char *before, const char *after);
// The value of each setting as the log knows it (its newest row, undos counted): keys up to 47
// characters, values as logged (a secret: set / cleared). Returns how many.
int audit_setting_values(char (*keys)[48], char (*vals)[192], int max);

// Queries; each returns how many it wrote into out.
// One entry per disc that has rows (of domain `dom`; -1 = any), newest activity first.
int audit_discs(char (*keys)[96], char (*names)[96], int *events, uint32_t *last_t, int max, int dom = -1);
// Events (job rows and rows outside any job, like settings) of a domain, newest first.
// `disc` NULL or "" = any disc.
int audit_events(AuditDom dom, const char *disc, AuditRow *out, int max);
// All rows of one job in log order, skipping the first `first`.
int audit_rows(uint32_t job, AuditRow *out, int first, int max);

// Undo. Reversible are `sector` rows (their bytes still kept in job<N>.bin, the image's sector still
// equal to the row's new bytes, the job not running) and `setting` rows (the setting still has the
// row's `after`). Rows go newest first and stop at the first one that cannot be undone; rows undone
// before that stay undone. Each undone row gets an `undo` row (`what` = its id).
// An `undo` row can itself be undone (a redo): the change it reversed is applied again, under the
// same guards (a sector: the image still has the bytes the undo put back; a setting: it still has
// the value the undo set), and a new `undo` row names it, so the chain goes on. A row is undone
// after an odd number of undos in its chain; an undo row is undone once a newer one follows it.
// Undoing an undone sector or setting row redoes it: the newest undo row of its chain is undone
// (rs and why[] then name that row), so it can be undone and redone by its own id any number of
// times.
// A job or disc whose sector rows are all undone is redone by its undo: oldest row first, stopping
// at the first refusal the same way.
// Returns 0 when done, -1 when refused; why[] then names the row and the reason.
// rs (may be NULL) gets the reason as a code (AW_NONE when done).
int audit_undo_row(uint32_t id, char *why, size_t n, AuditReason *rs = NULL);
int audit_undo_job(uint32_t job, char *why, size_t n, AuditReason *rs = NULL);
// every reversible row of a disc from time since_t on
int audit_undo_disc_since(const char *disc, uint32_t since_t, char *why, size_t n, AuditReason *rs = NULL);
// 1 when undoing the job / all of the disc would undo (or redo) at least one row (the same guards
// as the undo: the first row it would take must be undoable now), else 0 and rs says why.
// redo (may be NULL) = 1 when that undo would be a redo (every sector row undone).
int audit_job_reversible(uint32_t job, AuditReason *rs = NULL, int *redo = NULL);
int audit_disc_reversible(const char *disc, AuditReason *rs = NULL, int *redo = NULL);
// set_setting applies a config key/value the way the app does (0 = applied); recheck checks a rip
// again from its file, called once per file after its sectors were undone or redone, with the n
// sectors written (lbas), for each whether the row's old bytes are all zero (zero[i] = 1: a sector
// the rip could not read), and redo = 1 when they got the row's new bytes again (0: the old ones
// back). Both are called without the audit's lock held, so they may write audit rows.
// NULL = none (a setting is then not undone).
void audit_set_hooks(int (*set_setting)(const char *key, const char *value),
                     void (*recheck)(const char *file, const uint32_t *lbas, const uint8_t *zero, int n, int redo));
// Called by a job or disc undo before every AUDIT_TICK sectors (done of total), without the lock:
// the app keeps its screen and the system's callbacks going. Nonzero stops the undo there (rows
// undone so far stay undone, the recheck still runs). NULL = none.
#define AUDIT_TICK 64
void audit_set_progress(int (*tick)(uint32_t done, uint32_t total));
// Moves files as one job in AU_FILES (`what` = rename | trash | restore, `name` = the game's name):
// from[i] -> to[i], each a `move` row (before = from, after = to). Refused before anything moves when
// a path is too long for the log, a file is busy, a from is missing or a to is taken (no job then).
// When a move fails (or the moved hook does), the files moved so far go back, newest first, each with
// an `undo` row, and the job ends "failed": a set never stays split, and a failed job is never
// redone. Returns 0 when every file moved, else -1 with why[] and rs.
//
// Undo of moves: a move row, an undo row of one, or its job undo the job's moves together (a set is
// never split): newest first, each back from `after` to `before`; when all are undone the undo is a
// redo, oldest first, before -> after again (only for a job that finished). Every file is checked
// first (the file there, its place free, not busy; a file deleted by an `empty` job since: the
// trash was emptied); a refusal moves nothing. A move that fails then puts back the ones done.
// Emptying the trash: a job `empty` with a `delete` row per deleted file (before = its path), a
// record: never undone.
int audit_move(AuditSrc src, const char *disc, const char *name, const char *what,
               const char *const *from, const char *const *to, int n, char *why, size_t wn, AuditReason *rs = NULL);
// moved (may be NULL) is called after each file moved (also by an undo or a rollback), without the
// lock; nonzero = it failed (the move counts as failed and is put back). busy (may be NULL): 1 when
// the file must not be moved now (a drive rips into it, a check reads it).
void audit_set_move_hooks(int (*moved)(const char *from, const char *to), int (*busy)(const char *path));

// The audit_mb setting in bytes (default 256 MB). When the job<N>.bin files together exceed it, the
// oldest are deleted, never the file of a running job; their rows stay, no longer reversible.
// Applies at once.
void audit_set_cap(uint64_t bytes);

#endif
