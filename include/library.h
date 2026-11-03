// Game library: the game images in a source's PSXISO folder, as sets of files sharing a base name
// (.bin/.cue/.log/.md5/.jpg/.sbi, or a single .iso/.img), each with its format and status, grouped
// into the rows of the game list. Pure logic over an injected file interface (LibIo), so the host
// tests run it on real folders and the app on its own file layer.
#ifndef PSXRIP_LIBRARY_H
#define PSXRIP_LIBRARY_H

#include <stdint.h>

enum GameFmt { GF_UNKNOWN, GF_2352, GF_2048 };
// Worst first: a group's status is the lowest of its discs'.
enum GameStatus { GS_BROKEN, GS_RERIP, GS_ERRORS, GS_UNCHECKED, GS_NOMATCH, GS_VERIFIED };
// The list's filter: criteria that all have to match (0 = any). A multi-disc row matches when
// one of its discs matches all of them. The trash is not a filter: it is its own scan (.trash).
struct GameFilter { int fmt;     /* 0 any, 1 only 2352, 2 only 2048 */
                    int redump;  /* 0 any, 1 verified, 2 not checked, 3 no match (no_match/not_found), 4 not verified (= 2 or 3) */
                    int errors;  /* 0 any, 1 with errors, 2 without errors */
                    int other;   /* 0 any, 1 broken sets, 2 duplicates, 3 LibCrypt .sbi missing */ };

#define LIB_FILES 2048   // bytes for the names of a set's files

// One disc image (a set). base = folder + "/" + base name (no extension); image = the image file
// (the first .bin of a cue, or the .iso/.img); size = all its image files together. errors = sectors
// the log still lists as wrong (unreadable=, bad=, uncertain=... unconfirmed: the rule of the
// app's repair), bad = of those the bad= lines. title, rdname, date, drive come from the log;
// without one the title is the base name without [ID]. rd = the log's Redump result (1 verified,
// 0 no match / not found, -1 none), whatever the status.
struct GameDisc { char base[200], image[220], id[16], title[128], rdname[160], date[24], drive[32];
                  uint64_t size; GameFmt fmt; int tracks, errors, bad; GameStatus st; int has_cover,
                  has_log, libcrypt /*0 n/a, 1 sbi present, -1 missing*/, dup /*index of the other, -1*/, rd;
                  // every file of the set (names in base's folder, each ending in a 0), so moving the
                  // set moves exactly these; files_cut = the names did not all fit (do not move it)
                  int nfiles, files_cut; char files[LIB_FILES]; };
struct Game { char name[128]; int first, ndiscs; GameStatus st; };   // a row: one disc or a group
// list: calls cb with each name in dir (without the folder), returns < 0 when dir cannot be read.
// stat_size/read/exists take full paths; read returns the bytes read (< 0 on error), no 0 added.
// libcrypt (may be NULL): whether a disc ID is on the app's LibCrypt list; sbi_dir: where the
// installed .sbi files are (<sbi_dir>/<ID>.sbi).
struct LibIo { int (*list)(const char *dir, void (*cb)(const char *, void *), void *);
               int (*stat_size)(const char *path, uint64_t *size);
               int (*read)(const char *path, char *buf, int max);   // whole small text files
               int (*exists)(const char *path);
               int (*libcrypt)(const char *id);
               const char *sbi_dir; };

// The sets of one folder, unsorted; up to max. Returns how many, -1 when the folder cannot be read.
// dup is -1 in each: lib_mark_dups() over all the scanned sources sets it.
int lib_scan(const char *dir, const LibIo *io, GameDisc *discs, int max);
// Sorts the discs into rows and fills games, sorted by name (case-insensitive, ASCII before the
// rest). A group: discs in one folder whose title and region/language groups match apart from
// "(Disc N)", each number once; contiguous, by disc number. dup indices follow the move.
// Returns the rows (<= max).
int lib_group(GameDisc *discs, int n, Game *games, int max);
// Same disc ID and image size, or the same Redump name: each gets the other's index in dup.
void lib_mark_dups(GameDisc *discs, int n);
// The rows a filter shows (indices into g, in order); returns how many. f NULL = everything.
int lib_filter(const Game *g, int n, const GameDisc *d, const GameFilter *f, int *out);
// Counts over discs; any pointer may be NULL.
void lib_summary(const GameDisc *d, int n, int *total, int *verified, int *errors, int *rerip, int *broken);
// Jump by letter: shown = row indices (lib_filter), n of them, from = position in shown. dir > 0:
// the first row with the next letter; dir < 0: the first row of the previous letter (or of the
// current one when from is inside it). Returns a position in shown; from when there is nowhere to go.
int lib_first_letter_next(const Game *g, const int *shown, int n, int from, int dir);
// Full path of a set's i-th file into out; 0 when i is out of range.
int lib_file_path(const GameDisc *d, int i, char *out, int n);

#endif
