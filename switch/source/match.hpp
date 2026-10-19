// match.hpp : game name <-> guide file name matching for Guide Watch.
// Pure functions, no libnx: compiled on the Mac by `make test` (tests/match_test.cpp).
//
//   words("The Legend of Zelda: Link's Awakening") -> legend of zelda links awakening
//   score(game, guide)  Jaccard token overlap, 0 when the sequel numbers disagree,
//                       kContained when the shorter name sits wholly in the longer one
//   rank(game, guide)   ordering key for the picker (rewards a guide name fully
//                       contained in the game name, e.g. skyward.txt for Skyward Sword HD)
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace match {

  using Tokens = std::vector<std::string>;  // sorted + unique (see tokens())

  constexpr double kStrong = 0.8;    // score >= this (and unique) opens a guide without asking
  constexpr double kContained = 0.85;  // score of a containment match (strong)
  constexpr double kMinRank = 0.25;  // rank >= this lands in the picker's "Best matches"

  // UTF-8 U+00C0..U+00FF (second byte 0x80..0xBF after 0xC3) folded to ASCII, so
  // "Pokémon" matches "Pokemon". Upper and lower case share the same layout.
  inline const char* foldLatin1(unsigned char second) {
    static const char* kFold[32] = {
      "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
      "d", "n", "o", "o", "o", "o", "o", " ", "o", "u", "u", "u", "u", "y", "th", "ss",
    };
    if (second == 0xBF) return "y";  // U+00FF y with diaeresis (U+00DF is sharp s)
    return kFold[(second - 0x80) & 0x1F];
  }

  // 1..39 in lowercase roman numerals ("" for anything else)
  inline std::string toRoman(int n) {
    static const char* kOnes[10] = {"", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix"};
    return std::string(size_t(n / 10), 'x') + kOnes[n % 10];
  }

  // "xii" -> "12"; anything that is not a canonical numeral 1..39 is returned unchanged
  inline std::string romanToDigits(const std::string& w) {
    if (w.empty() || w.size() > 6 || w.find_first_not_of("ivx") != std::string::npos) return w;
    for (int n = 1; n < 40; n++)
      if (toRoman(n) == w) return std::to_string(n);
    return w;
  }

  inline bool isNumber(const std::string& w) {
    return !w.empty() && w.find_first_not_of("0123456789") == std::string::npos;
  }

  inline bool isStopWord(const std::string& w) {
    static const char* kStop[] = {"the", "a", "guide", "guides", "faq", "faqs", "walkthrough", "walkthroughs"};
    for (const char* s : kStop)
      if (w == s) return true;
    return false;
  }

  // Normalised words in their original order:
  //   lowercase, accents folded, apostrophes removed (Link's -> links, so it matches a
  //   file that dropped the apostrophe), (...) / [...] / {...} tags removed, every other
  //   non-alphanumeric -> word break, stop words dropped, roman numerals -> digits,
  //   leading zeros dropped, 5+ digit numbers dropped (GameFAQs file ids like 198972-...-10780).
  inline std::vector<std::string> words(std::string_view s) {
    std::string flat;
    flat.reserve(s.size());
    int depth = 0;  // inside (...) / [...] / {...}
    for (size_t i = 0; i < s.size(); i++) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == '(' || c == '[' || c == '{') { depth++; continue; }
      if (c == ')' || c == ']' || c == '}') { if (depth) depth--; flat += ' '; continue; }
      if (depth) continue;
      if (c == '\'' || c == '`') continue;                                   // ASCII apostrophes
      if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
          (static_cast<unsigned char>(s[i + 2]) == 0x98 || static_cast<unsigned char>(s[i + 2]) == 0x99)) {
        i += 2;                                                              // curly apostrophes
        continue;
      }
      if (c == 0xC3 && i + 1 < s.size()) {                                   // accented Latin letter
        flat += foldLatin1(static_cast<unsigned char>(s[++i]));
        continue;
      }
      if (c >= 'A' && c <= 'Z') flat += char(c - 'A' + 'a');
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) flat += char(c);
      else flat += ' ';                                                      // punctuation, ™, other UTF-8
    }

    std::vector<std::string> out;
    size_t i = 0;
    while (i < flat.size()) {
      while (i < flat.size() && flat[i] == ' ') i++;
      size_t j = i;
      while (j < flat.size() && flat[j] != ' ') j++;
      if (j > i) {
        std::string w = romanToDigits(flat.substr(i, j - i));
        if (isNumber(w)) {
          w.erase(0, std::min(w.find_first_not_of('0'), w.size() - 1));  // "007" -> "7", "0" stays
          if (w.size() >= 5) { i = j; continue; }                          // file id, not a title number
        }
        if (!isStopWord(w)) out.push_back(w);
      }
      i = j;
    }
    return out;
  }

  // Words joined with single spaces (what the tests compare)
  inline std::string normalise(std::string_view s) {
    std::string out;
    for (const std::string& w : words(s)) {
      if (!out.empty()) out += ' ';
      out += w;
    }
    return out;
  }

  // Sorted unique word set used for scoring
  inline Tokens tokens(std::string_view s) {
    Tokens t = words(s);
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    return t;
  }

  inline size_t overlap(const Tokens& a, const Tokens& b) {
    size_t n = 0;
    for (auto i = a.begin(), j = b.begin(); i != a.end() && j != b.end();) {
      if (*i < *j) i++;
      else if (*j < *i) j++;
      else { n++; i++; j++; }
    }
    return n;
  }

  // Sequel guard: true when EACH side has a number the other lacks (IX vs XII, 2 vs 3).
  // A number on one side only ("Paper Mario 2 ..." vs "Paper Mario: The Thousand-Year
  // Door") is not a mismatch; it just counts against the overlap like any other word.
  inline bool numbersClash(const Tokens& a, const Tokens& b) {
    auto hasOwnNumber = [](const Tokens& x, const Tokens& y) {
      for (const std::string& w : x)
        if (isNumber(w) && !std::binary_search(y.begin(), y.end(), w)) return true;
      return false;
    };
    return hasOwnNumber(a, b) && hasOwnNumber(b, a);
  }

  // Same set of numbers on both sides (none counts as a set)
  inline bool sameNumbers(const Tokens& a, const Tokens& b) {
    Tokens na, nb;
    for (const std::string& w : a) if (isNumber(w)) na.push_back(w);
    for (const std::string& w : b) if (isNumber(w)) nb.push_back(w);
    return na == nb;  // tokens are sorted, so the number lists are too
  }

  // Jaccard similarity |A n B| / |A u B|, 0 on a sequel-number clash.
  // Containment: when every word of the shorter name (2+ words) is in the longer one
  // and the numbers are identical, the pair is strong (kContained). Forwarder tiles are
  // often short ("Twilight Princess") while guide files carry the full name ("The
  // Legend of Zelda - Twilight Princess"). Containment needs identical numbers, the
  // strict form of the sequel guard, so "Xenoblade Chronicles" never becomes strong
  // for "Xenoblade Chronicles 2".
  inline double score(const Tokens& game, const Tokens& guide) {
    if (game.empty() || guide.empty() || numbersClash(game, guide)) return 0.0;
    const size_t both = overlap(game, guide);
    const double jaccard = double(both) / double(game.size() + guide.size() - both);
    const size_t shorter = std::min(game.size(), guide.size());
    if (shorter >= 2 && both == shorter && sameNumbers(game, guide)) return std::max(jaccard, kContained);
    return jaccard;
  }

  // Picker ordering: mean of the Jaccard score and how much of the guide name the game
  // name covers. Pure Jaccard would rank "Links Awakening" (shares legend/of/zelda)
  // above "skyward" for "Skyward Sword HD"; coverage puts the right guide first.
  inline double rank(const Tokens& game, const Tokens& guide) {
    const double s = score(game, guide);
    if (s == 0.0) return 0.0;
    return (s + double(overlap(game, guide)) / double(guide.size())) / 2.0;
  }

  // Best score of one guide against several game names (melonDS hands us up to five ROMs)
  inline double bestScore(const std::vector<Tokens>& games, const Tokens& guide) {
    double best = 0.0;
    for (const Tokens& g : games) best = std::max(best, score(g, guide));
    return best;
  }

  inline double bestRank(const std::vector<Tokens>& games, const Tokens& guide) {
    double best = 0.0;
    for (const Tokens& g : games) best = std::max(best, rank(g, guide));
    return best;
  }

  // Index of the single guide that strongly matches, or -1 if none or several do
  inline int strongPick(const std::vector<Tokens>& games, const std::vector<Tokens>& guides) {
    int pick = -1;
    for (size_t i = 0; i < guides.size(); i++) {
      if (bestScore(games, guides[i]) < kStrong) continue;
      if (pick >= 0) return -1;  // two strong candidates: let the user choose
      pick = int(i);
    }
    return pick;
  }

  // Guides with rank >= kMinRank, best first (ties keep list order), at most `limit`
  inline std::vector<int> bestMatches(const std::vector<Tokens>& games, const std::vector<Tokens>& guides, size_t limit) {
    std::vector<std::pair<double, int>> scored;
    for (size_t i = 0; i < guides.size(); i++) {
      const double r = bestRank(games, guides[i]);
      if (r >= kMinRank) scored.push_back({r, int(i)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& x, auto& y) { return x.first > y.first; });
    std::vector<int> out;
    for (size_t i = 0; i < scored.size() && i < limit; i++) out.push_back(scored[i].second);
    return out;
  }

  // "sdmc:/roms/nds/Zelda - PH (E).nds" -> "Zelda - PH (E)"
  inline std::string stem(std::string_view path) {
    const size_t slash = path.find_last_of("/\\:");
    if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
    const size_t dot = path.rfind('.');
    if (dot != std::string_view::npos && dot > 0) path = path.substr(0, dot);
    return std::string(path);
  }

  // Case-insensitive ASCII substring test ("melonDS" contains "melonds")
  inline bool containsNoCase(std::string_view hay, std::string_view needle) {
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
      size_t k = 0;
      while (k < needle.size() && lower(hay[i + k]) == lower(needle[k])) k++;
      if (k == needle.size()) return true;
    }
    return false;
  }

}  // namespace match
