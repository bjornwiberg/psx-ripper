# Third-party notices

PSX Ripper is licensed under the GPL-3.0 (see [LICENSE](LICENSE)). It includes or uses the
following third-party work.

| Component | Where | License |
|---|---|---|
| [FatFs](http://elm-chan.org/fsw/ff/) by ChaN (exFAT/FAT on USB drives) | `fatfs/` | FatFs license (1-clause BSD style), see [`fatfs/LICENSE.txt`](fatfs/LICENSE.txt) |
| [stb_image_write](https://github.com/nothings/stb) by Sean Barrett (JPEG for the remote view) | `include/stb_image_write.h` | Public domain or MIT, at your choice (see the end of the file) |
| [DejaVu fonts](https://dejavu-fonts.github.io/) (rendered into `source/font.inc` by `tools/gen_font.py`) | `fonts/` | Bitstream Vera / DejaVu license, see [`fonts/DejaVu-LICENSE`](fonts/DejaVu-LICENSE) |
| [GameDB-PSX](https://github.com/niemasd/GameDB-PSX) by Niema Moshiri (serial → title, rendered into `source/titles.inc` by `tools/gen_titles.py`) | `db/PSX.titles.json` | GPL-3.0 |
| LibCrypt serial list (serial numbers only, no protection data) | `db/libcrypt_serials.txt` | Facts; compiled for this project |
| [PSL1GHT](https://github.com/ps3dev/PSL1GHT) and its portlibs (zlib), linked into the app | build toolchain | MIT (PSL1GHT), zlib license (zlib) |

Not included: Redump data (downloaded by the console at runtime, from [redump.org](http://redump.org/)),
`.sbi` files (created from the user's own disc), and any Sony code, firmware or artwork.

The app's own icons (disc, drive, USB, medal, warning and the button symbols) are drawn in code
in `source/ui.cpp`; the XMB icon and background (`pkgfiles/ICON0.PNG`, `pkgfiles/PIC1.PNG`) are
rendered by `tools/make_icon.py` and `tools/make_background.py`.
