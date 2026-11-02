// Remote view: a small web server on port 8090 with an MJPEG stream of the app's screen and a
// status page in JSON, and a pad in the web page whose buttons press the console's. Switched on in the settings.
#ifndef PSXRIP_REMOTE_H
#define PSXRIP_REMOTE_H

#define REMOTE_PORT 8090

struct RemoteHooks {
	int (*status_json)(char *out, int n);         // current state as JSON
	int (*debug)(const char *query, char *out, int n);   // /dbg?... (only with the debug flag file)
	int (*press)(const char *button);                     // POST /pad "cross", "l2", ...: a button press; 0 = known
};

void remote_init(const RemoteHooks *hooks);
void remote_set(int on);                          // start or stop the server
int remote_ip(char *out, int n);                  // the console's IP address, 0 on success

#endif
