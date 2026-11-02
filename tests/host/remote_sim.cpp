// Remote view simulator: the app's own screens and the real web server (source/remote.cpp) on
// this computer, so the remote view can be tried without a console.
//   make -C tests/host remote      then open http://<this computer>:8090 (a phone on the same network works)
// Two drives with discs; the buttons on the page (or the keyboard) move through the app's screens.
// Nothing reads discs here: identify, rips and USB are the stubs of the UI tests.
#include "host.h"
#define main psx_main
#include "../../source/main.cpp"
#undef main
#include <signal.h>

static void disc(int c, const char *title, const char *rd, const char *serial, unsigned mb)
{
	Card &k = g_cards[c];
	k.present = 1; k.state = DISC_READY; k.tried = 1;   // already identified: no identify thread here
	snprintf(k.title, sizeof(k.title), "%s", title);
	snprintf(k.rd_name, sizeof(k.rd_name), "%s", rd);
	snprintf(k.serial, sizeof(k.serial), "%s", serial);
	k.mb = mb;
	snprintf(k.dest, sizeof(k.dest), "/dev_hdd0/PSXISO");
	snprintf(k.outdir, sizeof(k.outdir), "/dev_hdd0/PSXISO");
	name_from(g_name_pat, k.rd_name, k.title, k.serial, k.fname, sizeof(k.fname));
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);   // a browser that goes away: send() fails instead of ending the program
	if (argc > 1 && !strcmp(argv[1], "en")) host_lang = PS3_LANG_ENGLISH_US;
	static uint32_t fb[1920 * 1080];
	color_buffer[0] = color_buffer[1] = fb;
	ui_init();
	lang_init();
	for (int c = 0; c < NCARDS; c++) { ctx_init(c); card_defaults(g_cards[c]); g_cards[c].rd_state = -1; }
	host_usb[0] = HostUsb{1, 0, "Apple SuperDrive"};
	disc(0, "Final Fantasy IX (Disc 1)", "Final Fantasy IX (Europe) (Disc 1)", "SLES_029.65", 706);
	disc(1, "Croc 2", "Croc 2 (Europe) (En,Fr,De,Es,It,Nl,Sv)", "SLES_020.88", 654);
	select_card(0);

	g_remote = 1;
	static const RemoteHooks hooks = {status_json, NULL, remote_press};
	remote_init(&hooks);
	remote_set(1);
	char ip[32];
	remote_ip(ip, sizeof(ip));
	printf("remote view: http://%s:%d   (Ctrl-C ends)\n", ip, REMOTE_PORT);
	fflush(stdout);
	while (!exit_requested) {
		int a = home_screen();
		if (a == 0) break;   // the app's "exit": start the home screen again
	}
	return 0;
}
