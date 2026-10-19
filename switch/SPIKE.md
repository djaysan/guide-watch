# Switch overlay framebuffer-width spike

`guides-spike.ovl` is a throwaway libultrahand overlay. It answers two questions before we build the real Guide Watch reader on Switch:

1. Which framebuffer widths actually come up on screen with each nx-ovlloader heap size (4 / 6 / 8 MB)?
2. Does a wider framebuffer cover more of the screen at the same sharpness, or does it just get stretched or clipped?

Nothing here has run on hardware yet. Everything under "Findings" comes from reading the source and checking it on the host. The test matrix is for Ludovic to fill in.

---

## 1. Install and test (Ludovic)

### Files

| What | Where on the SD card |
|---|---|
| `switch/guides-spike.ovl` | `/switch/.overlays/guides-spike.ovl` |
| width setting (optional) | `/config/guides-spike/width.txt` (one number, e.g. `896`) |
| log (written by the overlay) | `/config/guides-spike/spike.log` |

You don't need to create the `/config/guides-spike/` folder yourself. The overlay creates it the first time it runs.

### Setting the width

You have two options:

- **On the console (easiest):** open the spike, then press **D-pad Left/Right**. Each press cycles 448 → 896 → 1280 → (widest width that fits) and writes the value to `width.txt` straight away. Close with **B** and launch it again to apply the new width. The yellow line at the top shows "Next launch width".
- **From a computer:** create `/config/guides-spike/width.txt` with a single number in it. Rules:
  - If the file is missing, empty or not a number, the width is 448.
  - The number is clamped to 256..1280.
  - It is then rounded down to a multiple of 32 (see the alignment section below).

### Setting the nx-ovlloader heap (4 / 6 / 8 MB)

1. Open Ultrahand (default combo ZL + ZR + D-pad Down).
2. Go to **Ultrahand Settings → System → Overlay Memory** and pick 4 MB, 6 MB or 8 MB.
3. Back out. Ultrahand writes `/config/nx-ovlloader/heap_size.bin`, which is a little-endian u64 byte count, and reloads the loader.
4. Check the `heap_size.bin:` line in the spike to confirm the change took effect.

Things to know about the heap setting:

- Ultrahand refuses to raise the heap ("Not enough memory.") if the system doesn't have the new size plus 5 MB of RAM free. If that happens, close the running game and try again.
- If `heap_size.bin` is missing, the loader uses a default that depends on the firmware: 4 MB on HOS 21+, 6 MB on 20.x and 8 MB on older versions. This follows libultrahand's `getDefaultHeapSize()`.

### Test matrix

Use handheld mode for all nine cells. For each cell:

1. Set the heap.
2. Set the width.
3. Launch "Guides Spike" from Ultrahand.
4. Photograph the screen.
5. Fill in the cell.

| Heap | 448 | 896 | 1280 | widest fit (value shown on screen) |
|---|---|---|---|---|
| 4 MB | | | | |
| 6 MB | | | | |
| 8 MB | | | | |

Hardware result 2026-10-18: 448, 896 and 1280 all drew correctly (heap believed 8 MB, setting could not be changed from Ultrahand). Target width chosen: 896.

What to write in each cell:

- **UP:** the overlay drew at the width you asked for.
- **FELL BACK:** red "FB create: FAIL" line, drawn at 448.
- **FATAL:** Atmosphère error screen. Note the error code.
- **CLIPPED** or **STRETCHED**, if you see either (see below).

### What to photograph

- **The whole screen, straight on, with all four edges in frame.** That one photo answers the coverage question:
  - **White 2 px border:** should be visible on all four sides of the overlay area.
  - **Orange bar on the right edge:** if it's missing, the right side is clipped.
  - **Cyan X:** both diagonals should end exactly in the overlay's corners.
  - **Yellow ruler along the top:** numbers every 128 framebuffer pixels. On a correct 896-wide run, the overlay ends at 70 % of the screen width (896 of 1280 physical pixels in handheld) and text looks just as sharp as at 448. If the 896 run fills the whole screen, or text looks wider or blurrier than at 448, it was stretched.
- **A close-up of the stats block**, so all numbers can be read. The lines are:
  - framebuffer
  - layer
  - heap_size.bin
  - before init
  - now
  - svcGetInfo
- **Optional, docked:** one photo at 1280 on the TV, to check underscan handling.
- **If you hit a fatal error:** photograph the error screen, including the error code and program ID.

After the run, copy `/config/guides-spike/spike.log` off the SD card and paste it into the findings. There is one line per launch:

- The first half is written before the renderer starts.
- `| UP ...` is appended after the first frame is drawn.
- `| NO FIRST FRAME (crashed in init?)` is appended on the next launch if the previous one never drew a frame.

---

## 2. Findings from libultrahand and libnx

The source checked was libultrahand `2e4df54` (2026-08-16) and libnx 4.12.0. `framebuffer.c` has not changed between libnx v4.12.0 and the current master.

### How the width flows into the layer

The defaults live in `libultra/source/tsl_utils.cpp:65-66`:

```cpp
u16 DefaultFramebufferWidth = 448;            ///< Width of the framebuffer
u16 DefaultFramebufferHeight = 720;           ///< Height of the framebuffer
```

The constants are in `libtesla/include/tesla.hpp:363-366`. VI layer coordinates are always in 1920x1080 space:

```cpp
constexpr u32 ScreenWidth = 1920;       ///< Width of the Screen
constexpr u32 ScreenHeight = 1080;      ///< Height of the Screen
constexpr u32 LayerMaxWidth = 1280;
constexpr u32 LayerMaxHeight = 720;
```

`Renderer::init()` is at `tesla.hpp:5168-5183`:

```cpp
cfg::LayerPosX = 0;
cfg::LayerPosY = 0;
cfg::FramebufferWidth  = ult::DefaultFramebufferWidth;
cfg::FramebufferHeight = ult::DefaultFramebufferHeight;
offsetWidthVar = (((cfg::FramebufferWidth / 2) >> 4) << 3);
ult::correctFrameSize = (cfg::FramebufferWidth == 448 && cfg::FramebufferHeight == 720);
...
const float divW = ult::windowedLayerPixelPerfect ? float(cfg::ScreenWidth)  : float(cfg::LayerMaxWidth);
const float divH = ult::windowedLayerPixelPerfect ? float(cfg::ScreenHeight) : float(cfg::LayerMaxHeight);
cfg::LayerWidth  = cfg::ScreenWidth  * (float(cfg::FramebufferWidth)  / divW);
cfg::LayerHeight = cfg::ScreenHeight * (float(cfg::FramebufferHeight) / divH);
```

Then, at `tesla.hpp:5274, 5297-5300`:

```cpp
ASSERT_FATAL(viSetLayerScalingMode(&this->m_layer, ViScalingMode_FitToLayer));
...
ASSERT_FATAL(viSetLayerSize(&this->m_layer, cfg::LayerWidth, cfg::LayerHeight));
ASSERT_FATAL(viSetLayerPosition(&this->m_layer, cfg::LayerPosX, cfg::LayerPosY));
ASSERT_FATAL(nwindowCreateFromLayer(&this->m_window, &this->m_layer));
ASSERT_FATAL(framebufferCreate(&this->m_framebuffer, &this->m_window, cfg::FramebufferWidth, cfg::FramebufferHeight, PIXEL_FORMAT_RGBA_4444, 2));
```

What this means:

- **The layer scales with the framebuffer.** `LayerWidth = 1.5 × FramebufferWidth` (in VI units), so the scale factor never changes. A wider framebuffer covers a proportionally wider part of the screen at the same sharpness:
  - 448 → 672 VI units, which is 35 % of the screen
  - 896 → 1344 VI units (70 %)
  - 1280 → 1920 VI units, the full width
  - In handheld that is 1 framebuffer pixel = 1 panel pixel.
- **Nothing else needs to change for the layer to get wider.** The only requirement is to set `ult::DefaultFramebufferWidth/Height` before `Renderer::init()`. `tsl::loop` runs `new TOverlay()` → `parseOverlaySettings()` → `initServices()` → `initScreen()` (`tesla.hpp:16128-16138`), so setting the values in `main()` before `tsl::loop` works. That is what the spike does, and Status-Monitor-Overlay does the same for its 1280x720 modes.
- **Wide layers anchor to the left.** The layer is anchored at x = 0. Ultrahand's right-alignment setting only applies to 448x720 (`if (ult::useRightAlignment && ult::correctFrameSize)`, `tesla.hpp:5192`).
- **Limits:**
  - Width must be ≤ 1280. Anything wider makes `LayerWidth` > 1920, `viSetLayerSize` fails, and the console shows a fatal error.
  - Height must be ≤ 720 for the same reason vertically. A portrait framebuffer is therefore impossible with this math.
- **A failed `framebufferCreate` is fatal.** It is wrapped in `ASSERT_FATAL`, so running out of heap gives an Atmosphère fatal screen, not an error code. That's why the spike test-allocates the exact buffer first and falls back to 448.
- **Supporting evidence:** Status-Monitor-Overlay (same library) already ships 1280x720 framebuffers on 6 MB+ heaps. On a 4 MB heap it falls back to 448x720 with the comment "setting 1280x720 on a 4MB heap crashes the overlay loader" (`Status-Monitor-Overlay/source/main.cpp:1200-1204`). In limited-memory mode its micro bar uses 1280x360.

### Alignment rule: width must be a multiple of 32 px

libnx `framebufferCreate` (`nx/source/display/framebuffer.c:78-84`):

```c
const u32 width_aligned_bytes = (width*bytes_per_pixel + 63) &~ 63; // GOBs are 64 bytes wide
const u32 width_aligned = width_aligned_bytes / bytes_per_pixel;
const u32 height_aligned = (height + block_height - 1) &~ (block_height - 1);   // block_height = 128
const u32 fb_size = width_aligned_bytes*height_aligned;
const u32 buf_size = (num_fbs*fb_size + 0xFFF) &~ 0xFFF; // needs to be page aligned
fb->buf = __libnx_aligned_alloc(0x1000, buf_size);
```

libtesla writes pixels with its own block-linear swizzle (`tesla.hpp:5152`):

```cpp
return ((((y & 127) >> 4) + ((x >> 5) << 3) + ((y >> 7) * offsetWidthVar)) << 9) + ...
```

The two disagree unless the width is a multiple of 32:

- libnx rounds the row stride **up** to whole 64-byte GOBs, i.e. 32 px at 2 bytes per pixel.
- libtesla's `offsetWidthVar = floor(W/32) * 8` rounds **down**.
- So for a width that isn't a multiple of 32, every row from 128 down is drawn at the wrong offset (sheared).

I checked this on the host by comparing libtesla's formula with the block-linear layout libnx allocates, for every pixel of a W×720 frame:

- 448, 480, 896, 1024, 1152 and 1280 match exactly.
- 464 has 274,688 misplaced pixels, which is every pixel below row 128.
- 912 and 1000 are also wrong.

**Rule: width % 32 == 0, 256 ≤ width ≤ 1280, height ≤ 720.** The spike rounds down to a multiple of 32.

### Memory cost

At height 720 (rounded up to 768 rows) with two RGBA4444 buffers, the buffer costs **3072 bytes per pixel of width**, i.e. 96 KB per 32 px:

| Width | Buffer | Layer (VI units) | Screen coverage |
|---|---|---|---|
| 448 | 1,344 KB | 672 x 1080 | 35 % |
| 896 | 2,688 KB | 1344 x 1080 | 70 % |
| 1024 | 3,072 KB | 1536 x 1080 | 80 % |
| 1152 | 3,456 KB | 1728 x 1080 | 90 % |
| 1280 | 3,840 KB | 1920 x 1080 | 100 % |

Predictions, which the hardware run should confirm or refute:

- **1280 does not fit in a 4 MB heap.** 3,840 KB leaves 256 KB for everything else. The spike reports FELL BACK.
- **1280 fits in 6 MB and 8 MB.**
- **896 fits in all three heaps.**
- **The real 4 MB ceiling** is whatever the "widest FB that fit" line shows. The spike keeps 256 KB of headroom (`kHeadroom`) free after the framebuffer, for glyph cache, GUI, thread stacks and sounds. The "Now: malloc used" line shows the real overhead, so that guess can be tuned.

Lowering the height doesn't help: 720 and 768 cost the same, because libnx rounds height up to 128-row blocks.

### What a real wide guide overlay must change

1. **Set the size in `main()` before `tsl::loop`.** Width must be a multiple of 32 and ≤ 1280; height ≤ 720. Choose the width from a heap test (like this spike's probe), not from the heap setting alone, because a wrong guess is a fatal error rather than a recoverable one.
2. **Plan for 4 MB heaps.** 4 MB is the default on HOS 21+, and full width doesn't fit there. Options:
   - Ask users to set Overlay Memory to 6 MB or more.
   - Fall back to the widest width that fits, probably around 1100-1200.
   - Use 1280 at a reduced height. Status Monitor uses 1280x360 in limited-memory mode.
   - Patch libultrahand to a single buffer: change `2` to `1` in the `framebufferCreate` call (`tesla.hpp:5300`). This halves the cost, so 1280 → 1,920 KB, but the screen will probably tear.
3. **Don't build on the stock 448-centric widgets without an audit.** Several are hardcoded to 448:
   - `drawEdgeSeparator` draws at x = 447 (`tesla.hpp:5503`).
   - The wallpaper fast path and right alignment only work when `correctFrameSize` is true (448x720).
   - Notifications are `NOTIF_WIDTH = 448`.

   The spike uses its own root `Element` and draws everything with `cfg::FramebufferWidth/Height`. The real reader should do the same, or the stock widgets need checking first.
4. **Touch:** a touch with `x > FramebufferWidth + layerEdge` hides the overlay (`tesla.hpp:14149`). At 896, tapping the right 30 % of the screen closes the reader. At 1280 the whole screen belongs to the overlay.
5. **Rotated mode (later):** a portrait framebuffer is ruled out by the 720 height cap. Two routes:
   - Keep the landscape 1280x720 framebuffer and draw rotated text yourself with a transposed glyph blit, since `drawString` cannot rotate.
   - Test `nwindowSetTransform(&m_window, HAL_TRANSFORM_ROT_90)` (libnx). `m_window` is private in `Renderer` (`tesla.hpp:5068`), so this needs a small libultrahand patch, and it's unknown whether vi honours buffer transforms on this layer. That would be its own spike.
6. **Leave `ult::windowedLayerPixelPerfect` at false.** When it's true, the layer maps 1:1 to 1080p units. That gives a sharper picture docked, but a 1280 framebuffer then covers only 2/3 of the screen. A full-width 1080p framebuffer (1920 x 1152 rows x 2 buffers) would need about 8.4 MB, which doesn't fit even in an 8 MB heap.

---

## 3. Build

You need Docker Desktop. The image `devkitpro/devkita64:latest` contains devkitA64 r29.2, GCC 15.2 and libnx 4.12.0, and runs natively on Apple Silicon.

```sh
cd switch
./build.sh          # -> switch/guides-spike.ovl
./build.sh clean
```

How `build.sh` works:

- It fetches libultrahand into `switch/libultrahand/` at the pinned commit (`2e4df5430045…`) if the folder is missing. The folder is gitignored, not vendored.
- It runs `make` in the container. If the image doesn't run natively, it retries with `--platform linux/amd64`.

How the Makefile is set up:

- It's copied from Ultrahand-Overlay and trimmed down.
- `include libultrahand/ultrahand.mk` adds libultrahand's sources and includes.
- Flags: `-std=c++26 -fno-exceptions -fno-rtti`, plus LTO.
- `elf2nro` builds the overlay with `--nacp` only (no icon), and the Makefile appends the `ULTR` signature.
- No libultrahand `-D` directives are needed:
  - Leave out `IS_LAUNCHER_DIRECTIVE`. It's only for Ultrahand itself.
  - Widget, logging and exception-wrap directives are left off to keep the build small and simple.

Portlibs:

- Nothing extra had to be installed.
- `switch-minizip` no longer exists as a separate package. The minizip headers ship inside `switch-zlib`, which is already in the image.
- `libultra/download_funcs.hpp` needs the curl, zlib and minizip headers to compile, but only `-lnx` is linked, as Status-Monitor-Overlay does. LTO and `--gc-sections` drop the unused download code.

Output: `switch/guides-spike.ovl`, 462,908 bytes. Build intermediates (`build/`, `.elf`, `.nacp`) are gitignored.

Code: `switch/source/main.cpp` (single file) and `switch/Makefile`.
