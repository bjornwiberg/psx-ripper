// Audit log: every change the app makes (rips, settings, files), kept as text rows in audit.log and
// the sector bytes of each job in job<N>.bin, so a change can be shown and undone later.
#ifndef PSXRIP_AUDIT_H
#define PSXRIP_AUDIT_H

#include <stddef.h>
#include <stdint.h>

enum AuditSrc { AU_PAD, AU_REMOTE, AU_APP, AU_MCP, AU_UNDO };
enum AuditDom { AU_RIPS, AU_SETTINGS, AU_FILES, AU_MCPDOM };

// The image files (undo reads and writes sectors through this): main passes the vf_* functions,
// the host test stdio. Modes: "rb", "r+b".
struct AuditIo {
	void *(*open)(const char *path, const char *mode);
	size_t (*read)(void *f, void *p, size_t n);
	size_t (*write)(void *f, const void *p, size_t n);
	int (*seek)(void *f, uint64_t pos);
	int (*close)(void *f);
};

struct AuditRow {
	uint32_t id, job, lba, blob;
	uint32_t t;
	AuditSrc src;
	AuditDom dom;
	char disc[96], kind[12], what[160], before[192], after[192], file[256];
	int undone, reversible;
};

// Creates the directory and reads the next row id and job id from the log. A half-written last
// line (power loss) is dropped. May be called again to reload.
void audit_init(const char *dir, const AuditIo *io);
// Starts a job (`what` = rip | repair | check, `name` = display name, kept in the row's `before`).
uint32_t audit_job_begin(AuditSrc src, const char *disc, const char *name, const char *what);
void audit_job_end(uint32_t job, const char *result);
void audit_note(uint32_t job, AuditDom dom, const char *disc, const char *kind,
                const char *what, const char *before, const char *after);
// Logs one changed sector: old and new bytes go to job<N>.bin (4704 bytes per row).
void audit_sector(uint32_t job, const char *file, uint32_t lba, const uint8_t *old2352, const uint8_t *new2352);
void audit_setting(AuditSrc src, const char *key, const char *before, const char *after);

// Queries; each returns how many it wrote into out.
// One entry per disc that has rows, newest activity first.
int audit_discs(char (*keys)[96], char (*names)[96], int *events, uint32_t *last_t, int max);
// Events (job rows and rows outside any job, like settings) of a domain, newest first.
// `disc` NULL or "" = any disc.
int audit_events(AuditDom dom, const char *disc, AuditRow *out, int max);
// All rows of one job in log order, skipping the first `first`.
int audit_rows(uint32_t job, AuditRow *out, int first, int max);

#endif
