<p align="center">
  <img src="docs/images/banner.png" alt="Guide Watch: in-game walkthroughs on the handheld itself">
</p>

<p align="center">
  <a href="https://github.com/djaysan/guide-watch/releases/latest"><img src="https://img.shields.io/github/v/release/djaysan/guide-watch?style=flat-square&color=58c274&labelColor=17130e" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/device-Miyoo%20Mini%20Plus-57a4ff?style=flat-square&labelColor=17130e" alt="Miyoo Mini Plus">
  <img src="https://img.shields.io/badge/device-Nintendo%20Switch-e4000f?style=flat-square&labelColor=17130e" alt="Nintendo Switch">
  <img src="https://img.shields.io/badge/firmware-OnionOS-a06bf5?style=flat-square&labelColor=17130e" alt="OnionOS">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-ffc93c?style=flat-square&labelColor=17130e" alt="MIT license"></a>
</p>

---

Stuck on a puzzle, a boss, a missable item. You put the handheld down and reach for your phone. **Guide Watch** keeps the walkthrough on the device instead: press a combo while you play and the guide opens over the game, press again and you are back exactly where you paused.

It started as a rebuild of the "in-game guide" feature from cbepx-me's Anbernic StockOS mod and now exists for several consoles. Each one is a separate build with its own install steps, linked below.

## Supported consoles

| Console | What you get | Status | Read more |
|---|---|---|---|
| **Miyoo Mini / Mini Plus** on OnionOS | Rumble cue when a guide exists, L2 + R2 then Down opens it, MENU drops you back in. Plus an on-device downloader that fetches guides over wifi. | Released, [download](https://github.com/djaysan/guide-watch/releases/latest/download/GuideWatch-OnionOS.zip) | [Install and details](#miyoo-mini-plus-onionos), [onion/README.md](onion/README.md) |
| **Nintendo Switch** with Atmosphère + Ultrahand | A wide overlay over any game: detects the running title, opens its guide, remembers your place, text size on the right stick, maps and tables kept on a grid. | Released, [download](https://github.com/djaysan/guide-watch/releases/latest/download/GuideWatch.ovl) | [Install and details](#nintendo-switch), [switch/README.md](switch/README.md) |
| **Nintendo 3DS / 2DS** with Luma3DS | A plugin that opens the walkthrough over any 3DS game with L + R + Select, page remembered per game. | Released, separate repo | [djaysan/3ds-ingame-guides](https://github.com/djaysan/3ds-ingame-guides) |
| **Nintendo DS** with a DSpico flashcart | An in-game guide reader built into the DSpico loader. | Released, separate repo | [djaysan/pico-loader, release v1.0.0-guides](https://github.com/djaysan/pico-loader/releases/tag/v1.0.0-guides) |
| **Anbernic H700** on stock firmware | The original Real Time Game Guide feature, by cbepx-me, not by this project. | Third party | cbepx-me's StockOS modification |
| **TrimUI Brick / Smart Pro, spruceOS, Allium** | Planned ports, each as its own folder here. | Not started | [Other handhelds](#other-handhelds) |

The guides themselves are plain text FAQs from the GameFAQs archive on archive.org, see [Where the guides come from](#where-the-guides-come-from). The browser tool at [guides.retromodlab.com](https://guides.retromodlab.com) fills a card for any of these consoles from a computer.

## Miyoo Mini Plus, OnionOS

It is the Anbernic mod's idea rebuilt for OnionOS, plus something that mod never had: an app that downloads the guides for your games on the device itself, over wifi.

### Download

Grab **`GuideWatch-OnionOS.zip`** from the [latest release](https://github.com/djaysan/guide-watch/releases/latest/download/GuideWatch-OnionOS.zip), unzip it, and copy the two folders onto your SD card, merging them with what is already there:

| From the zip | Onto the card |
|---|---|
| `App/GuideWatch/` | `SDCARD/App/GuideWatch/` |
| `App/GuideDownload/` | `SDCARD/App/GuideDownload/` |
| `.tmp_update/startup/guidewatch.sh` | `SDCARD/.tmp_update/startup/guidewatch.sh` |

Reboot. That is the whole install: no patching, nothing overwritten, delete the folders to uninstall.

> On Windows the `.tmp_update` folder is hidden by default. Turn on hidden files, or drag `App/` first and copy the startup script separately.

<p align="center">
  <img src="docs/images/how-it-works.png" alt="How it works: launch a game, press L2+R2 then Down, read the guide, press MENU to return">
</p>

### Two apps, one package

**Guide Watch** is the daemon. It starts with the console, notices when a game you have a guide for is running, and handles the rumble cue, the button combo and the return trip.

**Guide Downloader** is how the guides get there. Open it from the Apps menu with wifi on, and it scans your `Roms` folders, counts what is missing, and lets you fetch guides for every console at once or just one at a time. Guides land next to each ROM as `Game Name.txt`, which is exactly where Guide Watch looks for them.

The two ship together because they are two halves of the same thing, and because keeping one download link means one thing to update.

### What you need

- A **Miyoo Mini Plus** running **OnionOS** (any version with the `.tmp_update/startup` folder, which is Onion 4.2 and later).
- The **Ebook Reader** app, installed from Onion's own Package Manager. Guide Watch shows guides through it, and it is what remembers your reading position per guide.
- Wifi, for the downloader only. Guide Watch itself is entirely offline.
- Onion's **Auto save** setting on, which is the default. That is what makes the return trip land you exactly where you were.

RetroArch games are supported. Standalone emulators like DraStic are not.

### Notes from the device

Three things the Miyoo taught me, written down for anyone porting this to another handheld:

1. **Nothing can be drawn over a paused RetroArch.** The frozen game frame stays composited on top, so the first version ran the reader invisibly behind it. Guide Watch instead saves and quits the game, shows the guide, then relaunches with the save state, using the same quick-switch mechanism Onion's own Game List Options uses.
2. **Onion deletes `cmd_to_run.sh` after every program exits** unless `/tmp/quick_switch` exists. Without that flag, a handoff lands on the main menu instead of your guide.
3. **The busybox `awk` on this device fails on bracket-expression regexes**, silently returning an empty string. String work here is done with shell built-ins for that reason.

### Build from source

The daemon is a single C file, cross compiled in the same Docker toolchain image OnionOS itself uses:

```sh
git clone https://github.com/djaysan/guide-watch.git
cd guide-watch/onion
./build.sh
```

It runs the host self-test first, then produces a static ARM binary at `sd-package/App/GuideWatch/guidewatch`, leaving `sd-package/` ready to copy onto a card. The downloader is plain shell and needs no build step.

More detail, including the on-device test checklist, is in [onion/README.md](onion/README.md).

## Where the guides come from

Walkthroughs are plain text FAQs from the GameFAQs archive hosted on [archive.org](https://archive.org/details/Gamespot_Gamefaqs_TXTs). On OnionOS the downloader matches your ROM names against an index and pulls each guide directly from archive.org to your card.

**This repository contains no guide files, and neither does the index.** Nothing is rehosted. If you would rather do this from a computer, the browser version is at [guides.retromodlab.com](https://guides.retromodlab.com) and writes the same `Game Name.txt` files to the same places.


## Nintendo Switch

The Switch version is an **Ultrahand overlay**: open Ultrahand during any game and the walkthrough slides in over the left 70 % of the screen, wider than a normal overlay strip, at the text size you pick with the right stick. It reads which title is running, opens the matching guide on its own, remembers your pick per game and reopens at the line where you stopped. Hand-drawn maps and tables are laid out on a fixed grid so they keep their shape.

1. Download **`GuideWatch.ovl`** from the [latest release](https://github.com/djaysan/guide-watch/releases/latest/download/GuideWatch.ovl) and copy it to `/switch/.overlays/` on the SD card.
2. Put your guides as `.txt` files anywhere under `/Guides/`, subfolders welcome.
3. In a game, open Ultrahand and pick **Guide Watch**.

Needs Atmosphère with Ultrahand. Details, controls, how matching works and the build steps are in [`switch/README.md`](switch/README.md).

## Other handhelds

OnionOS and the Switch are done here, the 3DS and DS versions live in their own repos (table above). Next in line are the TrimUI Brick and Smart Pro on stock firmware, spruceOS (A30, Flip, Brick) and Allium on the Miyoo Flip, each as its own folder in this repo. If you want one of those next, open an issue and say which device you have, or cast a vote on [guides.retromodlab.com](https://guides.retromodlab.com).

## Credits

- [OnionUI](https://github.com/OnionUI/Onion) for the firmware this is built on, and for a source tree clear enough to learn its internals from.
- [pixel-reader](https://github.com/ealang/pixel-reader) by ealang, the Ebook Reader that displays the guides.
- cbepx-me's Anbernic StockOS mod, whose Real Time Game Guide feature is the original idea.
- The FAQ authors who wrote these walkthroughs, some of them thirty years ago, and the archive.org volunteers who keep them readable.

## License

MIT, see [LICENSE](LICENSE).

---

Guide Watch is free. If it saved you from reaching for your phone mid dungeon, you can buy me a coffee:

[<img src="https://storage.ko-fi.com/cdn/kofi2.png?v=6" alt="Buy Me a Coffee at ko-fi.com" height="36">](https://ko-fi.com/H2H81I6YY1)
