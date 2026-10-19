// GuideWatch.ovl : Guide Watch, a plain-text game walkthrough over the running game.
// libultrahand overlay for Ultrahand. Replaces the framebuffer-width spike (SPIKE.md).
//
// Flow:
//   main()     -> detect the running title (pm + ns), scan /Guides for .txt files, choose
//                 a guide (last.ini, then a unique strong name match, else the picker;
//                 melonDS and "no game" always get the picker), probe-allocate the
//                 896x720 framebuffer (fallback 448), log the launch, then tsl::loop.
//   onShow()   -> the overlay stays resident while hidden: re-detect, and if the game
//                 changed, rebuild the GUI stack for the new game.
//   PickerGui  -> last opened / suggestions first, all guides below; A opens, the pick
//                 goes to last.ini.
//   ReaderGui  -> the whole guide in memory, wrapped to the framebuffer width; the scroll
//                 position goes to positions.ini on B, on hide and on exit. The right
//                 stick steps the text size (16..26 px, saved in settings.ini).

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <dirent.h>
#include <malloc.h>     // mallinfo, memalign
#include <strings.h>    // strcasecmp
#include <sys/stat.h>   // mkdir, stat
#include <cinttypes>
#include <cstdarg>
#include <ctime>

#include "match.hpp"
#include "text.hpp"

// libnx sets these to the heap nx-ovlloader handed us (see libnx runtime/init.c)
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

namespace {

  // ---- Paths ------------------------------------------------------------------------
  constexpr const char* kGuidesDir = "/Guides";
  constexpr const char* kStateDir  = "/config/guidewatch";
  constexpr const char* kLastPath  = "/config/guidewatch/last.ini";       // title id -> guide path
  constexpr const char* kPosPath   = "/config/guidewatch/positions.ini";  // guide path -> source line
  constexpr const char* kSettings  = "/config/guidewatch/settings.ini";   // font=NN
  constexpr const char* kLogPath   = "/config/guidewatch/guidewatch.log";
  constexpr const char* kOldLog    = "/config/guidewatch/guidewatch.old.log";
  constexpr const char* kMelonIni[] = {"/switch/melonds/melonDS.ini", "/melonds/melonDS.ini"};

  // ---- Framebuffer (numbers and reasons in SPIKE.md) ----------------------------------
  constexpr u32 kWantWidth     = 896;         // 70 % of the screen, drew fine on hardware 2026-10-18
  constexpr u32 kFallbackWidth = 448;         // libultrahand's default, fits any heap
  constexpr u32 kHeight        = 720;         // LayerHeight = 1080 * H / 720 must stay <= 1080
  constexpr size_t kHeadroom   = 256 * 1024;  // heap that must stay free after the FB / the guide
  static_assert(kWantWidth % 32 == 0, "libtesla's swizzle needs the width to be a multiple of 32");

  // ---- Layout -----------------------------------------------------------------------
  // Every font size is a separate set in libtesla's ~500-glyph cache. On screen at once:
  // the reader size + kSmall + the cell size of the art
  // blocks in view (usually one or two), or kListFont + kSmall in the picker. The cache
  // is cleared on a zoom step.
  constexpr u32 kSizes[]     = {16, 18, 20, 22, 24, 26};  // reader text sizes (right stick)
  constexpr u32 kDefaultFont = 22;
  u32 g_font = kDefaultFont;                                // current reader size, settings.ini
  s32 rowH() { return s32(g_font) + 6; }                    // reader line pitch (22 -> 28)
  s32 baseline() { return s32(g_font) - 1; }                // drawString's y is the baseline (22 -> 21)

  constexpr u32 kListFont   = 22;  // picker rows and title, red messages (not zoomed)
  constexpr u32 kSmall      = 17;  // footers, tags, header context
  constexpr s32 kMsgRowH    = 28;  // red message pitch / baseline at kListFont
  constexpr s32 kMsgBase    = 21;
  constexpr s32 kPickRowH   = 36;  // picker row pitch
  constexpr s32 kMargin     = 14;
  constexpr s32 kTop        = 10;
  constexpr s32 kFooterH    = 44;

  // Colours (RGBA4444)
  const tsl::Color kBg(0x0, 0x0, 0x0, 0xE);
  const tsl::Color kText(0xF, 0xF, 0xF, 0xF);
  const tsl::Color kDim(0x9, 0x9, 0xA, 0xF);      // tags, footer
  const tsl::Color kAccent(0x4, 0xC, 0xF, 0xF);   // title, section labels, scrollbar thumb
  const tsl::Color kSelect(0x2, 0x5, 0x9, 0xC);   // picker cursor
  const tsl::Color kRule(0x5, 0x5, 0x5, 0xF);     // separators, scrollbar track
  const tsl::Color kRed(0xF, 0x4, 0x4, 0xF);

  // ---- Log --------------------------------------------------------------------------
  // /config/guidewatch/guidewatch.log: one line per launch, one per guide opened.
  void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  void logf(const char* fmt, ...) {
    FILE* f = fopen(kLogPath, "a");
    if (!f) return;
    char ts[32] = "?";
    const time_t now = time(nullptr);
    if (const tm* t = localtime(&now)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    fprintf(f, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
  }

  // Start a fresh log once it passes 64 KB (the previous one is kept as .old.log)
  void rotateLog() {
    struct stat st;
    if (stat(kLogPath, &st) != 0 || st.st_size < 64 * 1024) return;
    remove(kOldLog);
    rename(kLogPath, kOldLog);
  }

  // Process memory as the kernel sees it, for the log
  void svcMemory(u64& total, u64& used) {
    total = used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  }

  // Binary search for the largest page-aligned block the heap can hand out right now
  size_t largestBlock() {
    size_t lo = 0, hi = size_t(fake_heap_end - fake_heap_start);
    while (hi - lo > 0x10000) {  // 64 KB resolution is plenty
      const size_t mid = ((lo + hi) / 2) & ~size_t(0xFFF);
      void* p = memalign(0x1000, mid);
      if (p) { free(p); lo = mid; } else { hi = mid; }
    }
    return lo;
  }

  // ---- key=value files ------------------------------------------------------------------
  // One "key=value" per line. last.ini splits at the first '=' (key = title id),
  // positions.ini at the last '=' (key = guide path, which may itself contain '=').
  using KV = std::vector<std::pair<std::string, std::string>>;

  KV readKV(const char* path, bool splitAtLast) {
    KV kv;
    FILE* f = fopen(path, "r");
    if (!f) return kv;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
      std::string l(buf);
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      const size_t eq = splitAtLast ? l.rfind('=') : l.find('=');
      if (eq == std::string::npos || eq == 0) continue;
      kv.push_back({l.substr(0, eq), l.substr(eq + 1)});
    }
    fclose(f);
    return kv;
  }

  void writeKV(const char* path, const KV& kv) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (const auto& [k, v] : kv) fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
    fclose(f);
  }

  std::string getKV(const KV& kv, const std::string& key) {
    for (const auto& [k, v] : kv)
      if (k == key) return v;
    return "";
  }

  // Read-modify-write one key (files are tiny, so rewriting them whole is fine)
  void putKV(const char* path, bool splitAtLast, const std::string& key, const std::string& value) {
    KV kv = readKV(path, splitAtLast);
    bool found = false;
    for (auto& [k, v] : kv)
      if (k == key) { v = value; found = true; }
    if (!found) kv.push_back({key, value});
    writeKV(path, kv);
  }

  std::string titleKey(u64 tid) {
    char b[20];
    snprintf(b, sizeof(b), "%016" PRIX64, tid);
    return b;
  }

  // ---- Launch context (filled by main() before tsl::loop) ------------------------------
  struct GuideFile {
    std::string path;  // /Guides/NS guides/skyward.txt
    std::string name;  // skyward
    std::string tag;   // NS guides  ("" for files directly in /Guides)
  };

  struct Launch {
    u64 titleId = 0;
    std::string title;               // NACP display name, "" when no application runs
    bool melon = false;              // the title is the melonDS port
    std::vector<std::string> roms;   // melonDS LastROMPath0..4 stems (melonDS or no game running)
    std::vector<GuideFile> guides;   // sorted by tag, then name
    int lastOpened = -1;             // no game running: guide opened last time (last.ini key "0")
    std::vector<int> best;           // picker suggestions (indices into guides)
    const char* bestLabel = "Best matches";
    std::string autoOpen;            // guide opened straight away ("" = picker)
    const char* reason = "picker";   // last.ini / match / picker
    u32 width = kFallbackWidth;
    bool probeOk = false;
  } g;

  // Title id + display name of the running application. Services are opened only here
  // and closed straight after (pmdmnt is already open: libtesla's __appInit uses it).
  void detectRunning() {
    u64 pid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) return;  // no game (HOME menu, or hbmenu via album)
    if (R_SUCCEEDED(pminfoInitialize())) {
      if (R_FAILED(pminfoGetProgramId(&g.titleId, pid))) g.titleId = 0;
      pminfoExit();
    }
    if (!g.titleId || R_FAILED(nsInitialize())) return;
    // NsApplicationControlData is NACP + icon = 144 KB: heap, never the stack
    auto* data = static_cast<NsApplicationControlData*>(malloc(sizeof(NsApplicationControlData)));
    u64 size = 0;
    if (data && R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, g.titleId, data, sizeof(*data), &size))) {
      for (const NacpLanguageEntry& e : data->nacp.lang) {  // first non-empty language entry
        const char* nul = static_cast<const char*>(memchr(e.name, 0, sizeof(e.name)));  // no strnlen in strict newlib
        const size_t n = nul ? size_t(nul - e.name) : sizeof(e.name);
        if (n) { g.title.assign(e.name, n); break; }
      }
    }
    free(data);
    nsExit();
  }

  // melonDS writes LastROMPath0..4 (most recent first) when it exits
  void readMelonRoms() {
    for (const char* ini : kMelonIni) {
      FILE* f = fopen(ini, "r");
      if (!f) continue;
      std::string roms[5];
      char line[1024];
      while (fgets(line, sizeof(line), f)) {
        // LastROMPath0=sdmc:/roms/nds/Game.nds
        if (strncmp(line, "LastROMPath", 11) != 0 || line[11] < '0' || line[11] > '4' || line[12] != '=') continue;
        std::string v(line + 13);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        roms[line[11] - '0'] = v;
      }
      fclose(f);
      for (const std::string& r : roms)
        if (!r.empty()) g.roms.push_back(match::stem(r));
      return;  // first ini that exists wins
    }
  }

  bool isTxt(const char* name) {
    const size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".txt") == 0;
  }

  // Recursive scan of /Guides: .txt files only, hidden and macOS "._" files skipped,
  // the subfolder path becomes the tag. Depth-limited in case of odd folder loops.
  void scanGuides(const std::string& dir, const std::string& tag, int depth) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      const std::string path = dir + "/" + e->d_name;
      bool isDir = e->d_type == DT_DIR;
      if (e->d_type == DT_UNKNOWN) {
        struct stat st;
        isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      }
      if (isDir) {
        if (depth < 4) scanGuides(path, tag.empty() ? e->d_name : tag + "/" + e->d_name, depth + 1);
      } else if (isTxt(e->d_name)) {
        g.guides.push_back({path, match::stem(e->d_name), tag});
      }
    }
    closedir(d);
  }

  // Fresh detection of what runs. With no application at all (hbmenu homebrew, which is
  // how melonDS is usually started) melonDS's ini is still read: its recent ROMs become
  // picker suggestions, without claiming melonDS is running.
  void detect() {
    g.titleId = 0;
    g.title.clear();
    g.roms.clear();
    detectRunning();
    g.melon = match::containsNoCase(g.title, "melonds");
    if (g.melon || !g.titleId) readMelonRoms();
  }

  // (Re)scan /Guides, sorted by tag, then name
  void scanLibrary() {
    g.guides.clear();
    scanGuides(kGuidesDir, "", 0);
    std::sort(g.guides.begin(), g.guides.end(), [](const GuideFile& a, const GuideFile& b) {
      const int t = strcasecmp(a.tag.c_str(), b.tag.c_str());
      return t ? t < 0 : strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
  }

  // last.ini key for a pick: the title id, or "0" when no game runs (never used in melonDS)
  std::string lastKey() { return g.titleId ? titleKey(g.titleId) : "0"; }

  // Sets autoOpen (a guide to open without asking) and the picker suggestions:
  //   game running: (a) last.ini for this title, (b) a unique strong name match, (c) picker
  //   melonDS:      always the picker, suggestions from its five recent ROMs
  //   no game:      always the picker, "Last opened" on top, then melonDS ROM suggestions
  void chooseGuide() {
    g.autoOpen.clear();
    g.reason = "picker";
    g.lastOpened = -1;
    std::vector<match::Tokens> names;
    if (g.melon || !g.titleId) {
      for (const std::string& r : g.roms) names.push_back(match::tokens(r));
    } else if (!g.title.empty()) {
      names.push_back(match::tokens(g.title));
    }
    std::vector<match::Tokens> library;
    for (const GuideFile& f : g.guides) library.push_back(match::tokens(f.name));
    g.best = match::bestMatches(names, library, 10);
    g.bestLabel = g.titleId ? "Best matches" : "Recent DS ROMs (melonDS, as of its last exit)";

    if (!g.titleId) {
      // Preselected at the top of the picker, never opened by itself
      const std::string last = getKV(readKV(kLastPath, false), lastKey());
      for (size_t i = 0; i < g.guides.size(); i++)
        if (g.guides[i].path == last) g.lastOpened = int(i);
      return;
    }
    // melonDS: never auto-open. It writes LastROMPath only when it exits, so the ROM
    // running now is unknown, and one per-title pick would reopen the same guide for
    // every DS game. Re-enable (a)/(b) once melonDS writes its ini when a ROM loads.
    if (g.melon) return;

    const std::string last = getKV(readKV(kLastPath, false), lastKey());
    struct stat st;
    if (!last.empty() && stat(last.c_str(), &st) == 0) {
      g.autoOpen = last;
      g.reason = "last.ini";
      return;
    }
    const int pick = match::strongPick(names, library);
    if (pick >= 0) {
      g.autoOpen = g.guides[pick].path;
      g.reason = "match";
    }
  }

  // Same math as libnx framebufferCreate() for RGBA4444 (2 bytes/px), 2 buffers:
  // stride aligned to 64 bytes, height aligned to 128 rows, total page aligned.
  size_t fbBufferBytes(u32 width, u32 height) {
    const size_t strideBytes = (width * 2 + 63) & ~size_t(63);
    const size_t rows = (height + 127) & ~size_t(127);
    return (2 * strideBytes * rows + 0xFFF) & ~size_t(0xFFF);
  }

  // libultrahand wraps framebufferCreate in ASSERT_FATAL, so running out of heap there
  // is an Atmosphere fatal. Allocate exactly that buffer (plus headroom) first and fall
  // back to 448 if it does not fit.
  void chooseWidth() {
    void* fb = memalign(0x1000, fbBufferBytes(kWantWidth, kHeight));
    if (fb) {
      void* spare = malloc(kHeadroom);
      g.probeOk = spare != nullptr;
      free(spare);
      free(fb);
    }
    g.width = g.probeOk ? kWantWidth : kFallbackWidth;
  }

  // ---- Input helpers ------------------------------------------------------------------
  u64 nowNs() { return armTicksToNs(armGetSystemTick()); }

  // Hold-to-repeat for one button: fires on the press, then after 300 ms every 70 ms,
  // and every 25 ms once held for 1.5 s. A button already held when the GUI appeared
  // (part of the launch combo) never fires until pressed again.
  struct Repeat {
    u64 pressed = 0, last = 0;
    bool fire(u64 down, u64 held, u64 key) {
      const u64 now = nowNs();
      if (down & key) { pressed = last = now; return true; }
      if (!(held & key) || !pressed) { pressed = 0; return false; }
      const u64 t = now - pressed;
      const u64 every = t > 1500000000ULL ? 25000000ULL : 70000000ULL;
      if (t < 300000000ULL || now - last < every) return false;
      last = now;
      return true;
    }
  };

  // Pixel advance of one glyph at `size`, computed exactly like Renderer::drawString:
  // same font choice (FontManager::selectFontForCharacter) and the same per-glyph
  // truncation of xAdvance * scale, so a wrapped line never overflows on screen. It
  // reads the metrics directly instead of getOrCreateGlyph() so wrapping does not
  // rasterize a bitmap for every distinct character or churn libtesla's glyph cache.
  s32 glyphAdvance(u32 cp, u32 size) {
    stbtt_fontinfo* font = tsl::gfx::FontManager::selectFontForCharacter(cp);
    if (!font) return 0;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(font, int(cp), &adv, &lsb);
    return s32(adv * stbtt_ScaleForPixelHeight(font, float(size)));
  }

  // Advance at the current reader size. Cached because wrapping asks for every character
  // of the guide; the cache empties itself when g_font changes.
  s32 advance(uint32_t cp) {
    static u32 size = 0;
    static s32 ascii[128];
    static std::unordered_map<u32, s32> other;
    if (size != g_font) {
      size = g_font;
      for (s32& a : ascii) a = -1;
      other.clear();
    }
    if (cp < 128) {
      if (ascii[cp] < 0) ascii[cp] = glyphAdvance(cp, size);
      return ascii[cp];
    }
    const auto it = other.find(cp);
    if (it != other.end()) return it->second;
    return other[cp] = glyphAdvance(cp, size);
  }

  // Width of one fixed cell for art lines at font size px: the advance of 'n'
  // (0.6 em if the font reports nothing). Cached per size; sizes never exceed 26.
  s32 cellWidth(int px) {
    static s32 cells[32] = {};
    if (px <= 0 || px >= 32) return px * 6 / 10;
    if (!cells[px]) {
      const s32 n = glyphAdvance('n', u32(px));
      cells[px] = n > 0 ? n : px * 6 / 10;
    }
    return cells[px];
  }

  // One art line in fixed cells: character k is drawn centred in the cell at
  // x0 + k * cell, spaces advance a cell and draw nothing, glyphs wider than the cell
  // (W, M, @) may touch their neighbours, and nothing is drawn past xEnd. Uses
  // Renderer::renderGlyph, the public per-glyph blit drawString itself calls, so there
  // is no one-character std::string and no per-call translation-cache lookup; the
  // centring advance comes from the glyph we already hold (same metric as advance()).
  void drawCells(tsl::gfx::Renderer* r, const char* s, size_t n, u32 size, s32 x0, s32 xEnd, s32 y,
                 const tsl::Color& color) {
    const s32 cell = cellWidth(int(size));
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    s32 x = x0;
    for (size_t i = 0; i < n && x < xEnd; x += cell) {
      uint32_t cp;
      i += text::decodeUtf8(p + i, cp);
      if (cp == ' ') continue;
      const auto glyph = tsl::gfx::FontManager::getOrCreateGlyph(cp, false, size);
      if (!glyph) continue;
      const s32 adv = s32(glyph->xAdvance * glyph->currFontSize);
      r->renderGlyph(glyph.get(), float(x + (cell - adv) / 2), float(y), color);
    }
  }

  // settings.ini: font=NN, only values from kSizes are accepted (anything else -> 22)
  void readSettings() {
    const u32 v = u32(strtoul(getKV(readKV(kSettings, false), "font").c_str(), nullptr, 10));
    for (u32 size : kSizes)
      if (size == v) g_font = v;
  }

  void openGuide(int guide);  // picker -> reader, defined after both GUIs

  // ---- Reader -------------------------------------------------------------------------
  // Draws the visible slice of the guide. All reading state lives here; ReaderGui feeds
  // it buttons, libtesla feeds it touch through onTouch().
  class ReaderView : public tsl::elm::Element {
  public:
    text::Doc doc;
    std::string name;                // footer title
    std::vector<std::string> error;  // red message shown instead of the text
    size_t top = 0;                  // first visible wrapped line
    size_t topSource = 0;            // its source line, cached on every scroll
    u64 zoomUntil = 0;               // footer shows "NN px" instead of the line counter until then

    ReaderView() { m_isItem = false; }

    void layout(u16 x, u16 y, u16 w, u16 h) override { setBoundaries(x, y, w, h); }

    static s32 rows() { return (s32(tsl::cfg::FramebufferHeight) - kTop - kFooterH) / rowH(); }
    size_t maxTop() const { return doc.count > size_t(rows()) ? doc.count - size_t(rows()) : 0; }

    void scrollTo(long long line) {
      const size_t t = size_t(std::clamp<long long>(line, 0, (long long)maxTop()));
      if (t == top) return;
      topSource = text::sourceLineAt(doc, top, topSource, t);
      top = t;
    }
    void scrollBy(long long delta) { scrollTo((long long)top + delta); }

    int percent() const { return maxTop() ? int(top * 100 / maxTop()) : 100; }

    void draw(tsl::gfx::Renderer* r) override {
      const s32 W = tsl::cfg::FramebufferWidth;
      const s32 H = tsl::cfg::FramebufferHeight;
      r->fillScreen(r->a(kBg));

      if (!error.empty()) {
        s32 y = kTop + kMsgBase;
        for (const std::string& l : error) {
          r->drawString(l, false, kMargin, y, kListFont, r->a(kRed));
          y += kMsgRowH;
        }
      } else {
        // Visible lines straight from the offset table: O(rows) per frame, whatever the guide size.
        // Art lines (maps, tables) are drawn in fixed cells at their block's size, in the
        // normal text colour, clipped at the text width when even 12 px is too wide.
        // ponytail: art keeps the reader's row pitch, so maps drawn at a smaller size
        // look vertically stretched; a per-block pitch is the upgrade.
        const s32 n = rows();
        const s32 textEnd = W - kMargin - 8;
        const tsl::Color ink = r->a(kText);
        for (s32 i = 0; i < n && top + size_t(i) < doc.count; i++) {
          size_t len;
          const char* p = text::lineAt(doc, top + size_t(i), len);
          if (!len) continue;
          const u32 cellSize = doc.mono[top + size_t(i)];
          const s32 y = kTop + i * rowH() + baseline();
          if (cellSize) drawCells(r, p, len, cellSize, kMargin, textEnd, y, ink);
          else r->drawString(std::string(p, len), false, kMargin, y, g_font, ink);
        }
        // Scrollbar along the right edge
        if (maxTop()) {
          const s32 trackH = n * rowH();
          const s32 thumbH = std::max<s32>(24, s32(s64(trackH) * n / s64(doc.count)));
          const s32 thumbY = kTop + s32(s64(trackH - thumbH) * s64(top) / s64(maxTop()));
          r->drawRect(W - 6, kTop, 3, trackH, r->a(kRule));
          r->drawRect(W - 6, thumbY, 3, thumbH, r->a(kAccent));
        }
      }

      // Footer: guide name on the left, "line X / Y   NN%" on the right, or for 1.5 s
      // after a zoom step the new text size in the accent colour
      r->drawRect(0, H - kFooterH, W, 1, r->a(kRule));
      char pos[64] = "";
      const bool zoomTip = nowNs() < zoomUntil;
      if (zoomTip) snprintf(pos, sizeof(pos), "%u px", g_font);
      else if (doc.count) snprintf(pos, sizeof(pos), "line %zu / %zu   %d%%", topSource + 1, doc.sourceLines, percent());
      const s32 posW = pos[0] ? r->getTextDimensions(pos, false, kSmall).first : 0;
      if (pos[0]) r->drawString(pos, false, W - kMargin - posW, H - 15, kSmall, r->a(zoomTip ? kAccent : kDim));
      r->drawString(r->limitStringLength(name, false, kSmall, W - 3 * kMargin - posW), false,
                    kMargin, H - 15, kSmall, r->a(kText));
    }

    // Touch drag scrolls: finger up = read further down, one line per row pitch
    bool onTouch(tsl::elm::TouchEvent ev, s32, s32 y, s32, s32, s32, s32) override {
      if (ev == tsl::elm::TouchEvent::Touch) {
        touchY = y;
        touchAcc = 0;
        return true;
      }
      if (ev == tsl::elm::TouchEvent::Hold || ev == tsl::elm::TouchEvent::Scroll) {
        touchAcc += touchY - y;
        touchY = y;
        const s32 lines = touchAcc / rowH();
        if (lines) {
          scrollBy(lines);
          touchAcc -= lines * rowH();
        }
        return true;
      }
      return false;
    }

  private:
    s32 touchY = 0, touchAcc = 0;
  };

  class ReaderGui;
  ReaderGui* g_reader = nullptr;  // so Overlay::onHide can save the position

  class ReaderGui : public tsl::Gui {
  public:
    // overPicker: pushed on top of the picker (B pops back to it) rather than opened
    // straight from main() (B then swaps the picker in).
    ReaderGui(std::string path, bool overPicker) : path(std::move(path)), overPicker(overPicker) { g_reader = this; }

    ~ReaderGui() override {
      savePosition();  // B, swap and overlay exit all end here; ~Gui deletes the view after this
      if (g_reader == this) g_reader = nullptr;
    }

    tsl::elm::Element* createUI() override {
      view = new ReaderView();
      open();
      return view;
    }

    // positions.ini: guide path -> top source line. Written only when it moved.
    void savePosition() {
      if (!view || !view->doc.count || view->topSource == savedSource) return;
      putKV(kPosPath, true, path, std::to_string(view->topSource));
      savedSource = view->topSource;
    }

    bool handleInput(u64 down, u64 held, const HidTouchState&, HidAnalogStickState left, HidAnalogStickState right) override;

  private:
    std::string path;
    bool overPicker;
    ReaderView* view = nullptr;
    size_t savedSource = 0;
    Repeat up, dn, pgUp, pgDn, farUp, farDn;
    float stickAcc = 0.0f;
    int zoomDir = 0;   // right stick direction last frame (-1 / 0 / +1)
    u64 zoomLast = 0;  // when the last zoom step fired

    static s32 wrapWidth() { return s32(tsl::cfg::FramebufferWidth) - 2 * kMargin - 8; }  // 8 px: scrollbar

    // Red message instead of the text, wrapped with libtesla's own wrapper (long paths)
    void showError(const std::vector<std::string>& lines) {
      view->error.clear();
      for (const std::string& l : lines)
        for (const std::string& w : tsl::wrapText(l, float(tsl::cfg::FramebufferWidth - 2 * kMargin), "word", false, "", 0, kListFont))
          view->error.push_back(w);
      view->error.push_back("");
      view->error.push_back("B: back to the guide list");
    }

    // Right stick: one step through kSizes, saved to settings.ini, re-wrapped so the same
    // source line stays at the top
    void zoom(int dir) {
      const int n = int(std::size(kSizes));
      int i = 0;
      while (i + 1 < n && kSizes[i] < g_font) i++;
      const u32 next = kSizes[std::clamp(i + dir, 0, n - 1)];
      view->zoomUntil = nowNs() + 1500000000ULL;  // feedback even when already at 16 / 26
      if (next == g_font) return;
      g_font = next;
      putKV(kSettings, false, "font", std::to_string(g_font));
      tsl::gfx::FontManager::clearCache();  // free the old size's glyph bitmaps
      if (!view->doc.count) return;         // error / empty: nothing to re-wrap
      savePosition();                       // in case the re-wrap runs out of memory
      const size_t src = view->topSource;
      if (!text::wrap(view->doc, wrapWidth(), int(g_font), advance, cellWidth)) {
        view->doc.clear();
        showError({"Not enough memory to re-wrap this guide at " + std::to_string(g_font) + " px."});
        logf("  zoom %u px \"%s\" NO MEMORY", g_font, path.c_str());
        return;
      }
      view->top = view->topSource = 0;
      view->scrollTo((long long)text::wrappedLineOf(view->doc, src));
    }

    // Load, decode, wrap; on any failure show a red message instead (never a fatal)
    void open() {
      view->name = match::stem(path);
      const u64 t0 = nowNs();
      text::Result res = text::load(path.c_str(), view->doc);
      if (res == text::Result::Ok) {
        bool ok = text::wrap(view->doc, wrapWidth(), int(g_font), advance, cellWidth);
        // The guide must leave room for glyph bitmaps and the GUI, or libtesla's own
        // small allocations would abort later instead of us failing cleanly now.
        void* spare = ok ? malloc(kHeadroom) : nullptr;
        ok = spare != nullptr;
        free(spare);
        if (!ok) {
          view->doc.clear();
          res = text::Result::NoMemory;
        }
      }
      const u64 ms = (nowNs() - t0) / 1000000ULL;

      struct stat st;
      const long long fileKB = stat(path.c_str(), &st) == 0 ? (long long)(st.st_size / 1024) : -1;
      u64 total, used;
      svcMemory(total, used);
      char msg[160];
      switch (res) {
        case text::Result::Ok:
          if (!view->doc.count) view->error = {"This guide is empty."};
          logf("  open \"%s\" ok %lld KB, %zu lines, %zu wrapped, %llu ms, svc_used=%lluK",
               path.c_str(), fileKB, view->doc.sourceLines, view->doc.count, (unsigned long long)ms,
               (unsigned long long)(used / 1024));
          break;
        case text::Result::NotFound:
          view->error = {"Could not open this guide:", path, "Was it moved or renamed?"};
          logf("  open \"%s\" NOT FOUND", path.c_str());
          break;
        case text::Result::ReadError:
          view->error = {"Could not read this guide from the SD card:", path};
          logf("  open \"%s\" READ ERROR", path.c_str());
          break;
        case text::Result::NoMemory: {
          const size_t largest = largestBlock();
          snprintf(msg, sizeof(msg), "File: %lld KB. Largest free block: %zu KB.", fileKB, largest / 1024);
          view->error = {"Not enough memory to open this guide.", msg,
                         "Try a smaller file, or raise Overlay Memory in",
                         "Ultrahand Settings > System > Overlay Memory."};
          logf("  open \"%s\" NO MEMORY %lld KB, largest=%zuK, svc_used=%lluK", path.c_str(), fileKB,
               largest / 1024, (unsigned long long)(used / 1024));
          break;
        }
      }
      if (!view->error.empty()) {
        showError(std::vector<std::string>(view->error));
        return;
      }

      // Reopen where we left off
      const std::string saved = getKV(readKV(kPosPath, true), path);
      if (!saved.empty()) view->scrollTo((long long)text::wrappedLineOf(view->doc, strtoull(saved.c_str(), nullptr, 10)));
      savedSource = view->topSource;
    }
  };

  // ---- Picker -------------------------------------------------------------------------
  struct Row {
    int guide;          // index into g.guides, -1 for a section label
    const char* label;  // "Best matches" / "All guides"
  };

  class PickerView : public tsl::elm::Element {
  public:
    std::vector<Row> rows;
    std::vector<std::string> context;  // header lines under the title
    int cursor = 0;                    // selected row
    int first = 0;                     // first visible row

    PickerView() {
      m_isItem = false;
      // Header: what is running (or melonDS's recent ROMs)
      if (g.melon) {
        context.push_back(g.roms.empty() ? "melonDS: no recent ROMs in melonDS.ini"
                                         : "melonDS: recent ROMs (as of its last exit)");
        for (const std::string& r : g.roms) context.push_back("  " + r);
      } else if (!g.titleId) {
        context.push_back("No game detected");
      } else {
        context.push_back(g.title.empty() ? "Title " + titleKey(g.titleId) : g.title);
      }
      // Last opened (no game only), suggestions, then every guide; the cursor starts on
      // the first guide row, i.e. the top entry
      if (!g.guides.empty()) {
        if (g.lastOpened >= 0) {
          rows.push_back({-1, "Last opened"});
          rows.push_back({g.lastOpened, nullptr});
        }
        if (!g.best.empty()) {
          rows.push_back({-1, g.bestLabel});
          for (int b : g.best) rows.push_back({b, nullptr});
        }
        rows.push_back({-1, "All guides"});
        for (int i = 0; i < int(g.guides.size()); i++) rows.push_back({i, nullptr});
      }
      moveBy(1);  // onto the first guide row
    }

    void layout(u16 x, u16 y, u16 w, u16 h) override { setBoundaries(x, y, w, h); }

    s32 listTop() const { return 12 + 30 + s32(context.size()) * 22 + 10; }
    int visible() const { return std::max(1, int((s32(tsl::cfg::FramebufferHeight) - kFooterH - listTop()) / kPickRowH)); }
    int cursorGuide() const { return cursor < int(rows.size()) ? rows[cursor].guide : -1; }

    // Move the cursor by d rows, landing on a guide row (labels are skipped)
    void moveBy(int d) {
      const int n = int(rows.size());
      if (!n) return;
      const int dir = d < 0 ? -1 : 1;
      const int c = std::clamp(cursor + d, 0, n - 1);
      int k = c;
      while (k >= 0 && k < n && rows[k].guide < 0) k += dir;
      if (k < 0 || k >= n)
        for (k = c; k >= 0 && k < n && rows[k].guide < 0; k -= dir) {}
      if (k >= 0 && k < n) cursor = k;
      // keep the cursor on screen, plus its section label when that sits right above it
      if (cursor < first) first = cursor;
      if (cursor > 0 && rows[cursor - 1].guide < 0 && cursor - 1 < first) first = cursor - 1;
      if (cursor >= first + visible()) first = cursor - visible() + 1;
    }

    void draw(tsl::gfx::Renderer* r) override {
      const s32 W = tsl::cfg::FramebufferWidth;
      const s32 H = tsl::cfg::FramebufferHeight;
      r->fillScreen(r->a(kBg));

      // Header
      r->drawString("Guide Watch", false, kMargin, 12 + 24, kListFont, r->a(kAccent));
      s32 y = 12 + 30 + 18;
      for (const std::string& l : context) {
        r->drawString(r->limitStringLength(l, false, kSmall, W - 2 * kMargin), false, kMargin, y, kSmall, r->a(kText));
        y += 22;
      }
      const s32 top = listTop();
      r->drawRect(0, top - 6, W, 1, r->a(kRule));

      // Rows
      if (rows.empty()) {
        r->drawString("No .txt guides found in /Guides/ on the SD card.", false, kMargin, top + 26, kListFont, r->a(kRed));
      }
      const int vis = visible();
      for (int i = 0; i < vis && first + i < int(rows.size()); i++) {
        const Row& row = rows[first + i];
        const s32 rowY = top + i * kPickRowH;
        if (row.guide < 0) {
          r->drawString(row.label, false, kMargin, rowY + 25, kSmall, r->a(kAccent));
          continue;
        }
        if (first + i == cursor) r->drawRect(0, rowY, W, kPickRowH, r->a(kSelect));
        const GuideFile& f = g.guides[row.guide];
        const s32 tagW = f.tag.empty() ? 0 : r->getTextDimensions(f.tag, false, kSmall).first;
        if (tagW) r->drawString(f.tag, false, W - kMargin - tagW, rowY + 24, kSmall, r->a(kDim));
        r->drawString(r->limitStringLength(f.name, false, kListFont, W - 3 * kMargin - tagW), false,
                      kMargin, rowY + 25, kListFont, r->a(kText));
      }

      // Footer
      r->drawRect(0, H - kFooterH, W, 1, r->a(kRule));
      r->drawString("A open    B close    L / R page", false, kMargin, H - 15, kSmall, r->a(kDim));
      char count[32];
      snprintf(count, sizeof(count), "%zu guides", g.guides.size());
      const s32 cw = r->getTextDimensions(count, false, kSmall).first;
      r->drawString(count, false, W - kMargin - cw, H - 15, kSmall, r->a(kDim));
    }

    // Drag scrolls the list, a tap (little movement) opens the guide under the finger
    bool onTouch(tsl::elm::TouchEvent ev, s32, s32 y, s32, s32, s32, s32) override {
      if (ev == tsl::elm::TouchEvent::Touch) {
        startY = lastY = y;
        dragAcc = 0;
        dragged = false;
        return true;
      }
      if (ev == tsl::elm::TouchEvent::Hold || ev == tsl::elm::TouchEvent::Scroll) {
        dragAcc += lastY - y;
        lastY = y;
        if (std::abs(y - startY) > 12) dragged = true;
        const s32 steps = dragAcc / kPickRowH;
        if (steps) {
          first = std::clamp(first + steps, 0, std::max(0, int(rows.size()) - visible()));
          dragAcc -= steps * kPickRowH;
        }
        return true;
      }
      if (ev == tsl::elm::TouchEvent::Release && !dragged && y >= listTop()) {
        const int row = first + (y - listTop()) / kPickRowH;
        if (row < int(rows.size()) && rows[row].guide >= 0 && row < first + visible()) {
          cursor = row;
          openGuide(rows[row].guide);
        }
        return true;
      }
      return false;
    }

  private:
    s32 startY = 0, lastY = 0, dragAcc = 0;
    bool dragged = false;
  };

  class PickerGui : public tsl::Gui {
  public:
    tsl::elm::Element* createUI() override {
      view = new PickerView();
      return view;
    }

    bool handleInput(u64 down, u64 held, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override {
      if (down & KEY_B) return false;  // libtesla's goBack: closes the overlay from here
      if (up.fire(down, held, HidNpadButton_AnyUp)) view->moveBy(-1);
      if (dn.fire(down, held, HidNpadButton_AnyDown)) view->moveBy(1);
      if (pgUp.fire(down, held, HidNpadButton_L)) view->moveBy(-view->visible());
      if (pgDn.fire(down, held, HidNpadButton_R)) view->moveBy(view->visible());
      if ((down & HidNpadButton_A) && view->cursorGuide() >= 0) openGuide(view->cursorGuide());
      return true;
    }

  private:
    PickerView* view = nullptr;
    Repeat up, dn, pgUp, pgDn;
  };

  // Reader controls: D-pad line, L/R page, ZL/ZR ten pages, left stick continuous,
  // right stick text size, B back to the picker. libtesla calls this with B alone first.
  bool ReaderGui::handleInput(u64 down, u64 held, const HidTouchState&, HidAnalogStickState left, HidAnalogStickState right) {
    if (down & KEY_B) {
      savePosition();
      if (overPicker) return false;  // libtesla pops us; the picker underneath shows again
      tsl::swapTo<PickerGui>();      // opened directly: replace this GUI with the picker
      return true;                   // `this` is destroyed now, touch nothing
    }
    // Right stick: a zoom step fires when the tilt crosses 60 %, then every 400 ms held there
    const float zoomTilt = float(right.y) / 32767.0f;  // + = up = bigger
    const int zdir = zoomTilt > 0.6f ? 1 : zoomTilt < -0.6f ? -1 : 0;
    if (zdir) {
      const u64 now = nowNs();
      if (zdir != zoomDir || now - zoomLast >= 400000000ULL) {
        zoom(zdir);
        zoomLast = now;
      }
    }
    zoomDir = zdir;

    const long long page = std::max<s32>(1, ReaderView::rows() - 1);  // keep one line of context
    if (up.fire(down, held, HidNpadButton_Up)) view->scrollBy(-1);
    if (dn.fire(down, held, HidNpadButton_Down)) view->scrollBy(1);
    if (pgUp.fire(down, held, HidNpadButton_L)) view->scrollBy(-page);
    if (pgDn.fire(down, held, HidNpadButton_R)) view->scrollBy(page);
    if (farUp.fire(down, held, HidNpadButton_ZL)) view->scrollBy(-10 * page);
    if (farDn.fire(down, held, HidNpadButton_ZR)) view->scrollBy(10 * page);

    // Left stick: past a 20 % dead zone, speed grows with tilt up to 1.5 lines per frame
    const float tilt = float(left.y) / 32767.0f;  // + = up = towards the start
    if (std::fabs(tilt) > 0.2f) stickAcc -= (tilt > 0 ? 1.0f : -1.0f) * (std::fabs(tilt) - 0.2f) / 0.8f * 1.5f;
    else stickAcc = 0.0f;
    const int lines = int(stickAcc);
    if (lines) {
      view->scrollBy(lines);
      stickAcc -= float(lines);
    }
    return true;
  }

  // Picker -> reader. The pick is remembered per title id, or under "0" when no game
  // runs; never in melonDS (one title id for every DS game, see chooseGuide).
  void openGuide(int guide) {
    const std::string path = g.guides[guide].path;
    if (!g.melon) putKV(kLastPath, false, lastKey(), path);
    tsl::changeTo<ReaderGui>(path, true);
  }

  class GuideWatchOverlay : public tsl::Overlay {
  public:
    void initServices() override {}
    void exitServices() override {}
    void onHide() override {
      if (g_reader) g_reader->savePosition();  // the close combo hides us
    }

    // The overlay stays resident while hidden, so the game may have changed since the
    // last show. Overlay::show() (and so this) is only called from tsl::loop's main loop
    // between frames: no Gui method is running and loop()/handleInput() fetch the current
    // Gui afresh every frame, so swapping the whole stack right here is safe and immediate.
    void onShow() override {
      if (!shownOnce) {  // first show after launch: main() has just detected everything
        shownOnce = true;
        return;
      }
      const u64 oldTid = g.titleId;
      const bool oldMelon = g.melon;
      detect();
      if (g.titleId == oldTid && g.melon == oldMelon) return;
      if (g_reader) g_reader->savePosition();
      scanLibrary();  // guides may have been added meanwhile
      chooseGuide();
      logf("show tid=%s name=\"%s\" melon=%d roms=%zu guides=%zu guide=\"%s\" (%s)",
           titleKey(g.titleId).c_str(), g.title.c_str(), g.melon, g.roms.size(), g.guides.size(),
           g.autoOpen.c_str(), g.reason);
      // Our stack is at most picker + reader: SwapDepth(2) replaces all of it. The new
      // GUI is created after the old ones are popped, so the old guide's memory is free.
      if (!g.autoOpen.empty()) tsl::swapTo<ReaderGui>(SwapDepth(2), g.autoOpen, false);
      else tsl::swapTo<PickerGui>(SwapDepth(2));
    }
    std::unique_ptr<tsl::Gui> loadInitialGui() override {
      if (!g.autoOpen.empty()) return initially<ReaderGui>(g.autoOpen, false);
      return initially<PickerGui>();
    }

  private:
    bool shownOnce = false;
  };

}  // namespace

int main(int argc, char** argv) {
  // __appInit (in tesla.hpp) has already mounted the SD card
  mkdir("/config", 0777);
  mkdir(kStateDir, 0777);

  // Everything below runs before libultrahand allocates its framebuffer
  readSettings();
  detect();
  scanLibrary();
  chooseGuide();

  const size_t heap = size_t(fake_heap_end - fake_heap_start);
  const size_t largest = largestBlock();
  chooseWidth();
  u64 total, used;
  svcMemory(total, used);
  rotateLog();
  logf("launch tid=%s name=\"%s\" melon=%d roms=%zu guides=%zu guide=\"%s\" (%s) fb=%u probe=%s "
       "fb_bytes=%zuK heap=%zuK largest=%zuK malloc_used=%zuK svc_total=%lluK svc_used=%lluK",
       titleKey(g.titleId).c_str(), g.title.c_str(), g.melon, g.roms.size(), g.guides.size(),
       g.autoOpen.c_str(), g.reason, g.width, g.probeOk ? "OK" : "FAIL",
       fbBufferBytes(kWantWidth, kHeight) / 1024, heap / 1024, largest / 1024, size_t(mallinfo().uordblks) / 1024,
       (unsigned long long)(total / 1024), (unsigned long long)(used / 1024));

  // Must be set before tsl::loop -> Overlay::initScreen -> Renderer::init, which copies
  // these into cfg::FramebufferWidth/Height and derives the layer size from them.
  ult::DefaultFramebufferWidth = u16(g.width);
  ult::DefaultFramebufferHeight = u16(kHeight);

  return tsl::loop<GuideWatchOverlay>(argc, argv);
}
