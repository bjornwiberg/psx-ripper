# Contributing to PSX RiPPER

Thanks for helping. Bug reports, tested discs, translations and code are all welcome.

## Ground rules

- **Only free, redistributable code and data.** Everything in this repository must be
  compatible with the GPL-3.0. Do not add code copied from proprietary SDKs, firmware dumps,
  or anything whose license you cannot name.
- **No copyrighted or protected material.** No BIOS files, disc images, `.sbi` files, Redump
  data, or Sony logos and button artwork. The app draws its own icons; keep it that way.
  Redump data is only ever fetched at runtime by the user's console.
- **No links to sites that distribute protected data.**
- Be kind. See the [Code of Conduct](CODE_OF_CONDUCT.md).

## Development setup

You need Docker (for the PS3 toolchain) and a C++ compiler (for the UI tests).

```sh
docker build --platform linux/amd64 -t ps3dev-psl1ght docker/
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src ps3dev-psl1ght make pkg
make -C tests/host
```

To install on a console with webMAN MOD (FTP and its web server on, the app's remote view on):

```sh
PS3=192.168.1.50 tools/deploy.sh     # or write the address into .ps3host
```

`deploy.sh` refuses while the app is ripping: closing it then can leave the lv1 patch on.

## Tests

`make -C tests/host` builds the app's real `main.cpp` for the host against stubs of the PS3
libraries (`tests/host/include`, `tests/host/stubs.cpp`), draws every scenario in
`tests/host/ui_test.cpp` and writes each frame to `tests/host/out/*.png`. Every frame is checked
for overlapping text, text off the screen or outside the content area, and characters the font
does not have; a self-test makes sure the checks find planted problems.

When you change a screen, add or update a scenario and look at the frames. CI runs the same
tests on every pull request and keeps the frames as an artifact.

### Remote view without a console

`make -C tests/host remote` runs the app's screens together with the real web server
(`source/remote.cpp`) on this computer and prints the address, e.g. `http://192.168.1.20:8090`.
Open it in a browser or on a phone on the same network: the stream, `/status.json` and the pad
work as on the console (the pad moves through the app's screens). Two drives have discs; nothing
reads discs or rips. `make -C tests/host remote LANG_ARG=en` shows the English texts.

## Code style

- Code, comments and log messages in English; the UI texts are in `source/lang.cpp` (English and
  Swedish, one entry per `StrId` in `include/lang.h`, in the same order in both tables).
- Comments say why, not what. Keep the existing formatting (tabs, braces on the same line).
- UI conventions: the app's own UI everywhere (no system dialogs for normal flow), plain words,
  ← → change a value (right increases), ✕ opens a sub-view, ○ goes back, L2/R2 page or scroll
  with arrows at the right edge when there is more. Details go in the small font.

## Commits and pull requests

`main` is protected: changes go through pull requests, which are **squash-merged**. The pull
request title becomes the commit on `main`, so it must follow
[Conventional Commits](https://www.conventionalcommits.org/):

| Title | Release |
|---|---|
| `fix: ...` | patch (1.0.0 → 1.0.1) |
| `feat: ...` | minor (1.0.0 → 1.1.0) |
| `feat!: ...` or a `BREAKING CHANGE:` footer | major (1.0.0 → 2.0.0) |
| `docs:`, `test:`, `refactor:`, `perf:`, `build:`, `ci:`, `chore:` | no release on their own (`perf` and `docs` appear in the changelog) |

Write the title for users: it ends up in the changelog. A check on every pull request rejects
titles that do not follow the format.

## Releases

Releases are automatic ([release-please](https://github.com/googleapis/release-please)):

1. Every merge to `main` updates an open pull request named `chore: release x.y.z` with the next
   version (in `version.txt`) and the changelog (`CHANGELOG.md`).
2. Merging that pull request tags `vx.y.z`, creates the GitHub release with the changelog, and
   attaches `psxripper-x.y.z.pkg` and its SHA-256, built by CI from the tag.

The version shows in the app's title, its log, `/status.json` and the package
(`APP_VER` in PARAM.SFO is `MM.mm`, the major and minor version; the PS3 has no field for the patch).
