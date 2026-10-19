// text.hpp : guide file loading, decoding and wrapping for Guide Watch.
// Pure C/C++ (no libnx), so `make test` exercises it on the Mac (tests/text_test.cpp).
//
// Memory comes from malloc/realloc, never std::vector/std::string: the overlay builds
// with -fno-exceptions, where a failed operator new aborts (= Atmosphere fatal). With
// malloc a guide that does not fit is just an error code the reader can print.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace text {

  // One decoded guide: UTF-8 text with LF line ends, plus the start offset of every
  // wrapped (on-screen) line. Line i spans [lines[i], lines[i+1]) minus trailing
  // spaces/newline, so jumping anywhere in the guide is O(1). mono[i] is the font size
  // of an ASCII-art line drawn in fixed-width cells (0 = ordinary text at the reader size).
  struct Doc {
    char* buf = nullptr;
    size_t size = 0;
    uint32_t* lines = nullptr;
    uint8_t* mono = nullptr;
    size_t count = 0;
    size_t sourceLines = 0;  // lines in the file before wrapping

    Doc() = default;
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    ~Doc() { clear(); }
    void clear() {
      free(buf);
      free(lines);
      free(mono);
      buf = nullptr;
      lines = nullptr;
      mono = nullptr;
      size = count = sourceLines = 0;
    }
  };

  enum class Result { Ok, NotFound, ReadError, NoMemory };

  // Length of the valid UTF-8 sequence at p (1..4), or 0 if the bytes are not valid UTF-8
  inline size_t utf8Len(const unsigned char* p, const unsigned char* end) {
    const unsigned char c = p[0];
    size_t n;
    if (c < 0x80) return 1;
    else if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3;
    else if (c >= 0xF0 && c <= 0xF4) n = 4;
    else return 0;
    if (size_t(end - p) < n) return 0;
    for (size_t i = 1; i < n; i++)
      if ((p[i] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;   // overlong 3-byte
    if (c == 0xED && p[1] >= 0xA0) return 0;  // UTF-16 surrogate
    if (c == 0xF0 && p[1] < 0x90) return 0;   // overlong 4-byte
    if (c == 0xF4 && p[1] >= 0x90) return 0;  // above U+10FFFF
    return n;
  }

  // Decode one codepoint from valid UTF-8 (the buffer is valid after normalise())
  inline size_t decodeUtf8(const unsigned char* p, uint32_t& cp) {
    if (p[0] < 0x80) { cp = p[0]; return 1; }
    if (p[0] < 0xE0) { cp = (uint32_t(p[0] & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
    if (p[0] < 0xF0) { cp = (uint32_t(p[0] & 0x0F) << 12) | (uint32_t(p[1] & 0x3F) << 6) | (p[2] & 0x3F); return 3; }
    cp = (uint32_t(p[0] & 0x07) << 18) | (uint32_t(p[1] & 0x3F) << 12) | (uint32_t(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return 4;
  }

  inline size_t utf8Size(uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

  inline size_t encodeUtf8(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = char(cp); return 1; }
    if (cp < 0x800) { out[0] = char(0xC0 | (cp >> 6)); out[1] = char(0x80 | (cp & 0x3F)); return 2; }
    out[0] = char(0xE0 | (cp >> 12));
    out[1] = char(0x80 | ((cp >> 6) & 0x3F));
    out[2] = char(0x80 | (cp & 0x3F));
    return 3;  // cp1252() never yields more than U+FFFF
  }

  // A byte that is not valid UTF-8 is read as Windows-1252: Latin-1 for 0xA0..0xFF plus
  // the smart quotes / dashes / ellipsis that Windows editors put in 0x80..0x9F.
  inline uint32_t cp1252(unsigned char b) {
    static const uint16_t k80[32] = {
      0x20AC, '?', 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, '?', 0x017D, '?',
      '?', 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, '?', 0x017E, 0x0178,
    };
    return (b >= 0x80 && b < 0xA0) ? k80[b - 0x80] : b;
  }

  // In-place cleanup of a raw file buffer (may realloc *buf):
  //   CRLF and lone CR -> LF, tab -> 4 spaces, other control chars and a UTF-8 BOM
  //   dropped, bytes that are not valid UTF-8 decoded as Windows-1252.
  // Pass 1 counts how many bytes the output grows by (N). The text is then shifted N
  // bytes right and pass 2 decodes it forward into the start of the same buffer; the
  // write position can never overtake the read position, so peak memory is size + N
  // instead of two full copies. Returns false (buffer untouched) if the realloc fails.
  inline bool normalise(char*& buf, size_t& size) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(buf);
    const unsigned char* end = p + size;
    size_t grow = 0;
    for (const unsigned char* q = p; q < end;) {
      if (*q == '\t') { grow += 3; q++; continue; }
      if (*q < 0x80) { q++; continue; }
      const size_t n = utf8Len(q, end);
      if (n) { q += n; continue; }
      grow += utf8Size(cp1252(*q)) - 1;
      q++;
    }

    size_t r = 0;
    if (grow) {
      char* bigger = static_cast<char*>(realloc(buf, size + grow));
      if (!bigger) return false;
      buf = bigger;
      memmove(buf + grow, buf, size);
      r = grow;
    }
    const size_t inEnd = size + grow;
    const unsigned char* in = reinterpret_cast<const unsigned char*>(buf);
    size_t w = 0;
    if (inEnd - r >= 3 && in[r] == 0xEF && in[r + 1] == 0xBB && in[r + 2] == 0xBF) r += 3;  // BOM
    while (r < inEnd) {
      const unsigned char c = in[r];
      if (c == '\r') {                                   // CRLF -> LF, lone CR (old Mac) -> LF
        r += (r + 1 < inEnd && in[r + 1] == '\n') ? 2 : 1;
        buf[w++] = '\n';
      } else if (c == '\t') {
        r++;
        memcpy(buf + w, "    ", 4);
        w += 4;
      } else if (c == '\n' || (c >= 0x20 && c < 0x7F)) {
        buf[w++] = char(c);
        r++;
      } else if (c < 0x80) {                             // other control chars, DEL
        r++;
      } else if (const size_t n = utf8Len(in + r, in + inEnd)) {
        memmove(buf + w, buf + r, n);                    // valid UTF-8 copied as is
        w += n;
        r += n;
      } else {
        w += encodeUtf8(cp1252(c), buf + w);
        r++;
      }
    }
    size = w;
    if (char* fit = static_cast<char*>(realloc(buf, size ? size : 1))) buf = fit;  // give back the slack
    return true;
  }

  // Read a whole file into doc.buf and normalise it. Wrapping is a separate step.
  inline Result load(const char* path, Doc& doc) {
    doc.clear();
    FILE* f = fopen(path, "rb");
    if (!f) return Result::NotFound;
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return Result::ReadError; }
    // ponytail: whole-file load (1.5 MB guides fit at 896 px on an 8 MB heap); streaming
    // a window of the file is the upgrade if 4 MB heaps or 5 MB guides need to work.
    doc.buf = static_cast<char*>(malloc(size_t(len) ? size_t(len) : 1));
    if (!doc.buf) { fclose(f); return Result::NoMemory; }
    doc.size = fread(doc.buf, 1, size_t(len), f);
    const bool ok = doc.size == size_t(len);
    fclose(f);
    if (!ok) { doc.clear(); return Result::ReadError; }
    if (!normalise(doc.buf, doc.size)) { doc.clear(); return Result::NoMemory; }
    return Result::Ok;
  }

  enum LineClass : uint8_t { kBlank = 0, kProse = 1, kArt = 2 };

  // Box drawing (U+2500..U+257F) and block elements (U+2580..U+259F)
  inline bool isBoxDrawing(uint32_t cp) { return cp >= 0x2500 && cp <= 0x259F; }

  // Classify one source line (valid UTF-8, no newline). Art = maps, boxes and tables:
  //   - at least 30 % of the non-space characters are not ASCII letters or digits, or
  //   - it contains box-drawing characters, or
  //   - it has two or more inner runs of 3+ spaces between text (column tables).
  // Other non-ASCII characters (accented letters, curly quotes) count as letters.
  inline LineClass classifyLine(const char* s, size_t n) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    size_t chars = 0, symbols = 0, gaps = 0, run = 0;
    for (size_t i = 0; i < n;) {
      uint32_t cp;
      i += decodeUtf8(p + i, cp);
      if (cp == ' ') { run++; continue; }
      if (chars && run >= 3) gaps++;  // a run with text on both sides
      run = 0;
      chars++;
      if (isBoxDrawing(cp)) return kArt;
      const bool alnum = cp >= 0x80 || (cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z');
      if (!alnum) symbols++;
    }
    if (!chars) return kBlank;
    return (symbols * 10 >= chars * 3 || gaps >= 2) ? kArt : kProse;
  }

  inline bool isArtLine(const char* s, size_t n) { return classifyLine(s, n) == kArt; }

  // Word-wrap every source line to maxWidth pixels at reader size `size`.
  //   adv(codepoint)  the renderer's advance of that glyph at `size` (main.cpp passes
  //                   libtesla's own glyph metrics)
  //   cellWidth(px)   the width of one fixed cell for art lines at font size px
  // Prose breaks at the last space that follows some text; a word longer than the line
  // is cut where it overflows. ASCII-art lines are never wrapped: consecutive art lines
  // (blank lines inside allowed) form a block drawn in fixed cells at the largest size
  // from {size, 20, 18, 16, 14, 12} at which the block's longest line (in codepoints)
  // fits maxWidth; a block that does not fit at 12 stays at 12 and is clipped.
  // A prose line whose nearest non-blank neighbours above and below are both art
  // becomes art too (labels inside maps). Returns false on out-of-memory.
  // ponytail: GameFAQs text is pre-wrapped at ~79 columns, so a line that overflows
  // by one word leaves a short orphan line (the reader's 18 px zoom step avoids most);
  // reflowing whole paragraphs is the upgrade.
  template <class Adv, class Cell>
  bool wrap(Doc& doc, int maxWidth, int size, Adv adv, Cell cellWidth) {
    free(doc.lines);
    free(doc.mono);
    doc.lines = nullptr;
    doc.mono = nullptr;
    doc.count = doc.sourceLines = 0;

    const unsigned char* b = reinterpret_cast<const unsigned char*>(doc.buf);
    size_t newlines = 0;
    for (size_t i = 0; i < doc.size; i++) newlines += doc.buf[i] == '\n';
    const size_t maxSource = newlines + 1;

    // Per source line, temporary: class, then length in codepoints (capped at 255,
    // which no size fits anyway), then the block's cell font size
    uint8_t* cls = static_cast<uint8_t*>(malloc(maxSource));
    uint8_t* len = static_cast<uint8_t*>(malloc(maxSource));
    // Capacity guess for wrapped lines: one per source line plus 25 %, grown if short
    size_t cap = newlines + newlines / 4 + 16;
    doc.lines = static_cast<uint32_t*>(malloc(cap * sizeof(uint32_t)));
    doc.mono = static_cast<uint8_t*>(malloc(cap));
    auto fail = [&]() {
      free(cls);
      free(len);
      return false;
    };
    if (!cls || !len || !doc.lines || !doc.mono) return fail();

    // Pass 1: classify and measure every source line
    size_t n = 0;
    for (size_t s = 0; s < doc.size; n++) {
      const unsigned char* nl = static_cast<const unsigned char*>(memchr(b + s, '\n', doc.size - s));
      const size_t e = nl ? size_t(nl - b) : doc.size;
      size_t z = e;
      while (z > s && b[z - 1] == ' ') z--;
      size_t cps = 0;
      for (size_t i = s; i < z; i++) cps += (b[i] & 0xC0) != 0x80;
      cls[n] = classifyLine(doc.buf + s, z - s);
      len[n] = uint8_t(cps > 255 ? 255 : cps);
      s = e + 1;
    }

    // Pass 2: smoothing on the original classes, blank lines skipped. Bit 4 = the
    // nearest non-blank line above is art, bit 8 = smoothed into art.
    for (size_t k = 0, last = kBlank; k < n; k++) {
      if (cls[k] == kBlank) continue;
      if (last == kArt) cls[k] |= 4;
      last = cls[k] & 3;
    }
    for (size_t k = n, next = kBlank; k-- > 0;) {
      const uint8_t own = cls[k] & 3;
      if (own == kBlank) continue;
      if (own == kProse && (cls[k] & 4) && next == kArt) cls[k] |= 8;
      next = own;
    }
    for (size_t k = 0; k < n; k++) {
      const uint8_t own = cls[k] & 3;
      cls[k] = (own == kArt || (cls[k] & 8)) ? kArt : own;
    }

    // Pass 3: blocks = art lines with only blank lines between them. len[] becomes the
    // block's cell font size on its art lines and 0 everywhere else.
    static const int kArtSizes[] = {20, 18, 16, 14, 12};
    for (size_t k = 0; k < n;) {
      if (cls[k] != kArt) { len[k++] = 0; continue; }
      size_t last = k, longest = 0;
      for (size_t q = k; q < n && cls[q] != kProse; q++)
        if (cls[q] == kArt) { last = q; longest = longest > len[q] ? longest : len[q]; }
      int pick = size;
      if (int(longest) * cellWidth(size) > maxWidth) {
        pick = 12;  // nothing fits: smallest size, the line is clipped at the edge
        for (int c : kArtSizes)
          if (c < size && int(longest) * cellWidth(c) <= maxWidth) { pick = c; break; }
      }
      for (size_t q = k; q <= last; q++) len[q] = cls[q] == kArt ? uint8_t(pick) : 0;
      k = last + 1;
    }

    auto push = [&](size_t off, uint8_t mono) {
      if (doc.count == cap) {
        const size_t more = cap + cap / 2;
        uint32_t* l = static_cast<uint32_t*>(realloc(doc.lines, more * sizeof(uint32_t)));
        if (l) doc.lines = l;
        uint8_t* m = l ? static_cast<uint8_t*>(realloc(doc.mono, more)) : nullptr;
        if (m) doc.mono = m;
        if (!l || !m) return false;
        cap = more;
      }
      doc.lines[doc.count] = uint32_t(off);
      doc.mono[doc.count++] = mono;
      return true;
    };

    // Pass 4: emit wrapped lines
    size_t s = 0;
    for (size_t k = 0; k < n; k++) {
      const unsigned char* nl = static_cast<const unsigned char*>(memchr(b + s, '\n', doc.size - s));
      const size_t e = nl ? size_t(nl - b) : doc.size;
      doc.sourceLines++;
      if (len[k]) {                                  // art: one line, never wrapped
        if (!push(s, len[k])) return fail();
        s = e + 1;
        continue;
      }
      size_t start = s;
      while (true) {
        if (!push(start, 0)) return fail();
        int x = 0;
        size_t p = start, lastSpace = 0;
        bool seenText = false;
        while (p < e) {
          uint32_t cp;
          const size_t step = decodeUtf8(b + p, cp);
          const int w = adv(cp);
          if (x + w > maxWidth && p > start) break;  // overflow (always keep >= 1 glyph)
          if (cp != ' ') seenText = true;
          else if (seenText) lastSpace = p;
          x += w;
          p += step;
        }
        if (p >= e) break;                           // rest of the source line fits
        size_t next = lastSpace > start ? lastSpace : p;
        while (next < e && b[next] == ' ') next++;   // continuation starts at text
        if (next >= e) break;                        // only trailing spaces were left
        start = next;
      }
      s = e + 1;
    }
    free(cls);
    free(len);
    return true;
  }

  // Wrapped line i as (pointer, length) with trailing spaces / newline trimmed
  inline const char* lineAt(const Doc& doc, size_t i, size_t& len) {
    const size_t a = doc.lines[i];
    size_t z = (i + 1 < doc.count) ? doc.lines[i + 1] : doc.size;
    while (z > a && (doc.buf[z - 1] == '\n' || doc.buf[z - 1] == ' ')) z--;
    len = z - a;
    return doc.buf + a;
  }

  // True if wrapped line i continues the previous source line (was split by wrap())
  inline bool isContinuation(const Doc& doc, size_t i) {
    return i > 0 && doc.buf[doc.lines[i] - 1] != '\n';
  }

  // Source line (0-based) of wrapped line `to`, given that wrapped line `from` is in
  // source line `src`. O(|to - from|), so scrolling never rescans the whole guide.
  inline size_t sourceLineAt(const Doc& doc, size_t from, size_t src, size_t to) {
    for (size_t k = from + 1; k <= to && k < doc.count; k++) src += !isContinuation(doc, k);
    for (size_t k = to + 1; k <= from && k < doc.count; k++) src -= !isContinuation(doc, k);
    return src;
  }

  // First wrapped line of source line `src` (clamped to the last line)
  inline size_t wrappedLineOf(const Doc& doc, size_t src) {
    size_t seen = 0;
    for (size_t k = 1; k < doc.count; k++) {
      if (isContinuation(doc, k)) continue;
      if (++seen == src) return k;
    }
    return doc.count ? (src ? doc.count - 1 : 0) : 0;
  }

}  // namespace text
