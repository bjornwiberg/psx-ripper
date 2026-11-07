# MCP list_settings (written by ui_test to out/mcp-settings.json): every setting the screens
# offer, in the standard scope and for each drive, each with its labels in both languages, its
# value among the allowed ones, and the secrets without a value.
# Usage: python3 check_settings.py out/mcp-settings.json internal usb0 usb2
import json
import sys

d = json.load(open(sys.argv[1], encoding="utf-8"))
drives = sys.argv[2:]
events = ["disc_identified", "rip_started", "rip_finished", "rip_cancelled", "rip_failed", "repair_finished",
          "disc_ejected", "disc_not_identified", "app_started", "disc_lost"]
standard = ["speed", "retries", "check", "eject", "autostart", "name_pattern", "redump", "remote", "mcp",
            "status_json", "webhook_on", "webhook", "webhook_headers_on", "webhook_header1", "webhook_header2",
            "webhook_header3"] + ["webhook_event_" + e for e in events] + ["webhook_progress", "audit_mb"]
per_drive = ["speed", "retries", "check", "eject", "autostart", "destination"]
secret = {"webhook", "webhook_header1", "webhook_header2", "webhook_header3"}
free_text = {"name_pattern"} | secret

bad = []
want = [(k, "standard") for k in standard] + [(k, dr) for dr in drives for k in per_drive]
got = [(s.get("key"), s.get("scope")) for s in d["settings"]]
for w in want:
    if w not in got:
        bad.append("missing %s (%s)" % w)
for g in got:
    if g not in want:
        bad.append("unexpected %s (%s)" % g)
if len(got) != len(set(got)):
    bad.append("a setting is listed twice")
for s in d["settings"]:
    k = "%s (%s)" % (s.get("key"), s.get("scope"))
    lab = s.get("label") or {}
    if not lab.get("en") or not lab.get("sv"):
        bad.append(k + ": label without en or sv")
    if s.get("secret") is not (s["key"] in secret):
        bad.append(k + ": secret should be %s" % (s["key"] in secret))
    if s["key"] in secret:
        if "value" in s or not isinstance(s.get("is_set"), bool):
            bad.append(k + ": a secret shows its value or has no is_set")
    elif not isinstance(s.get("value"), str):
        bad.append(k + ": no value")
    if s["key"] in free_text:
        if s.get("values", 0) is not None:
            bad.append(k + ": free text should have values null")
        continue
    vals = s.get("values")
    if not isinstance(vals, list) or len(vals) < (1 if s["key"] == "destination" else 2):
        bad.append(k + ": no list of allowed values")
        continue
    for v in vals:
        if not isinstance(v.get("value"), str) or not v.get("label", {}).get("en") or not v.get("label", {}).get("sv"):
            bad.append(k + ": a value without its text or en/sv label")
    if s["value"] not in [v["value"] for v in vals]:
        bad.append(k + ": value %r is not an allowed one" % s["value"])
    if s["scope"] != "standard" and s["key"] != "destination" and vals[0]["value"] != "-1":
        bad.append(k + ": a drive's setting should start with -1 (standard)")
for b in bad:
    print("  FAIL list_settings: " + b)
sys.exit(1 if bad else 0)
