#include <sysutil/sysutil.h>
#include "lang.h"

// PS3 system language ids (SYSUTIL_SYSTEMPARAM_ID_LANG)
#define PS3_LANG_ENGLISH_US 1
#define PS3_LANG_SWEDISH    13
#define PS3_LANG_ENGLISH_UK 18

static const char *en[STR_COUNT] = {
	"PSX Ripper\n\nInsert a PS1 disc - it is detected automatically.\nIs the disc already in the drive? Select Yes.\nSelect No to exit.",
	"Could not open the disc drive (0x%x).",
	"Could not read the disc's table of contents (0x%x).\nIs this a PS1 disc?\n\nLog: %s",
	"The drive refused raw reading (READ CD 0x%x).\n\nLog: %s",
	"unknown",
	"Found: %s\nSerial: %s\nTracks: %d   Size: %u MB\n\nRip to %s?",
	"Ripping %s [%s]\n○ cancels",
	"%u / %u sectors",
	"Ripping was cancelled. The incomplete file has been removed.",
	"Could not create the file on the hard disk.",
	"LibCrypt: could not be checked (no subchannel data).",
	"LibCrypt: no protection found.",
	"LibCrypt: this game is protected, but no protection data was found on the disc. This can happen if the disc is not an original. The game may not start.",
	"LibCrypt: no protection data on the disc; the built-in .sbi was installed.",
	"LibCrypt: %d protected sectors, saved to %s",
	"LibCrypt: %d mismatching sectors, but the serial is unknown.",
	"LibCrypt: not applicable (only PAL discs use it).",
	"Done!\n%s.bin/.cue\n%s\nUnreadable sectors: %u",
	"Update the webMAN game list now?",
	"The disc has been ejected. Insert the next disc and it will be ripped automatically.\n○ stops automatic ripping.",
	"webMAN did not answer. Run Refresh XML in webMAN to see the new games.",
};

static const char *sv[STR_COUNT] = {
	"PSX Ripper\n\nSätt i en PS1-skiva – den upptäcks automatiskt.\nSitter skivan redan i? Välj Ja.\nVälj Nej för att avsluta.",
	"Kunde inte öppna skivläsaren (0x%x).",
	"Kunde inte läsa skivans innehållsförteckning (0x%x).\nÄr det en PS1-skiva?\n\nLogg: %s",
	"Skivläsaren vägrar rå läsning (READ CD 0x%x).\n\nLogg: %s",
	"okänt",
	"Hittade: %s\nSkivnummer: %s\nSpår: %d   Storlek: %u MB\n\nRippa till %s?",
	"Rippar %s [%s]\n○ avbryter",
	"%u / %u sektorer",
	"Rippningen avbröts. Den ofullständiga filen är borttagen.",
	"Kunde inte skapa filen på hårddisken.",
	"LibCrypt: kunde inte kontrolleras (subkanal saknas).",
	"LibCrypt: inget skydd hittat.",
	"LibCrypt: spelet är skyddat, men inga skyddsdata hittades på skivan. Det kan hända om skivan inte är ett original. Spelet kanske inte startar.",
	"LibCrypt: skyddsdata saknades på skivan; den inbyggda .sbi-filen installerades.",
	"LibCrypt: %d skyddade sektorer, sparat i %s",
	"LibCrypt: %d avvikande sektorer, men skivnummer saknas.",
	"LibCrypt: ej aktuellt (används bara på PAL-skivor).",
	"Klart!\n%s.bin/.cue\n%s\nOläsbara sektorer: %u",
	"Uppdatera webMANs spellista nu?",
	"Skivan har matats ut. Sätt i nästa skiva så rippas den automatiskt.\n○ avslutar den automatiska rippningen.",
	"webMAN svarade inte. Kör Refresh XML i webMAN för att se de nya spelen.",
};

static const char **table = en;

void lang_init()
{
	s32 lang = PS3_LANG_ENGLISH_US;
	sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_LANG, &lang);
	switch (lang) {
	case PS3_LANG_SWEDISH: table = sv; break;
	default: table = en; break;
	}
}

const char *T(StrId id)
{
	const char *s = table[id];
	return s ? s : en[id];
}
