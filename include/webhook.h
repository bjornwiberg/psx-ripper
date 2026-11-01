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
// Appends a JSON string value (quoted and escaped) to out.
void json_str(char *out, size_t n, const char *s);

#endif
