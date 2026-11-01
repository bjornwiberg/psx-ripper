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
	STR_HOME_INSERT,
	STR_HOME_SAVING,
	STR_HINT_RIP,
	STR_HINT_SETTINGS,
	STR_HINT_EXIT,
	STR_SETTINGS_TITLE,
	STR_L_TARGET,
	STR_L_RETRIES,
	STR_L_SPEED,
	STR_L_EJECT,
	STR_HINT_SELECT,
	STR_HINT_CHANGE,
	STR_HINT_SAVE,
	STR_HINT_BACK,
	STR_L_AUTOSTART,
	STR_HOME_READING,
	STR_HOME_NOTPSX,
	STR_HINT_EJECT,
	STR_L_REDUMP,
	STR_RD_ONLINE,
	STR_RD_LOCAL,
	STR_L_RDCOPY,
	STR_RD_MISSING,
	STR_HINT_DOWNLOAD,
	STR_RD_DOWNLOADING,
	STR_RD_DL_PROGRESS,
	STR_RD_INDEXING,
	STR_RD_DL_DONE,
	STR_RD_DL_FAILED,
	STR_RD_GUESS,
	STR_RD_GUESS_MORE,
	STR_RD_GUESS_NONE,
	STR_RD_SEARCHING,
	STR_RD_VERIFIED,
	STR_RD_NOMATCH,
	STR_RD_UNCHECKED,
	STR_RD_NOTFOUND,
	STR_RD_COPY_VALUE,
	STR_BAD_SECTORS,
	STR_HINT_CANCEL,
	STR_COUNT
};

void lang_init();
const char *T(StrId id);

#endif
