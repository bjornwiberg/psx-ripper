# MCP phase 1 (app control) — implementation plan

Steps use checkboxes (`- [ ]`). Branch `feat/mcp-app-control` from `main` (audit log merged),
one commit per task, one pull request (`feat: MCP server for app control`). Built in parallel with
the game list (`feat/game-list`); both touch the settings screen — the later of the two rebases.

**Goal:** an MCP server at `http://<ps3>:8090/mcp` through which an agent can do everything the pad
can on the app's own screens, under the same rules, see what the screen shows, read and change
every setting with its allowed values, and read the app's log — plus the "Remote control" menu and
the [MCP] indicator.

**Architecture:** `source/mcp.cpp` / `include/mcp.h`: a JSON-RPC 2.0 handler (string in, string
out) with a small own JSON reader/writer, the tool table and argument checking; it calls the app
through a hook table (like `RemoteHooks`), so it is host-testable without `main.cpp`. `remote.cpp`
routes `POST /mcp` to it (also when other parts are off, but only when MCP is on). Anything that
changes the screen is a request queued for the UI thread (next to `g_remote_btn`); the HTTP thread
waits for it to be done (bounded) and returns the screen state afterwards. Actions share their
precondition checks with the pad handlers (one function per action, used by both).

**Tech stack:** C++ (gnu++14), PSL1GHT, the app's web server, host tests in `tests/host`.

**Spec:** `docs/design/2026-11-03-mcp-design.md` (Intent, Goals, Anti-goals, Constraints,
"Decided by the user", Phase 1, Protocol and connection).

## Global constraints

- Every UI text in English and Swedish; new `StrId`s before `STR_COUNT`. Code, comments, logs in English.
- No AI attribution anywhere. Conventional Commits.
- `make -C tests/host` before every commit touching the UI; scenarios for new screens; look at frames.
- PS3 build without warnings; the package stays well under 1 MB.
- MCP off by default. Nothing from `/dbg` is reachable over MCP. No secret value (webhook address,
  headers) is ever returned; the agent sees only whether one is set.
- Every change MCP makes goes through the audit log with source `mcp` (AU_MCP).
- A rip must not slow down because MCP is on (handler work is short; the HTTP threads stay at low
  priority).

## Decisions this plan fixes

- **Remote control menu:** the settings row "Remote view" becomes "Remote control" /
  "Fjärrstyrning"; X opens a menu with three on/off rows: "Remote view" / "Fjärrvisning" (stream
  and pad page), "MCP", "status.json". Config keys: `remote` (remote view), `mcp`, `status_json`.
  Migration: an existing `remote=1` turns on remote view and status.json; `mcp` defaults to 0. The
  web server runs when any of the three is on; each path answers only when its switch is on
  (`/mcp` when off: a JSON-RPC error saying MCP is off and how to turn it on).
- **Indicator:** "[MCP]" in the small font, top left, on every screen while MCP is on.
- **Transport:** `POST /mcp`, body one JSON-RPC request (batches refused), response
  `application/json`; methods `initialize` (protocolVersion `2025-06-18`, serverInfo
  `{name:"psx-ripper", version:APP_VERSION}`, capabilities `{tools:{}}`), `notifications/*`
  (HTTP 202, no body), `ping`, `tools/list`, `tools/call`. Requests over 16 KB refused.
- **Two ways to answer:** reading tools never touch the screen; only tools whose name starts with
  `show_`/`go_`/`press_` or actions change it, and those return the screen state afterwards.
- **Tools (names fixed):**
  - `get_state` → screen state: `screen` (home, settings, drive_settings, filename, webhooks,
    webhook_headers, about, audit_log, remote_control, dialog), selected tab (drive) and row,
    open dialog text and buttons, and per drive the same fields as status.json.
  - `get_screenshot` → MCP image content (`image/jpeg`, base64) of the canvas.
  - `get_drives` → status.json's drives.
  - `list_settings` → every setting: `key`, `label` (en and sv), `value`, `values` (allowed, each
    with en/sv labels), `scope` (`standard` or a drive), `secret` (true → value omitted, `is_set`).
    Free-text settings (name pattern, webhook address/headers) have `values: null`.
  - `set_setting {key, value, drive?}` → same validation and side effects as the screens
    (`cfg_apply_line` + the screen's side effects + `cfg_save` with source mcp); secrets refused
    for reading but may be set (logged as set/cleared). Returns the new value.
  - `go_screen {screen}` (home, settings, drive_settings {drive}, filename, webhooks, about,
    audit_log, remote_control) → state.
  - `go_tab {drive}` (`internal`, `usb0`, `usb1`, …, by port as the tabs show) → state.
  - `press_button {button}` (cross, circle, triangle, square, up, down, left, right, start, select,
    l1, r1, l2, r2) → state.
  - `rip {drive}`, `repair {drive}`, `check {drive}`, `cancel {drive}`, `skip_rereads {drive}`,
    `eject {drive}` → same preconditions as the pad; refused with the app's own reason text (en),
    otherwise the state.
  - `read_log {from_line?, max_lines? (≤ 500)}` → lines of `psxrip.log` with the next line number.
- **Rules:** each action is one function `can_<action>(card, reason)` + `do_<action>(card, src)`
  used by the pad handler and MCP alike; MCP never bypasses a dialog the pad would show — where
  the pad asks yes/no (cancel a rip), the MCP call itself is the confirmation and is logged.
- **UI requests:** a small queue (`g_ui_req`) the home screen loop and the other screens' loops
  take each frame (like `pad_pressed`); a request carries the action and its arguments; the
  HTTP thread waits up to 5 s for "done" and answers with the state then (or "busy" if the UI did
  not take it, e.g. while a system dialog is open).
- **Config:** `mcp=0/1`, `status_json=0/1` in `cfg_apply_line`/`cfg_text` (so they are audited).

## Review focus

1. A tool called while a system dialog or the on-screen keyboard is open: answers "busy" with
   what is open, never blocks the HTTP thread longer than 5 s, never presses into the system
   dialog. Test with a stub that never takes the request.
2. Malformed or hostile JSON (deep nesting, huge strings, wrong types, missing id): a JSON-RPC
   error, no crash, no overflow. Fuzz-style tests over truncated/garbled inputs.
3. Two MCP clients at once (two connections): requests are serialised; state stays consistent.
4. A setting change over MCP while the settings screen is open: the screen shows the new value
   (it reloads its copies) and nothing is saved twice.
5. Secrets: no tool output contains the webhook address or a header value (grep the outputs in
   tests after setting known values).

---

### Task 1: JSON-RPC core and tool table (module)

**Files:** create `include/mcp.h`, `source/mcp.cpp`, `tests/host/mcp_test.cpp`; `tests/host/Makefile`
(build/run `mcp_test`; link `mcp.cpp` into ui_test and remote_sim).

**Interfaces (produces):**
```c
struct McpHooks {   // filled by main.cpp; every function writes JSON (a value) into out
	int (*state)(char *out, int n);
	int (*screenshot_jpeg)(unsigned char **jpg, int *len);   // caller frees
	int (*drives)(char *out, int n);
	int (*list_settings)(char *out, int n);
	int (*set_setting)(const char *key, const char *value, const char *drive, char *out, int n);
	int (*ui)(const char *tool, const char *arg, char *out, int n);   // go_*, press_button, actions
	int (*read_log)(int from, int max, char *out, int n);
};
void mcp_init(const McpHooks *hooks, const char *version);
// One HTTP request body in, the response body out; returns the HTTP status (200, 202, 400, 413).
int mcp_handle(const char *body, int len, char *out, int n);
```
- [ ] Tests: initialize/ping/tools/list shapes; tools/call routing with stub hooks for every tool
  name and argument (missing/wrong-typed args → JSON-RPC error with the field); notifications →
  202; batch → error; >16 KB → 413; garbled/truncated/deep inputs never crash (a loop over cut
  points of valid requests and random bytes); the screenshot as image content.
- [ ] Implement (own minimal JSON reader: objects, arrays, strings with escapes, numbers, bools,
  null, depth limit 16), pass, PS3 build clean, commit `feat: MCP request handling and tools`.

### Task 2: Remote control menu, switches and the [MCP] indicator

**Files:** `source/main.cpp` (settings row rename, `remote_control_screen()`, config keys and
migration, indicator in `draw_backdrop`), `source/remote.cpp` (`POST /mcp` route; each path checks
its switch; server runs when any is on), `include/remote.h`, `include/lang.h`, `source/lang.cpp`,
`tests/host/ui_test.cpp`.
- [ ] Scenarios (sv/en): settings with "Remote control", the menu, indicator on/off; migration
  tests (`remote=1` → view+status.json on, mcp off); paths answer only when on (host simulator or
  a handler-level test).
- [ ] Implement, pass, frames looked at, PS3 build clean, commit `feat: remote control menu with MCP and status.json switches`.

### Task 3: state, drives, screenshot, log, settings tools (reading and setting)

**Files:** `source/main.cpp` (the hooks: `mcp_state`, `mcp_drives`, `mcp_screenshot` via the remote
view's encoder, `mcp_read_log`, `mcp_list_settings` / `mcp_set_setting` using a settings table that
the screens and MCP share: key, labels, allowed values with labels, scope, secret), `source/remote.cpp`
(expose the JPEG encoder), tests.
- [ ] Tests: list_settings covers every config key the screens offer with correct values/labels;
  set_setting validates like the screens, applies side effects, writes an audit row with src mcp,
  refuses unknown keys/values; secrets never appear in any output; the open settings screen shows a
  value changed over MCP; read_log paging.
- [ ] Implement, pass, PS3 build clean, commit `feat: MCP tools for state, screenshot, log and settings`.

### Task 4: screen and action tools through the UI thread

**Files:** `source/main.cpp` (the UI request queue, taken by every screen loop; `go_screen`,
`go_tab`, `press_button`; `can_*`/`do_*` for rip, repair, check, cancel, skip_rereads, eject shared
with the pad handlers; audit source mcp), tests.
- [ ] Tests: each tool's effect on the screen state; refusals with the pad's reasons (eject while
  ripping, rip on an empty drive); busy when the UI does not take the request (stub) within 5 s;
  the pad handlers still behave as before (existing scenarios unchanged); two clients serialised.
- [ ] Implement, pass, PS3 build clean, commit `feat: MCP tools that drive the screen and the jobs`.

### Task 5: docs, console check, pull request

**Files:** new `docs/mcp.md` (connecting, e.g. `claude mcp add --transport http psx-ripper
http://<ps3>:8090/mcp`, every tool with arguments and an example, the rules, security notes),
`docs/configuration.md` (keys), `docs/user-guide.md` (Remote control menu), `README.md`.
- [ ] Docs. Console check (controller, with the user): connect Claude Code, get_state, screenshot,
  show the settings on the TV, change and undo a setting, go to a tab, refused eject while ripping.
- [ ] Commit `docs: MCP`, push, open the pull request.
