// match_test.cpp : host unit tests for source/match.hpp (plain asserts).
// Run with `make test` from switch/ (uses the Mac's clang++, no devkitPro needed).
#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "../source/match.hpp"

using match::Tokens;

// The guide library from Ludovic's SD card, as the picker would list it (path stems)
static const char* kGuides[] = {
  "The Legend of Zelda Links Awakening",
  "Final Fantasy XII The Zodiac Age",
  "Paper Mario 2 The Thousand-Year Door - Guide",
  "Ni no Kuni Wrath of the White Witch - Guide",
  "skyward",
  "The Legend of Zelda - The Wind Waker",
  "Ship of Harkinian",
  "Final Fantasy IX",
  "198972-the-legend-of-zelda-oracle-of-seasons-faqs-10780",
};
static const int kCount = int(sizeof(kGuides) / sizeof(kGuides[0]));

static std::vector<Tokens> library() {
  std::vector<Tokens> out;
  for (const char* g : kGuides) out.push_back(match::tokens(g));
  return out;
}

// Which guide opens without the picker for this running name (-1 = picker)
static int autoPick(const char* running) {
  return match::strongPick({match::tokens(running)}, library());
}

static int indexOf(const char* guide) {
  for (int i = 0; i < kCount; i++)
    if (std::string(kGuides[i]) == guide) return i;
  assert(!"guide missing from kGuides");
  return -1;
}

static int passed = 0;
#define CHECK(cond) do { assert(cond); passed++; } while (0)

int main() {
  // --- normalise ---------------------------------------------------------------
  CHECK(match::normalise("The Legend of Zelda: Link's Awakening") == "legend of zelda links awakening");
  CHECK(match::normalise("The Legend of Zelda: Link\xE2\x80\x99s Awakening") == "legend of zelda links awakening");
  CHECK(match::normalise("FINAL FANTASY XII THE ZODIAC AGE") == "final fantasy 12 zodiac age");
  CHECK(match::normalise("Paper Mario 2 The Thousand-Year Door - Guide") == "paper mario 2 thousand year door");
  CHECK(match::normalise("Pok\xC3\xA9mon Legends: Arceus") == "pokemon legends arceus");
  CHECK(match::normalise("Zelda - Phantom Hourglass (Europe) (En,Fr,De) [!]") == "zelda phantom hourglass");
  CHECK(match::normalise("198972-the-legend-of-zelda-oracle-of-seasons-faqs-10780") == "legend of zelda oracle of seasons");
  CHECK(match::normalise("A Link Between Worlds Walkthrough") == "link between worlds");
  CHECK(match::normalise("GoldenEye 007") == "goldeneye 7");
  CHECK(match::normalise("Final Fantasy X-2") == "final fantasy 10 2");
  CHECK(match::normalise("Mix Civ Xvi") == "mix civ 16");  // only canonical i/v/x numerals convert
  CHECK(match::normalise("The Wind Waker (DarthMarth)") == "wind waker");

  // --- the five cases from the brief ------------------------------------------------
  // 1. Link's Awakening opens the Links Awakening file directly
  CHECK(autoPick("The Legend of Zelda: Link's Awakening") == indexOf("The Legend of Zelda Links Awakening"));
  // 2. Upper-case NACP name with roman numeral
  CHECK(autoPick("FINAL FANTASY XII THE ZODIAC AGE") == indexOf("Final Fantasy XII The Zodiac Age"));
  // 3. TTYD has no "2" in its Switch name; a number on one side only is not a clash,
  //    so 5 shared words of 6 (0.83) is still strong
  CHECK(autoPick("Paper Mario: The Thousand-Year Door") == indexOf("Paper Mario 2 The Thousand-Year Door - Guide"));
  // 4. Skyward Sword HD vs "skyward": 1 shared word of 6 is not strong (a lone word must
  //    never auto-open: a "zelda.txt" would then hijack every Zelda game). It falls to
  //    the picker, where coverage ranks skyward.txt FIRST, above the other Zelda guides.
  {
    const Tokens game = match::tokens("The Legend of Zelda: Skyward Sword HD");
    CHECK(match::score(game, match::tokens("skyward")) < match::kStrong);
    CHECK(autoPick("The Legend of Zelda: Skyward Sword HD") == -1);
    const std::vector<int> best = match::bestMatches({game}, library(), 10);
    CHECK(!best.empty() && best[0] == indexOf("skyward"));
  }
  // 5. Sequel guard: IX never matches XII (each side has a number the other lacks)
  {
    const Tokens ff9 = match::tokens("Final Fantasy IX");
    CHECK(match::score(ff9, match::tokens("Final Fantasy XII The Zodiac Age")) == 0.0);
    CHECK(match::rank(ff9, match::tokens("Final Fantasy XII The Zodiac Age")) == 0.0);
    CHECK(autoPick("Final Fantasy IX") == indexOf("Final Fantasy IX"));
    CHECK(autoPick("FINAL FANTASY XII THE ZODIAC AGE") != indexOf("Final Fantasy IX"));
  }

  // --- more behaviour --------------------------------------------------------------
  // Ni no Kuni Remastered: 7 of 8 words
  CHECK(autoPick("Ni no Kuni Wrath of the White Witch Remastered") == indexOf("Ni no Kuni Wrath of the White Witch - Guide"));
  // Wind Waker HD: strong (5 of 6) and unique in this library
  CHECK(autoPick("The Legend of Zelda: The Wind Waker HD") == indexOf("The Legend of Zelda - The Wind Waker"));
  // Two equally strong guides (same game, two authors) -> picker, both listed first
  {
    const std::vector<Tokens> two = {match::tokens("The Legend of Zelda - The Wind Waker"),
                                     match::tokens("The Legend of Zelda - The Wind Waker (DarthMarth)")};
    const Tokens game = match::tokens("The Legend of Zelda: The Wind Waker HD");
    CHECK(match::strongPick({game}, two) == -1);
    CHECK(match::bestMatches({game}, two, 10).size() == 2);
  }
  // Nothing in common -> picker with no best matches
  CHECK(autoPick("Mario Kart 8 Deluxe") == -1);
  CHECK(match::bestMatches({match::tokens("Mario Kart 8 Deluxe")}, library(), 10).empty());
  // Sequel numbers 2 vs 3 clash; original vs numbered sequel only costs overlap
  CHECK(match::score(match::tokens("Xenoblade Chronicles 3"), match::tokens("Xenoblade Chronicles 2")) == 0.0);
  CHECK(match::score(match::tokens("Xenoblade Chronicles 2"), match::tokens("Xenoblade Chronicles")) < match::kStrong);
  // No game detected (title id 0, empty name) -> nothing matches
  CHECK(autoPick("") == -1);

  // --- containment: short forwarder titles vs full guide names -----------------------
  {
    std::vector<Tokens> lib = library();
    lib.push_back(match::tokens("The Legend of Zelda - Twilight Princess"));
    const int tp = int(lib.size()) - 1;
    CHECK(match::score(match::tokens("Twilight Princess"), lib[tp]) == match::kContained);
    CHECK(match::strongPick({match::tokens("Twilight Princess")}, lib) == tp);
    // 1 word on the short side: no containment
    CHECK(match::score(match::tokens("Skyward Sword HD"), match::tokens("skyward")) < match::kStrong);
    CHECK(match::strongPick({match::tokens("Skyward Sword HD")}, lib) == -1);
    // Numbers must be identical for containment
    CHECK(match::score(match::tokens("Final Fantasy IX"), match::tokens("Final Fantasy XII")) < match::kStrong);
    CHECK(match::score(match::tokens("Final Fantasy"), match::tokens("Final Fantasy XII The Zodiac Age")) < match::kStrong);
    // Guide name inside the game name works too (both directions)
    CHECK(match::score(match::tokens("The Legend of Zelda: Twilight Princess HD"), match::tokens("Twilight Princess")) == match::kContained);
  }

  // --- melonDS: ROM paths from melonDS.ini are matched by their stem ------------------
  CHECK(match::stem("sdmc:/roms/nds/Final Fantasy IX (USA).nds") == "Final Fantasy IX (USA)");
  CHECK(match::stem("C:\\roms\\skyward.nds") == "skyward");
  CHECK(match::stem("noext") == "noext");
  CHECK(match::containsNoCase("melonDS 0.9.5", "melonds"));
  CHECK(!match::containsNoCase("Mario", "melonds"));
  {
    std::vector<Tokens> roms;
    for (const char* p : {"sdmc:/nds/Mario Kart DS (E).nds", "sdmc:/nds/Final Fantasy IX (USA).nds"})
      roms.push_back(match::tokens(match::stem(p)));
    CHECK(match::strongPick(roms, library()) == indexOf("Final Fantasy IX"));
    // Picker suggestions (melonDS running, or no game + melonDS.ini present): the ROM's
    // guide is the top entry, which the picker preselects
    const std::vector<int> best = match::bestMatches(roms, library(), 10);
    CHECK(!best.empty() && best[0] == indexOf("Final Fantasy IX"));
  }

  printf("match_test: %d checks passed\n", passed);
  return 0;
}
