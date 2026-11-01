// Webhooks: an HTTP POST with a JSON body to a configured address when something happens
// (rip started, finished, cancelled, failed). Plain http only.
#ifndef PSXRIP_WEBHOOK_H
#define PSXRIP_WEBHOOK_H

#include <stddef.h>

// Sends now and returns the HTTP status (negative on error).
int webhook_post(const char *url, const char *json);
// Sends from a background thread, so ripping never waits for the network.
void webhook_send_async(const char *url, const char *json);
// "http://host:port/path" -> "host:port" (for display and logs, without the secret path).
void webhook_host(const char *url, char *out, size_t n);
// Events a webhook can be sent for (bit i of the mask = WEBHOOK_EVENTS[i]).
enum {
	WH_DISC_IDENTIFIED, WH_RIP_STARTED, WH_RIP_PROGRESS, WH_RIP_FINISHED,
	WH_RIP_CANCELLED, WH_RIP_FAILED, WH_REPAIR_FINISHED, WH_DISC_EJECTED, WH_DISC_NOT_IDENTIFIED, WH_COUNT
};
static const char *const WEBHOOK_EVENTS[WH_COUNT] = {
	"disc_identified", "rip_started", "rip_progress", "rip_finished",
	"rip_cancelled", "rip_failed", "repair_finished", "disc_ejected", "disc_not_identified"
};
#define WH_DEFAULT_MASK ((1 << WH_RIP_FINISHED) | (1 << WH_RIP_CANCELLED) | (1 << WH_RIP_FAILED) | (1 << WH_REPAIR_FINISHED) | \
                         (1 << WH_DISC_NOT_IDENTIFIED))
// Screen texts for the events: long (web page) and short (home screen)
#include "lang.h"
static inline StrId wh_label(int i)
{
	static const StrId l[WH_COUNT] = {STR_WH_EV_0, STR_WH_EV_1, STR_WH_EV_2, STR_WH_EV_3, STR_WH_EV_4, STR_WH_EV_5, STR_WH_EV_6, STR_WH_EV_7, STR_WH_EV_8};
	return l[i];
}
static inline StrId wh_short(int i)
{
	static const StrId l[WH_COUNT] = {STR_WH_SHORT_0, STR_WH_SHORT_1, STR_WH_SHORT_2, STR_WH_SHORT_3, STR_WH_SHORT_4, STR_WH_SHORT_5, STR_WH_SHORT_6, STR_WH_SHORT_7, STR_WH_SHORT_8};
	return l[i];
}
int webhook_event_index(const char *name);

// Appends a JSON string value (quoted and escaped) to out.
void json_str(char *out, size_t n, const char *s);

#endif
