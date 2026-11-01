#!/bin/sh
# Installs psxripper.pkg on the PS3 via webMAN and starts it again.
# Refuses while a rip is running (remote view status.json says "ripping").
PS3=${PS3:-192.168.1.50}
cd "$(dirname "$0")/.." || exit 1
pad() { curl -s --max-time 8 "http://$PS3/pad.ps3?$1" >/dev/null; sleep 1; curl -s --max-time 8 "http://$PS3/pad.ps3?release" >/dev/null; curl -s --max-time 8 "http://$PS3/pad.ps3?off" >/dev/null; }
st=$(curl -s --max-time 8 "http://$PS3:8090/status.json")
case "$st" in *'"state":"ripping"'*) echo "a rip is running, not installing"; exit 2;; esac
old=$(curl -s --max-time 8 "ftp://$PS3/dev_hdd0/game/PSXR00001/USRDIR/" | grep EBOOT | awk '{print $6,$7,$8}')
curl -s --max-time 300 -T psxripper.pkg "ftp://$PS3/dev_hdd0/packages/psxripper.pkg" || { echo "upload failed"; exit 1; }
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
