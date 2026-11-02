# FAQ

### Which consoles and firmwares does it work on?

It is developed and tested on Evilnat 4.93 with Cobra. It needs PS3MAPI for the lv1 patch while
ripping from the internal drive, so other custom firmwares with Cobra and PS3MAPI should work;
they are not tested, and neither is PS3HEN.

### Can it rip PS2 or PS3 discs, or music CDs?

No. Only PlayStation 1 discs. A disc that is not a PS1 disc shows *Not PS1*.

### Is the lv1 patch safe?

It is the patch IRISMAN uses for the same purpose. PSX RiPPER checks the code before it patches
and refuses on a mismatch, keeps a backup, and restores the original as soon as the drive is no
longer needed. The patch is only in RAM; a reboot removes it too. Do not force-quit the app while
it rips.

### Why does my rip not match Redump?

There are three common reasons:

1. **Sectors could not be read.** The tab shows the warning and *Known errors*. Clean the disc
   and use **Repair**, or try another drive.
2. **The disc is a copy.** A burned disc (CD-R) is never identical to the original, even when
   every sector on it reads perfectly: it was written from an image made with some other tool,
   and burning often adds or removes the two-second pregap. Such a rip has no unreadable sectors
   but a different MD5, and often a size that differs from Redump's by exactly 150 sectors
   (2 seconds) or by a few sectors. You can tell a copy by its underside: original PS1 discs are
   black, burned discs are blue, green, purple or silver.
3. **The disc is a version Redump does not list**, for example another print or revision of
   the same game.

A rip that does not match can still play perfectly. PSX RiPPER checks every data sector while it
reads (sync, address, EDC and ECC), so a rip without known errors is a correct copy of the disc
in the drive; it is only not the same as the disc Redump has.

### What does "Double-checking sectors without a checksum" mean, and why does it take so long?

Video and sound on a PS1 disc are stored in sectors without a checksum, so the drive cannot tell
whether it read them right. In the *Bit-perfect* mode the app reads all of them once more and
compares; a sector that differs is read until three reads agree. Discs with a lot of video take
longer. Set *Sector check* to *Check and re-read* to skip this step.

### What does Repair do?

It re-reads only the sectors the rip log lists as unreadable and writes the ones it gets into the
image, then checks against Redump again. It never touches sectors that were read fine, so it
cannot make a rip worse.

### Will automatic start overwrite my rips?

No. With *Rip immediately when a disc is inserted* on, a disc that is not ripped to the
destination yet is ripped, one that is ripped with known errors is repaired, and one that is
fine is left alone. Only **✕ New rip** rips over an existing rip.

### Which USB drives work?

The Apple USB SuperDrive (USB ID 05AC:1500). The PS3 does not pass commands to USB optical drives
on its own, so PSX RiPPER has its own driver, written for that drive. Other USB drives are not
supported yet.

### Do I need a Y-cable or external power for the SuperDrive?

No. It runs on the PS3's USB power. It can take up to a minute to spin up.

### Where do the rips go, and how do I play them?

By default to `/dev_hdd0/PSXISO/`, where webMAN looks for PS1 games. Mount the `.cue` with webMAN
and start it from the XMB. See [Playing your rips](user-guide.md#playing-your-rips).

### Can I save to a USB drive?

Yes, to FAT32 and exFAT drives: **← →** on the tab switches the destination. NTFS is not
supported yet.

### What about LibCrypt (PAL) games?

For the PAL discs known to use LibCrypt, the protection data is read from the disc's subchannel
and saved as `/dev_hdd0/tmp/sbi/<SERIAL>.sbi`; Cobra uses it when the image is mounted. It only
works with an original disc: a burned copy has no such data. PSX RiPPER does not ship or
download `.sbi` files.

### Does it need the internet?

No. Redump verification can be *Off*, *Online*, or use a *Local copy* that is downloaded once.
No Redump data ships with the app.

### Can I use it from my phone?

Turn on **Remote view** and open `http://<the PS3's address>:8090/`: you see the screen and get a
pad that presses the console's buttons. It has no password, so keep it on your home network.

### Can I get a notification when a rip is done?

Yes, with **Webhooks**: the app sends an HTTP POST for each event, for example to Home Assistant,
which can notify your phone. See [configuration](configuration.md#remote-view-and-webhooks).

### The SuperDrive tab says "No answer".

The drive stopped answering commands. Press **△ Eject**; if that does not help, unplug its USB
cable and plug it in again. A disc still inside is found again after that.

### I changed the configuration file but nothing happened.

The app reads `/dev_hdd0/tmp/psxrip.cfg` only when it starts. Edit it while the app is closed,
or restart the app afterwards.

### How do I report a bug?

Open an [issue](../../../issues/new/choose) with the version (after the title), the firmware,
the drive, the disc, and the relevant part of `/dev_hdd0/tmp/psxrip.log` (remove webhook
addresses and headers first). Please never attach disc images or `.sbi` files.
