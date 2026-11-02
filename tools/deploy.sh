#!/bin/sh
# Installs psxripper.pkg on the PS3 via webMAN and starts it again.
# Refuses while the app uses the drive (remote view status.json says "ripping" or "busy":true):
# killing it then leaves the lv1 patch on and the install hangs the console.
# The console's address: $PS3, or the first line of .ps3host in the repository (not in git).
cd "$(dirname "$0")/.." || exit 1
[ -n "$PS3" ] || PS3=$(head -n1 .ps3host 2>/dev/null)
[ -n "$PS3" ] || { echo "set PS3=<the console's address> or write it into .ps3host"; exit 1; }
pad() { curl -s --max-time 8 "http://$PS3/pad.ps3?$1" >/dev/null; sleep 1; curl -s --max-time 8 "http://$PS3/pad.ps3?release" >/dev/null; curl -s --max-time 8 "http://$PS3/pad.ps3?off" >/dev/null; }
st=$(curl -s --max-time 8 "http://$PS3:8090/status.json")
case "$st" in *'"state":"ripping"'*|*'"busy":true'*) echo "the app is using the drive (rip, repair, identify or lv1 patch on), not installing"; exit 2;; esac
old=$(curl -s --max-time 8 "ftp://$PS3/dev_hdd0/game/PSXR00001/USRDIR/" | grep EBOOT | awk '{print $6,$7,$8}')
curl -s --max-time 300 -T psxripper.pkg "ftp://$PS3/dev_hdd0/packages/psxripper.pkg" || { echo "upload failed"; exit 1; }
# again right before closing the app: a rip may have started during the upload
st=$(curl -s --max-time 8 "http://$PS3:8090/status.json")
case "$st" in *'"state":"ripping"'*|*'"busy":true'*) echo "a rip started during the upload, not installing (package left in /dev_hdd0/packages)"; exit 2;; esac
curl -s --max-time 10 "http://$PS3/xmb.ps3\$exit" >/dev/null
ok=0; for i in $(seq 1 30); do sleep 2; r=$(curl -s --max-time 5 "http://$PS3/xmb.ps3\$screenshot?fast"); [ -n "$r" ] && ! echo "$r" | grep -q "Not in XMB" && { ok=1; break; }; done
[ $ok = 1 ] || { echo "not back in XMB"; exit 1; }
sleep 12   # installing right after the exit gives "No installable package"
curl -s --max-time 30 "http://$PS3/install_ps3/dev_hdd0/packages/psxripper.pkg" >/dev/null
new=""; for i in $(seq 1 30); do sleep 3; e=$(curl -s --max-time 5 "ftp://$PS3/dev_hdd0/game/PSXR00001/USRDIR/" | grep EBOOT | awk '{print $6,$7,$8}'); [ -n "$e" ] && [ "$e" != "$old" ] && { new=$e; break; }; done
[ -n "$new" ] || { echo "EBOOT unchanged"; exit 1; }
sleep 8; pad circle; sleep 5   # close the "installation completed" dialog
curl -s --max-time 10 "http://$PS3/play.ps3?PSXR00001" >/dev/null
echo "installed ($new) and started"
