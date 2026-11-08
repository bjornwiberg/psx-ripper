# Security

## Reporting a vulnerability

Please report security problems privately with GitHub's
[private vulnerability reporting](https://github.com/bjornwiberg/psx-ripper/security/advisories/new), not in a public issue.
Include what you found, how to reproduce it, and the version (shown after the title, e.g. v1.0.0).

## What to know before you use it

- **Remote control has no authentication.** When the remote view is on, anyone who can reach
  port 8090 on the console can watch the screen and press the app's buttons (start or cancel
  rips, eject, change settings). Keep it on a trusted network; never forward the port to the
  internet. A web page elsewhere cannot press the buttons through your browser: the pad answers
  only requests from the console's own page or without an `Origin` (programs).
- **MCP has no authentication either.** It is off by default; while it is on, **[MCP]** is shown
  on every screen. Anyone who can reach port 8090 can then use it as an agent would: start and
  cancel rips, change settings, rename and trash games, and empty a trash for good (`empty_trash`,
  which needs `confirm`). Use it on your home network only and never forward port 8090. Requests
  from a web browser are refused unless they come from the console's own page
  (`http://<the console's address>:8090`), so a web page you visit cannot use MCP. The webhook
  address and headers can be set over MCP but never read. See [MCP](docs/mcp.md#security).
- **Webhooks are plain HTTP.** Headers you add (for example an authentication token) are sent
  unencrypted and are stored in `/dev_hdd0/tmp/psxrip.cfg`.
- **lv1 patch.** While ripping from the internal drive the app patches the lv1 storage manager
  in RAM (the method IRISMAN uses). It checks the code before patching, refuses on a mismatch,
  and restores it afterwards; a reboot also removes it. Do not kill the app while it rips.
