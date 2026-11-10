# CD audio offset correction — plan

Steps use checkboxes (`- [ ]`). One branch and one pull request per phase. Phase 0 only measures:
nothing in it changes a rip.

**Goal:** a rip of a disc with CD audio tracks matches Redump when every sector reads, as data-only
discs already do. "Bit-perfect" can then be claimed for every rip that matches Redump, with or
without audio.

**Why rips with audio do not match today.** A drive returns CD audio shifted by a fixed number of
samples (its *read offset*, one value per drive model), and every disc was mastered with a shift of
its own (its *write offset*, on Redump's disc page, for example −647 for Aqua GT (Europe)). The two
add up to the *combined offset*. Redump stores audio with both removed. PSX RiPPER reads audio as
the drive returns it, so every audio track's MD5 differs, even though the music sounds the same.
Data sectors are not affected: they are read whole, and the address in each sector's header is
checked.

**What the console shows (2026-11-10).** Each track's MD5 of the multi-track rips without errors,
compared with Redump's DAT: 12 have a data track that matches. Of those, 5 match on every track
(all with a single audio track) and 7 have audio tracks that do not match (Tactics Ogre, Brave
Fencer Musashi, Rollcage, Darkstone, Lucky Luke, Dead or Alive; Lunar: Silver Star Story Complete
disc 2 matches one of its two). The right data with the wrong audio is what an offset gives. The
other multi-track rips have a data track that does not match either: another version of the
disc, which no offset changes.

**Terms.** One sample is 4 bytes (16-bit stereo); a sector holds 588 samples (2352 bytes).

## Phase 0 — measure (no change to rips)

The figures the rest depends on, measured on the PS3's drive and an Apple USB SuperDrive.

- [ ] **Read past a track's end.** Correcting an offset needs up to a few hundred samples beyond
      the first and last sector of the audio. Can each drive read the lead-out (`READ CD` past
      `g_leadout`)? If it cannot, Redump's own rule applies: the missing samples are zero, which is
      almost always right, as discs end in silence.
- [ ] **Read a data sector as CD-DA.** `READ CD` with sector type CD-DA (`0xBE`, byte 1 = `0x04`)
      on the last sectors of the data track. If a drive returns them *scrambled* (as raw audio),
      the 12-byte sync pattern can be found in the audio stream, and its position gives the
      combined offset for any disc, without Redump (as DiscImageCreator does at the data/audio
      transition). If a drive refuses, phase 2's fallback applies.
- [ ] **Each drive's read offset.** A diagnostic command (a flag file, as `psxrip.diag`) that
      takes an existing rip of a disc Redump lists and searches the shift, in samples (±6000), that
      makes every audio track's MD5 equal to Redump's. That shift is the combined offset; minus
      the disc's write offset from Redump's page, it gives the drive's read offset. Done on 3 or
      more discs per drive model, the result must be the same each time.
- [ ] Write down the results in this plan: read offset per drive model, whether the lead-out and
      scrambled reads work.

## Phase 1 — correct rips of discs in Redump

- [ ] Each drive model's read offset as a constant (from phase 0), shown in the rip log
      (`read_offset=`).
- [ ] The disc's write offset: from the sync pattern at the data/audio transition if phase 0
      showed that works; else, for a disc Redump lists, the shift that matches Redump's track
      MD5s (searched around the drive's read offset, as in phase 0).
- [ ] Shift the audio stream by the combined offset when the image is written: from the first
      audio track's pregap to the end of the disc, with the extra samples read before and after.
      Track boundaries stay where the TOC puts them; the data track is unchanged.
- [ ] Log `write_offset=` and `combined_offset=` in the rip log, and in the summary when it is
      not 0.
- [ ] Host tests: a synthetic audio stream shifted by known offsets, with and without readable
      lead-out samples (zeros), gives Redump's bytes back.

## Phase 2 — discs Redump does not list

- [ ] Without the sync method (phase 0), the write offset of an unlisted disc cannot be found.
      The rip is then made with the drive's read offset removed and write offset 0, and the
      rip log says so (`write_offset=unknown`). Such a rip never claims to match Redump.

## Phase 3 — existing rips

- [ ] Rips made before phase 1 are uncorrected. When the samples the shift needs beyond the
      audio are silence (zero), a rip can be re-aligned from its own file, without the disc:
      a new action, through the audit log (undoable), followed by a Redump check. Otherwise the
      disc has to be ripped again.

## Phase 4 — wording

- [ ] With phase 1 in place and tested on both drives, README, site and FAQ can say that rips
      which match Redump are bit-perfect for audio too; the FAQ's fourth reason goes.

## Out of scope, noted

- **C2 error pointers.** The app does not request them. Audio has no checksum, so without C2 only
  a Redump match proves an audio track. Whether the PS3's drive (through the lv1 storage
  interface) and the SuperDrive return C2 pointers with `READ CD` is unknown; worth a probe in
  phase 0 as well, but correction does not depend on it.
