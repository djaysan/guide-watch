# Guide Watch (Switch overlay)

Guide Watch is an Ultrahand overlay that shows a plain-text game walkthrough (GameFAQs-style `.txt`) over the game you are playing. It works out which game is running, opens the matching guide, and remembers where you stopped reading.

- File: `GuideWatch.ovl`, version 0.4.0, by Ludovic Verbeeck
- Needs: Atmosphère with Ultrahand (libultrahand overlay loader)
- Draws a 896x720 panel on the left 70 % of the screen, at the same sharpness as normal 448 px overlays. If the overlay heap is too small for that, it falls back to the standard 448 px width.

## Install

1. Copy `GuideWatch.ovl` to `/switch/.overlays/` on the SD card.
2. Put your guides anywhere under `/Guides/` as `.txt` files. Subfolders are fine and show up as a tag next to each guide, for example:
   - `/Guides/NS guides/The Legend of Zelda Links Awakening.txt` (tag "NS guides")
   - `/Guides/PS/Final Fantasy IX.txt` (tag "PS")
3. Open Ultrahand during a game and pick **Guide Watch**.

Guides can be CRLF or LF, UTF-8 or Latin-1 / Windows-1252. Files from 200 KB to about 1.5 MB are the expected size.

## How it picks a guide

Detection runs every time the overlay appears, not only when it starts. Overlays stay loaded while hidden, so if you hide Guide Watch, switch games and bring it back, it notices the new game, saves your place in the old guide and opens the right one (the `/Guides/` folder is scanned again too).

**A game is running:**

1. **Remembered pick.** If you chose a guide for this game before, it opens straight away.
2. **Name match.** Otherwise the game's name is compared with the guide file names. If exactly one guide is a strong match, it opens. Region tags like `(USA)`, words like "guide", "FAQ", "walkthrough", "the" and "a", punctuation and apostrophes are ignored, and roman numerals count as numbers. Sequels are kept apart: "Final Fantasy IX" never opens "Final Fantasy XII". A short title also matches a longer file name that contains all of its words (at least two words, same numbers): a "Twilight Princess" forwarder opens `The Legend of Zelda - Twilight Princess.txt`, and "Skyward Sword HD" still asks before opening `skyward.txt`.
3. **Picker.** If nothing (or more than one guide) matches strongly, you get a list: best matches first, then every guide. The header shows the game's name.

Whatever you pick in the list is remembered for that game, so next time it opens directly. Press **B** in the reader to get back to the list and choose another guide.

**melonDS is running** (as an installed title whose name contains "melonDS"): always the list, never a guide by itself. See [melonDS](#melonds).

**No game is running.** This is how homebrew started from hbmenu looks, for example melonDS or SwitchWaker launched from the album. The header says "No game detected" and you always get the list:

- **Last opened**: the guide you opened the last time no game was detected, preselected at the top. It is never opened by itself.
- **Recent DS ROMs (melonDS, as of its last exit)**: if `/switch/melonds/melonDS.ini` (or `/melonds/melonDS.ini`) exists, guides matching melonDS's five recent ROMs. This does not mean melonDS is running; it's a hint in case it is.
- **All guides.**

## Controls

| Where | Button | Action |
|---|---|---|
| Reader | D-pad Up / Down | One line (hold to repeat) |
| Reader | L / R | One page |
| Reader | ZL / ZR | Ten pages |
| Reader | Left stick | Continuous scroll, faster the further you push |
| Reader | Right stick up / down | Text size one step bigger / smaller (16, 18, 20, 22, 24, 26 px; hold to repeat) |
| Reader | Touch and drag | Scroll |
| Reader | B | Save position, back to the guide list |
| List | D-pad or stick Up / Down | Move (hold to repeat) |
| List | L / R | One page |
| List | A, or tap a guide | Open it |
| List | Touch and drag | Scroll the list |
| List | B | Close the overlay |
| Anywhere | Ultrahand combo | Hide the overlay (position is saved) |

The footer of the reader shows the guide name, `line X / Y` (lines of the original file) and how far through the guide you are. For 1.5 s after a size change it shows the new size (for example `18 px`) instead.

**Maps and tables.** Lines that look like ASCII art are drawn in fixed-width cells so hand-drawn maps and tables keep their columns: every character sits centred in a cell as wide as the letter "n", and spaces take one cell. A line counts as art when at least 30 % of its characters are symbols, when it contains box-drawing characters, or when it has two or more wide gaps between words (column tables). A text line sitting between two art lines (a label inside a map) counts as art too. Art lines are never wrapped. Each group of art lines gets the largest size from your text size, 20, 18, 16, 14 and 12 px (never bigger than your text size) at which its widest line fits, so an 80-column map comes out at about 17 px (16 or 18). Art is drawn in the normal text colour.

**Text size.** The default is 22 px. At 18 px, 79-column GameFAQs guides fit without wrapping on most lines, so long lines no longer leave short leftover lines under them. The size applies to every guide, is remembered across launches, and the reader stays on the same line of the guide when you change it.

Touching the screen outside the panel (the right 30 %) hides the overlay, like every libultrahand overlay.

## melonDS

Guide Watch reads `/switch/melonds/melonDS.ini` (or `/melonds/melonDS.ini`) and takes the five recent ROMs from `LastROMPath0` to `LastROMPath4`. Folder and file extension are stripped, so `sdmc:/roms/nds/Final Fantasy IX (USA).nds` is matched as "Final Fantasy IX". This happens in two cases:

- **melonDS runs as an installed title** (its name contains "melonDS"): the list header shows "melonDS: recent ROMs (as of its last exit)" and the ROM names. Guides matching them come first, and the top one is preselected.
- **No game is detected**: the matches appear under "Recent DS ROMs (melonDS, as of its last exit)", see above.

In melonDS mode nothing opens by itself and nothing is written to `last.ini`. melonDS is one title for every DS game, so a remembered pick would open your Pokemon guide while you play Zelda.

Limitation: melonDS only writes its recent-ROM list when it exits. The game you are playing right now is only in it if you played it in a previous session, so the suggestions are "your recent DS games", not necessarily the current one. Opening a guide automatically can come back once melonDS writes its ini when a ROM loads.

## Files it writes

All in `/config/guidewatch/` (created on first launch):

| File | Content |
|---|---|
| `last.ini` | `TITLEID=guide path`, the guide chosen for each game; key `0` is the last guide opened with no game running |
| `positions.ini` | `guide path=line`, where you stopped reading in each guide |
| `settings.ini` | `font=NN`, the reader text size (16 to 26, default 22). Written only when you change it. |
| `guidewatch.log` | One line per launch (title id, name, chosen guide, width, memory), one per game change noticed on show, and one per guide opened. Rotates to `guidewatch.old.log` at 64 KB. |

Delete a line from `last.ini` to get the picker back for a game.

## Build

You need Docker Desktop. The image `devkitpro/devkita64:latest` runs natively on Apple Silicon.

```sh
cd switch
./build.sh          # -> switch/GuideWatch.ovl
./build.sh clean
make test           # host unit tests (Mac clang++, no Docker): name matching, decoding, wrapping
```

`build.sh` fetches libultrahand at the pinned commit into `switch/libultrahand/` if it's missing. `make test` also loads and wraps any sample guides in `switch/guides/` (gitignored, never commit them).

Code:

- `source/main.cpp`: game detection, guide choice, framebuffer probe, picker and reader screens
- `source/match.hpp`: name normalising and scoring (no libnx, tested on the Mac)
- `source/text.hpp`: file load, decoding, line wrapping (no libnx, tested on the Mac)
- `tests/`: the host tests
- `SPIKE.md`: the framebuffer-width spike this overlay is based on

## Known limits

- **Maps are stretched and a bit squashed.** Art keeps the normal line spacing, so a map drawn smaller than your text size looks taller than drawn. Wide characters such as W, M and @ are wider than a cell and may touch their neighbours. Maps wider than about 120 columns don't fit even at 12 px and are cut off at the right edge. Guides that put prose in a column next to a map (Zyre's Wind Waker does this a lot) show that prose at the map's size, because the whole line is part of the map.
- **Two-column lists** with only one wide gap per line stay in the normal font and lose their alignment.
- **Pre-wrapped text.** GameFAQs lines are already about 79 characters, so at 22 px and above a line slightly too wide for the panel leaves a short leftover line under it. Lower the size to 18 px (right stick down) if that bothers you; paragraphs are not reflowed.
- **Whole file in memory.** A guide must fit in the overlay heap. A file that doesn't fit shows a red message with the sizes instead of crashing. With an 8 MB heap, 1.5 MB guides are fine; with 4 MB they may not be.
- **hbmenu title takeover.** Homebrew started by holding R on a game is reported as that game, so it gets that game's guide.
- **melonDS** recent-ROM list is from its previous exit, so melonDS never opens a guide by itself (see above).
- **Same game, new files.** `/Guides/` is rescanned when the game changes. Files added while the same game keeps running show up the next time the overlay is started from the Ultrahand menu.
- **Untested on hardware.** Built and unit-tested on the Mac only.
