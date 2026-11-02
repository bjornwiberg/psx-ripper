# Notes for AI coding assistants

Read [CONTRIBUTING.md](CONTRIBUTING.md) first; everything there applies. In addition:

- **No attribution.** Never add `Co-Authored-By`, "Generated with …", session links or any other
  mention of an AI assistant to commits, pull request descriptions, comments or code.
- **Commits** follow Conventional Commits (`feat:`, `fix:`, …); the title is what users read in
  the changelog. Work on a branch and open a pull request; `main` is protected.
- **Tests before every commit that touches the UI:** `make -C tests/host`. Add or update a
  scenario in `tests/host/ui_test.cpp` for a new or changed screen, and look at the frames in
  `tests/host/out/`.
- **Build:** `docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src ps3dev-psl1ght make pkg`
  (see the README for building the image).
- **Never install on a console while it rips.** `tools/deploy.sh` checks `/status.json` (`busy`)
  and refuses; do not work around it. Do not press the console's buttons (webMAN `pad.ps3` or the
  remote view's pad) while someone is using it.
- **UI:** the app's own UI only, plain words, right increases, ✕ opens, ○ goes back, L2/R2 page
  with arrows at the right edge, details in the small font, no overlapping text (the tests check).
  Every UI text exists in English and Swedish (`source/lang.cpp`).
- **Legal:** no copyrighted or protected material (disc images, `.sbi` files, Redump data, BIOS,
  Sony logos or button artwork), no code without a GPL-3.0-compatible license, no links to sites
  that distribute protected data.
