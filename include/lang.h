// UI strings. English is the fallback; add a language by adding a table in lang.cpp.
#ifndef PSXRIP_LANG_H
#define PSXRIP_LANG_H

enum StrId {
	STR_WELCOME,            // %s target
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
	STR_SET_TARGET,         // %s target
	STR_SET_RETRIES,        // %d retries
	STR_TARGET_HDD,         // %s path
	STR_TARGET_USB,         // %d port, %s path
	STR_SET_SPEED,          // %s speed
	STR_SET_EJECT,          // %s on/off
	STR_SPEED_MAX,
	STR_ON,
	STR_OFF,
	STR_MENU,
	STR_NEXT_DISC_NOEJECT,
	STR_COUNT
};

void lang_init();
const char *T(StrId id);

#endif
