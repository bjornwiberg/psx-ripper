# Security

## Reporting a vulnerability

Please report security problems privately with GitHub's
[private vulnerability reporting](https://github.com/bjornwiberg/psx-ripper/security/advisories/new), not in a public issue.
Include what you found, how to reproduce it, and the version (shown after the title, e.g. v1.0.0).

## What to know before you use it

- **The remote view has no authentication.** When it is on, anyone who can reach port 8090 on
  the console can watch the screen and press the app's buttons (start or cancel rips, eject,
  change settings). Keep it on a trusted network; never forward the port to the internet.
- **Webhooks are plain HTTP.** Headers you add (for example an authentication token) are sent
  unencrypted and are stored in `/dev_hdd0/tmp/psxrip.cfg`.
- **lv1 patch.** While ripping from the internal drive the app patches the lv1 storage manager
  in RAM (the method IRISMAN uses). It checks the code before patching, refuses on a mismatch,
  and restores it afterwards; a reboot also removes it. Do not kill the app while it rips.
