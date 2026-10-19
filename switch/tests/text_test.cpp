// text_test.cpp : host unit tests for source/text.hpp (decode + wrap), plain asserts.
// Run with `make test`. If the gitignored switch/guides/*.txt samples exist, it also
// loads and wraps them with an approximate glyph width and prints the numbers.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <string>

#include "../source/text.hpp"

static int passed = 0;
#define CHECK(cond) do { assert(cond); passed++; } while (0)

// Run normalise() on a copy of `in` and return the result
static std::string norm(const std::string& in) {
  size_t size = in.size();
  char* buf = static_cast<char*>(malloc(size ? size : 1));
  memcpy(buf, in.data(), size);
  assert(text::normalise(buf, size));
  std::string out(buf, size);
  free(buf);
  return out;
}

// Fake metrics: 10 px per proportional glyph, art cell = half the font size
static int fakeAdv(uint32_t) { return 10; }
static int fakeCell(int px) { return px / 2; }

// Load `content` through a temp file, wrap at `width` (reader size `size`)
static void wrapString(text::Doc& doc, const std::string& content, int width, int size = 22) {
  const char* path = "build/host/wrap_test.txt";
  FILE* f = fopen(path, "wb");
  assert(f);
  fwrite(content.data(), 1, content.size(), f);
  fclose(f);
  assert(text::load(path, doc) == text::Result::Ok);
  assert(text::wrap(doc, width, size, fakeAdv, fakeCell));
}

static std::string line(const text::Doc& doc, size_t i) {
  size_t n;
  const char* p = text::lineAt(doc, i, n);
  return std::string(p, n);
}

int main() {
  // --- decode -------------------------------------------------------------------------
  CHECK(norm("a\r\nb\r\n") == "a\nb\n");                       // CRLF
  CHECK(norm("a\rb") == "a\nb");                               // lone CR (old Mac)
  CHECK(norm("caf\xE9") == "caf\xC3\xA9");                     // Latin-1 e acute
  CHECK(norm("\xA9 1998") == "\xC2\xA9 1998");                 // Latin-1 copyright
  CHECK(norm("it\x92s") == "it\xE2\x80\x99s");                 // cp1252 right quote (+2 bytes)
  CHECK(norm("caf\xC3\xA9") == "caf\xC3\xA9");                 // valid UTF-8 untouched
  CHECK(norm("\xEF\xBB\xBFhi") == "hi");                       // BOM dropped
  CHECK(norm("a\tb") == "a    b");                             // tab -> 4 spaces
  CHECK(norm("a\x0C\x1A\x7F" "b") == "ab");                    // form feed, ^Z, DEL dropped
  CHECK(norm("\xE9\t\r\n\xE9") == "\xC3\xA9    \n\xC3\xA9");   // growth + shrink in one pass
  CHECK(norm("\xC3") == "\xC3\x83");                           // truncated UTF-8 lead -> Latin-1
  CHECK(norm("\xED\xA0\x80") == "\xC3\xAD\xC2\xA0\xE2\x82\xAC"); // surrogate bytes are not UTF-8
  CHECK(norm("") == "");

  // --- wrap (10 px per glyph, 100 px = 10 glyphs) ------------------------------------
  {
    text::Doc d;
    wrapString(d, "short\n\naaaa bbbb cccc dddd\naaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                  "   indent text that wraps\n   abcdefghij\n", 100);
    CHECK(d.sourceLines == 6);
    CHECK(line(d, 0) == "short");
    CHECK(line(d, 1) == "");                                   // blank line kept
    CHECK(line(d, 2) == "aaaa bbbb");                          // break at the last space
    CHECK(line(d, 3) == "cccc dddd");
    CHECK(text::isContinuation(d, 3) && !text::isContinuation(d, 2));
    CHECK(line(d, 4) == "aaaaaaaaaa");                         // hard break inside a long word
    CHECK(line(d, 5) == "aaaaaaaaaa" && line(d, 6) == "aaaaaaaaaa");
    CHECK(line(d, 7) == "   indent");
    CHECK(line(d, 8) == "text that" && line(d, 9) == "wraps");
    CHECK(line(d, 10) == "   abcdefg" && line(d, 11) == "hij"); // never breaks inside the indent
    CHECK(d.count == 12);
    CHECK(text::sourceLineAt(d, 0, 0, 0) == 0 && text::sourceLineAt(d, 0, 0, 3) == 2);
    CHECK(text::sourceLineAt(d, 0, 0, 6) == 3 && text::sourceLineAt(d, 0, 0, 9) == 4);
    CHECK(text::sourceLineAt(d, 0, 0, 11) == 5);
    CHECK(text::sourceLineAt(d, 11, 5, 3) == 2 && text::sourceLineAt(d, 6, 3, 8) == 4);  // backwards / forwards
    CHECK(text::wrappedLineOf(d, 0) == 0 && text::wrappedLineOf(d, 2) == 2);
    CHECK(text::wrappedLineOf(d, 3) == 4 && text::wrappedLineOf(d, 4) == 7 && text::wrappedLineOf(d, 5) == 10);
    CHECK(text::wrappedLineOf(d, 99) == 11);                   // past the end -> last line
    for (size_t i = 0; i < d.count; i++) CHECK(d.mono[i] == 0);  // all prose
  }

  // --- classifier --------------------------------------------------------------------
  auto art = [](const char* l) { return text::isArtLine(l, strlen(l)); };
  CHECK(art("==========") && art("| -- |") && art("| Item |"));       // >= 30 % symbols
  CHECK(art("ab-") && !art("abcdefg-"));                               // 33 % vs 12.5 %
  CHECK(art("\xE2\x94\x80\xE2\x94\x80 Map of the island"));             // box drawing U+2500
  CHECK(art("Item     Location     Price"));                         // two inner 3+ space runs
  CHECK(!art("Item     Location") && !art("      indented prose line"));  // one run / indent only
  CHECK(!art("That fish will give you a sea chart piece. There is"));
  CHECK(!art("Caf\xC3\xA9 it\xE2\x80\x99s fine"));                      // accents, curly quote = letters
  CHECK(!art("") && !art("    "));                                    // blank is never art

  // --- smoothing and fixed-cell blocks (fake metrics, 300 px text width) --------------
  {
    text::Doc d;
    wrapString(d,
      "+--------+\n"               // 0 art
      "| Shop   |\n"               // 1 art
      "Town label\n"               // 2 prose between art lines -> art (map label)
      "+--------+\n"               // 3 art
      "\n"                         // 4 blank inside the block
      "+--------+\n"               // 5 art, same block
      "\n"
      "A prose paragraph, line one,\n"  // 7 stays prose (between two maps)
      "and line two of it.\n"           // 8 stays prose
      "\n"
      "+--------------------------------------+\n"                 // 10 art, 40 chars
      "\n"
      "Go north.\n"                // 12 single line between art across blank lines -> art
      "\n"
      "+--------+\n",              // 14 art
      300);
    auto mono = [&](size_t src) { return d.mono[text::wrappedLineOf(d, src)]; };
    CHECK(mono(0) == 22 && mono(1) == 22 && mono(2) == 22 && mono(3) == 22 && mono(5) == 22);
    CHECK(mono(4) == 0);                                          // blank lines carry no size
    CHECK(mono(7) == 0 && mono(8) == 0);                          // paragraph is not smoothed into art
    // 40 chars: 22 -> 440 px, 20 -> 400, 18 -> 360, 16 -> 320, 14 -> 280 fits 300
    CHECK(mono(10) == 14 && mono(12) == 14 && mono(14) == 14);    // one size for the whole block
    CHECK(d.count == d.sourceLines);                              // the prose lines fit, art never wraps
    // Reader at 16 px: a block is never bigger than the reader size
    wrapString(d, "+--------+\n| Shop   |\n+--------+\n", 300, 16);
    CHECK(mono(0) == 16 && mono(1) == 16);
    // Too wide even at 12 px (60 chars * 6 px > 100): still one line, minimum size 12
    wrapString(d, "+----------------------------------------------------------+\n", 100);
    CHECK(d.count == 1 && d.mono[0] == 12);
  }

  // --- block size with a 0.6 em cell (the 'n' advance) at the real 896 px text width ----
  {
    const int textW = 896 - 2 * 14 - 8;  // main.cpp: framebuffer - margins - scrollbar
    auto cell06 = [](int px) { return px * 6 / 10; };
    auto blockSize = [&](size_t cols, int reader) {
      text::Doc d;
      const std::string row = "|" + std::string(cols - 2, '-') + "|\n";
      const std::string content = row + row;
      FILE* f = fopen("build/host/wrap_test.txt", "wb");
      fwrite(content.data(), 1, content.size(), f);
      fclose(f);
      assert(text::load("build/host/wrap_test.txt", d) == text::Result::Ok);
      assert(text::wrap(d, textW, reader, fakeAdv, cell06));
      return int(d.mono[0]);
    };
    const int s80 = blockSize(80, 22), s120 = blockSize(120, 22);
    printf("  0.6 em cell, %d px text width: 80-col block -> %d px, 120-col block -> %d px\n", textW, s80, s120);
    CHECK(s80 >= 16);                         // 18: 80 * 10 = 800 <= 860 (20 px: 80 * 12 = 960)
    CHECK(s120 == 12);                        // 120 * 7 = 840 fits at 12, 14 px would be 960
    CHECK(blockSize(80, 16) == 16);           // never bigger than the reader size
    CHECK(blockSize(40, 26) == 26);           // small blocks keep the reader size
  }
  {
    text::Doc d;
    wrapString(d, "no newline at end", 1000);                  // last line without LF
    CHECK(d.count == 1 && line(d, 0) == "no newline at end");
    wrapString(d, "", 1000);                                   // empty file
    CHECK(d.count == 0 && d.sourceLines == 0);
    wrapString(d, "x\r\n\r\n", 1000);                          // CRLF file, trailing blank line
    CHECK(d.count == 2 && line(d, 0) == "x" && line(d, 1) == "");
  }
  {
    text::Doc d;
    CHECK(text::load("build/host/does-not-exist.txt", d) == text::Result::NotFound);
  }

  // --- real GameFAQs samples (optional, gitignored) ----------------------------------
  if (DIR* dir = opendir("guides")) {
    while (dirent* e = readdir(dir)) {
      const std::string name = e->d_name;
      if (name.size() < 5 || name.compare(name.size() - 4, 4, ".txt") != 0 || name[0] == '.') continue;
      const std::string path = "guides/" + name;
      text::Doc d;
      const auto t0 = std::chrono::steady_clock::now();
      CHECK(text::load(path.c_str(), d) == text::Result::Ok);
      // ~11 px average advance at 22 px (capital/wide glyphs a bit more); art cell 0.6 em
      CHECK(text::wrap(d, 860, 22, [](uint32_t cp) { return (cp >= 'A' && cp <= 'Z') || cp == 'm' || cp == 'w' ? 15 : 10; },
                       [](int px) { return px * 6 / 10; }));
      const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      CHECK(d.count >= d.sourceLines);
      // Art share of the non-blank source lines, and how many fixed-cell blocks
      size_t nonBlank = 0, artLines = 0, blocks = 0;
      bool inBlock = false;
      for (size_t i = 0; i < d.count; i++) {
        size_t n;
        text::lineAt(d, i, n);
        if (text::isContinuation(d, i) || !n) continue;
        nonBlank++;
        if (d.mono[i]) { artLines++; if (!inBlock) blocks++; }
        inBlock = d.mono[i] != 0;
      }
      printf("  sample %-60s %8zu bytes  %6zu lines -> %6zu wrapped  %.1f ms\n",
             name.c_str(), d.size, d.sourceLines, d.count, ms);
      printf("         art: %zu of %zu non-blank lines (%.1f %%), %zu blocks\n",
             artLines, nonBlank, 100.0 * double(artLines) / double(nonBlank ? nonBlank : 1), blocks);
      if (name == "The Legend of Zelda - The Wind Waker.txt") {  // Zyre's guide
        auto monoOf = [&](const char* needle) {
          for (size_t i = 0; i < d.count; i++) {
            size_t n;
            const char* p = text::lineAt(d, i, n);
            if (std::string(p, n).find(needle) != std::string::npos) return int(d.mono[i]);
          }
          assert(!"sample line not found");
          return -1;
        };
        CHECK(monoOf("B .   . @#.   . @ .   . @ .   . B") > 0);         // sea chart row
        CHECK(monoOf("1   2   3   4   5   6   7") > 0);                 // sea chart column labels
        CHECK(monoOf("That fish will give you a sea chart piece.") == 0);  // walkthrough prose
        CHECK(monoOf("This document is protected by U.S. Copyright Law") == 0);  // paragraph after an art banner
      }
    }
    closedir(dir);
  }

  printf("text_test: %d checks passed\n", passed);
  return 0;
}
