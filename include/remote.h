// Remote view: a small web server on port 8090 with an MJPEG stream of the app's screen, a
// status page in JSON and a page to set the webhook address. Switched on in the settings.
#ifndef PSXRIP_REMOTE_H
#define PSXRIP_REMOTE_H

#define REMOTE_PORT 8090

struct RemoteHooks {
	int (*status_json)(char *out, int n);         // current state as JSON
	void (*get_webhook)(char *out, int n);        // webhook address
	void (*set_webhook)(const char *url);         // save a new address
	int (*test_webhook)();                        // send a test event, returns the HTTP status
	int (*get_events)();                          // bit mask of WEBHOOK_EVENTS that are sent
	void (*set_events)(int mask);
};

void remote_init(const RemoteHooks *hooks);
void remote_set(int on);                          // start or stop the server
int remote_ip(char *out, int n);                  // the console's IP address, 0 on success

#endif
