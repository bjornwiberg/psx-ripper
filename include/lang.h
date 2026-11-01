// UI strings. English is the fallback; add a language by adding a table in lang.cpp.
#ifndef PSXRIP_LANG_H
#define PSXRIP_LANG_H

enum StrId {
	STR_WELCOME,
	STR_OPEN_FAILED,        // %x
	STR_TOC_FAILED,         // %x, %s log path
	STR_READCD_FAILED,      // %x, %s log path
	STR_UNKNOWN,
	STR_FOUND,              // %s name, %s serial, %d tracks, %u MB, %s out dir
	STR_RIPPING,
	STR_PROGRESS,           // %u done, %u total
	STR_ABORTED,
	STR_CREATE_FAILED,
	STR_LC_UNCHECKED,
	STR_LC_NONE,
	STR_LC_MISSING,
	STR_LC_BUILTIN,
	STR_LC_FOUND,           // %d sectors, %s path
	STR_LC_NOSERIAL,        // %d sectors
	STR_LC_NOTPAL,
	STR_DONE,               // %s base, %s libcrypt info, %u errors
	STR_ASK_REFRESH,
	STR_NEXT_DISC,
	STR_REFRESH_MANUAL,
	STR_COUNT
};

void lang_init();
const char *T(StrId id);

#endif
