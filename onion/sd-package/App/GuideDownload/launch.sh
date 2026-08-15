#!/bin/sh
# Guide Downloader for OnionOS (Miyoo Mini Plus)
#
# On-device version of guides.retromodlab.com: scans mapped folders in
# /mnt/SDCARD/Roms, and for each ROM without a "<name>.txt" guide it looks the
# game up in the same guides-index JSONs the web tool uses, then downloads the
# walkthrough from archive.org's in-7z extraction endpoint.
#
# Flow: "Scanning" panel immediately -> wifi check -> fast pre-scan (one awk
# per console folder, no per-file forks: the Mini Plus CPU takes minutes to
# fork thousands of subshells, which showed as a black screen) -> console
# picker (Onion's prompt tool: All, or a single console) -> download with a
# time estimate shown up front.
#
# Matching = same normalize() + FIXES table as the web app, exact and
# subtitle matching only. The fuzzy tier stays on the web tool; unmatched
# games are listed in guide-download.log.
#
# Guides are written into a "Guides" folder rather than beside the rom, which
# is what the in-game guide reader (and Allium) look for. See the GUIDES /
# LAYOUT / MIGRATE settings below.
#
# Needs wifi. Uses OnionOS's own curl + jq + infoPanel + prompt.

cd "$(dirname "$0")"
BIN=/mnt/SDCARD/.tmp_update/bin
ROMS=/mnt/SDCARD/Roms
SITE=https://guides.retromodlab.com/app/guides-index
ITEM=Gamespot_Gamefaqs_TXTs
LOG="$(pwd)/guide-download.log"
IDXCACHE=/tmp/guides-index
SCANLIST=/tmp/guide-scan.txt

# ---- where guides are written --------------------------------------------
#
# The in-game guide reader (and Allium) looks for a "Guides" folder beside
# the rom, mirroring the rom's path inside it:
#
#   per-console   Roms/GBA/Guides/Golden Sun.txt
#   shared        Roms/Guides/GBA/Golden Sun.txt
#
# Both layouts are read by guide_find(); pick whichever you prefer. A folder
# of .txt files named after the rom also works and is shown as a picker in
# the reader, which is why an existing directory counts as "already has one".
GUIDES=Guides
LAYOUT=per-console          # per-console | shared

# Move guides already sitting beside their rom into the Guides folder on the
# first run. Only a .txt whose basename matches a rom in the same folder is
# moved, so unrelated text files are left alone. Set to 0 to disable.
MIGRATE=1

# ---- progress panel ------------------------------------------------------
#
# infoPanel takes its text as a startup argument and has no way to change it
# afterwards, so every update means killing it and starting another one -
# which costs an SDL init and a theme load each time, and blinks the screen.
#
# So the panel is redrawn at most every PROGRESS_EVERY guides, or after
# PROGRESS_SECS seconds, whichever comes first. Raise either to make it
# quieter and slightly faster; PROGRESS_EVERY=1 updates on every guide and is
# noticeably flickery. 0 in both disables the indicator entirely.
PROGRESS_EVERY=8
PROGRESS_SECS=12

# Setting only one of them to 0 would otherwise mean "trigger on every item",
# which is the opposite of what 0 reads like. Both 0 = indicator off.
PROGRESS_OFF=0
[ "$PROGRESS_EVERY" -le 0 ] && [ "$PROGRESS_SECS" -le 0 ] && PROGRESS_OFF=1
[ "$PROGRESS_EVERY" -le 0 ] && PROGRESS_EVERY=999999
[ "$PROGRESS_SECS" -le 0 ] && PROGRESS_SECS=999999

# ---- cancel --------------------------------------------------------------
#
# Hold B to stop. detectKey samples the button state with EVIOCGKEY - it does
# not grab the input device, so nothing else on the system loses its input and
# there is no daemon to clean up if we exit early.
#
# It is checked once per guide, never per file: a folder can hold thousands of
# entries and a fork each would cost more than the downloads. Guides are one to
# two seconds apart, so the worst case is a short wait after you press.
#
# The hold is what makes it safe - a single sample would fire on a stray press
# from putting the device down. CANCEL_HOLD is in seconds, and the sleep only
# happens once B is actually down, so the normal path pays nothing for it.
CANCEL_BTN=29               # B = KEY_LEFTCTRL. MENU would be 1 (KEY_ESC).
CANCEL_HOLD=1

CURL="$BIN/curl -k -s -f -L --connect-timeout 15 --max-time 120"
JQ=$BIN/jq
PANEL=$BIN/infoPanel
PROMPT=$BIN/prompt
DETECT=$BIN/detectKey

panel_pid=""

# show a persistent status panel, replacing the previous one
show_panel() {
  close_panel
  if [ -x "$PANEL" ]; then
    $PANEL --title "Guide Downloader" --message "$1" --persistent &
    panel_pid=$!
  fi
}

# Ask the panel to go, then make sure the flag is NOT left set.
#
# infoPanel --persistent renders once and then spins on /tmp/dismiss_info_panel,
# clearing the flag itself when it sees it. Killing it instead leaves the flag
# behind, and the NEXT panel then finds it already set on startup and exits the
# moment it has drawn. With one or two panels per run that is invisible (the
# picture stays on screen because nothing redraws over it); with a progress
# indicator it happens dozens of times, so clear it deliberately.
close_panel() {
  if [ -n "$panel_pid" ]; then
    touch /tmp/dismiss_info_panel       # the polite exit: it clears this itself
    kill "$panel_pid" 2>/dev/null       # and the blunt one, in case it is mid-init
    wait "$panel_pid" 2>/dev/null
  fi
  panel_pid=""
  rm -f /tmp/dismiss_info_panel
}

# same console-folder -> index-platform mapping as the web app (FOLDER_MAP)
folder_platforms() {
  case "$1" in
    FC|NES|FAMICOM) echo "nes";;
    FDS) echo "famicomds nes";;
    SFC|SNES) echo "snes";;
    MD|GENESIS|MEGADRIVE|SMD) echo "genesis";;
    MDCD|SEGACD|MEGACD) echo "segacd";;
    SEGA32X|32X) echo "sega32x";;
    SMS|MASTERSYSTEM) echo "sms";;
    GG|GAMEGEAR) echo "gamegear sms";;
    GB) echo "gameboy";;
    GBC) echo "gbc gameboy";;
    GBA) echo "gba";;
    N64) echo "n64";;
    NDS|DS) echo "ds";;
    PS|PSX|PS1|PSONE) echo "ps";;
    PSP) echo "psp";;
    SATURN|SS) echo "saturn";;
    DC|DREAMCAST) echo "dreamcast";;
    PCE|PCENGINE|TG16) echo "tg16";;
    PCECD|TGCD) echo "turbocd tg16";;
    NGP|NGPC) echo "ngpc ngpocket ngp";;
    LYNX) echo "lynx";;
    WS|WONDERSWAN) echo "wonderswan wsc";;
    WSC) echo "wsc wonderswan";;
    VB|VIRTUALBOY) echo "virtualboy";;
    MSX|MSX2) echo "msx";;
    C64) echo "c64";;
    VIC20) echo "vic20";;
    A2600|ATARI2600) echo "atari2600";;
    A5200) echo "atari5200";;
    A7800) echo "atari7800";;
    A800) echo "atari8bit";;
    NEOGEO) echo "neo";;
    NEOCD) echo "neogeocd";;
    ODYSSEY) echo "odyssey2";;
    INTELLIVISION) echo "intellivision";;
    COLECO|COLECOVISION) echo "colecovision";;
    VECTREX) echo "vectrex";;
    SG1000) echo "sg1000";;
    3DO) echo "3do";;
    JAGUAR) echo "jaguar";;
    AMIGA) echo "amiga";;
    *) echo "";;
  esac
}

# Where this console folder's guides live. $1 = rom dir (trailing slash),
# $2 = folder name. Returns a path with a trailing slash.
guides_dir() {
  case "$LAYOUT" in
    shared) echo "$ROMS/$GUIDES/$2/";;
    *) echo "$1$GUIDES/";;
  esac
}

# count ROMs missing a guide in one folder - single awk pass, zero per-file
# forks. Keep the aux-extension list in sync with is_candidate() below.
#
# Two listings are piped in, separated by a marker: the rom folder, then the
# Guides folder. A rom counts as covered by "<base>.txt" or "<base>.md" in
# Guides, by a "<base>" directory in Guides (a multi-file guide), or by a
# legacy "<base>.txt" still sitting beside it.
count_missing() {
  { ls -1 "$1" 2>/dev/null; echo "@@GUIDES@@"; ls -1 "$2" 2>/dev/null; } | awk '
    BEGIN {
      inguides = 0;
      # direct assignments: split()+for-in init can silently fail on the
      # ancient busybox awk some devices ship. Keep in sync with is_candidate.
      # NOTE: cue is NOT aux here - for bin/cue games the cue IS the ROM.
      aux["bin"]=1; aux["sub"]=1; aux["img"]=1; aux["ccd"]=1; aux["txt"]=1;
      aux["sav"]=1; aux["srm"]=1; aux["state"]=1; aux["png"]=1; aux["jpg"]=1;
      aux["jpeg"]=1; aux["xml"]=1; aux["dat"]=1; aux["db"]=1; aux["cfg"]=1;
      aux["log"]=1;
    }
    $0 == "@@GUIDES@@" { inguides = 1; next }
    { if (inguides) guides[$0] = 1; else names[$0] = 1 }
    END {
      cnt = 0;
      for (f in names) {
        if (f ~ /^\./) continue;
        n = split(f, parts, ".");
        if (n < 2) continue;
        ext = tolower(parts[n]);
        if (ext in aux) continue;
        base = substr(f, 1, length(f) - length(parts[n]) - 1);
        if ((base ".txt") in guides) continue;
        if ((base ".md") in guides) continue;
        if (base in guides) continue;      # a directory of guide files
        if ((base ".txt") in names) continue;   # legacy, still beside the rom
        cnt++;
      }
      print cnt;
    }'
}

# true if this basename is a ROM we should find a guide for.
# cue is deliberately a candidate (for bin/cue games the cue IS the ROM);
# bin/img/sub/ccd stay skipped as disc data. Keep in sync with count_missing.
is_candidate() {
  case "$1" in .*) return 1;; esac
  ext=$(echo "${1##*.}" | tr 'A-Z' 'a-z')
  [ "$ext" = "$1" ] && return 1  # no extension
  case "$ext" in
    bin|sub|img|ccd|txt|sav|srm|state|png|jpg|jpeg|xml|dat|db|cfg|log) return 1;;
  esac
  return 0
}

# keep in sync with normalize() in guides-app build-index.py / app js:
# lowercase, -/_ -> space, strip (..) [..] tags, non-alnum -> space,
# drop the/a/an/and, roman numerals i..xvi -> digits
#
# Deliberately awk-free and regex-free. The device's busybox awk returned an
# empty string for every name here (2026-08-14: 68 candidates in, 0 out) while
# the plain awk in count_missing worked - the difference being the bracket
# regexes. Shell built-ins are the one thing that cannot vary between busybox
# builds, and this version forks once (tr) instead of once per name.
normalize() {
  s=$(echo "$1" | tr 'A-Z' 'a-z')

  # drop "(...)" and "[...]" tag groups, innermost-first, left to right
  while :; do
    case "$s" in
      *"("*")"*) s="${s%%"("*} ${s#*")"}";;
      *) break;;
    esac
  done
  while :; do
    case "$s" in
      *"["*"]"*) s="${s%%"["*} ${s#*"]"}";;
      *) break;;
    esac
  done

  # every character that is not a-z0-9 becomes a space
  rest="$s"; s=""
  while [ -n "$rest" ]; do
    c="${rest%"${rest#?}"}"
    rest="${rest#?}"
    case "$c" in
      [a-z0-9]) s="$s$c";;
      *) s="$s ";;
    esac
  done

  # drop stopwords, roman numerals -> digits (unquoted $s splits on spaces;
  # only a-z0-9 survive above, so there is nothing for the shell to glob)
  out=""
  for w in $s; do
    case "$w" in
      the|a|an|and) continue;;
      i) w=1;; ii) w=2;; iii) w=3;; iv) w=4;; v) w=5;; vi) w=6;;
      vii) w=7;; viii) w=8;; ix) w=9;; x) w=10;; xi) w=11;; xii) w=12;;
      xiii) w=13;; xiv) w=14;; xv) w=15;; xvi) w=16;;
    esac
    out="${out:+$out }$w"
  done
  printf '%s\n' "$out"
}

# hand-reviewed FIXES from the web app, keyed "firstPlatform|normalizedName".
# __SKIP__ = no correct guide exists in the dump (a lookalike would match).
fixes_lookup() {
  case "$1" in
    'gba|fire emblem binding blade') echo 'fire emblem fuuin no tsurugi';;
    'gba|medabots metabee version') echo 'medabots metabee';;
    'gba|medabots rokusho version') echo 'medabots rokusho';;
    'gba|spider man battle for new york'|'genesis|spot goes to hollywood'|'ps|destruction derby raw'|'gbc|asterix search for dogmatix') echo '__SKIP__';;
  esac
}

# fetch index json for a platform once per run; echo cached path or nothing
index_for() {
  idx="$IDXCACHE/$1.json"
  if [ ! -s "$idx" ]; then
    $CURL -o "$idx" "$SITE/$1.json" || rm -f "$idx"
  fi
  [ -s "$idx" ] && echo "$idx"
}

# look up a normalized key across the folder's platforms; echo "archive<TAB>path"
lookup() {
  key="$1"; platforms="$2"
  [ -z "$key" ] && return 1
  for p in $platforms; do
    idx=$(index_for "$p") || continue
    [ -z "$idx" ] && continue
    hit=$($JQ -r --arg k "$key" '.[$k] // empty | .[1] + "\t" + .[2]' "$idx")
    if [ -n "$hit" ]; then echo "$hit"; return 0; fi
  done
  return 1
}

# Move guides that predate the Guides layout into it. Only moves a .txt whose
# basename matches a rom sitting in the same folder - a loose readme.txt or
# credits.txt has no matching rom and is left where it is.
migrate_folder() {
  dir="$1"; gdir="$2"
  moved=0

  for t in "$dir"*.txt; do
    [ -f "$t" ] || continue
    tname=${t##*/}
    tbase=${tname%.txt}

    hasrom=0
    for r in "$dir$tbase".*; do
      [ -f "$r" ] || continue
      rname=${r##*/}
      [ "$rname" = "$tname" ] && continue
      if is_candidate "$rname"; then hasrom=1; break; fi
    done
    [ "$hasrom" -eq 1 ] || continue

    # Never delete: if both exist they may not be the same text (one edited,
    # one downloaded). Leave the loose copy alone and say so in the log - the
    # reader uses the one in Guides either way.
    if [ -f "$gdir$tname" ]; then
      echo "kept both: $folder/$tname (a guide of that name is already in $GUIDES/)" >> "$LOG"
      continue
    fi

    mkdir -p "$gdir"
    if mv "$t" "$gdir$tname"; then
      moved=$((moved + 1))
    else
      echo "MOVE FAILED: $folder/$tname" >> "$LOG"
    fi
  done

  [ "$moved" -gt 0 ] && echo "moved $moved existing guide(s) into ${gdir#$ROMS/}" >> "$LOG"
  return 0
}

# match + download every guide-less ROM in one console folder
process_folder() {
  dir="$1"
  folder=${dir%/}; folder=${folder##*/}
  platforms=$(folder_platforms "$(echo "$folder" | tr 'a-z' 'A-Z')")
  [ -z "$platforms" ] && return
  first_platform=${platforms%% *}
  gdir=$(guides_dir "$dir" "$folder")

  # diagnostic: what does this folder actually contain?
  n=0
  for e in "$dir"*; do
    n=$((n + 1))
    if [ "$n" -le 8 ]; then
      if [ -d "$e" ]; then t=dir; elif [ -f "$e" ]; then t=file; else t=other; fi
      echo "  entry($t): ${e##*/}" >> "$LOG"
    fi
  done
  nfiles=0; ncand=0

  for f in "$dir"*; do
    [ -f "$f" ] || continue
    nfiles=$((nfiles + 1))
    fname=${f##*/}
    is_candidate "$fname" || continue
    ncand=$((ncand + 1))
    base=${fname%.*}

    if [ -f "$gdir$base.txt" ] || [ -f "$gdir$base.md" ] || [ -d "$gdir$base" ] \
       || [ -f "$dir$base.txt" ]; then
      had=$((had + 1))
      continue
    fi

    if check_cancel; then
      cancelled=1
      break
    fi

    # counted here, before the skip/no-match paths, so every rom the pre-scan
    # counted as missing advances the bar exactly once
    ndone=$((ndone + 1))
    progress_tick "$folder" "$base"

    norm=$(normalize "$base")
    [ "$ncand" -le 3 ] && echo "  norm: [$base] -> [$norm]" >> "$LOG"
    if [ -z "$norm" ]; then
      echo "EMPTY NORMALIZE: $folder/$base" >> "$LOG"
      miss=$((miss + 1)); continue
    fi

    # FIXES table first, then exact, then subtitle halves ("Anthology - FF V")
    key="$norm"
    fix=$(fixes_lookup "$first_platform|$norm")
    if [ "$fix" = "__SKIP__" ]; then
      echo "SKIP (no correct guide in dump): $folder/$base" >> "$LOG"
      miss=$((miss + 1)); continue
    elif [ -n "$fix" ]; then
      key="$fix"
    fi

    hit=$(lookup "$key" "$platforms")
    if [ -z "$hit" ]; then
      case "$base" in
        *" - "*)
          hit=$(lookup "$(normalize "${base##* - }")" "$platforms")
          [ -z "$hit" ] && hit=$(lookup "$(normalize "${base%% - *}")" "$platforms")
          ;;
      esac
    fi
    if [ -z "$hit" ]; then
      echo "NOT MATCHED: $folder/$base (try guides.retromodlab.com)" >> "$LOG"
      miss=$((miss + 1)); continue
    fi

    archive=$(printf '%s\n' "$hit" | cut -f1)
    gpath=$(printf '%s\n' "$hit" | cut -f2)
    # local index says genN.7z; the archive.org file uses the long name
    remote="gamefaqs.gamespot.com.txt.faqs.$(printf '%s\n' "$archive" | sed 's/^gen\([0-9]\)\.7z$/\1/').gen.7z"
    encpath=$(printf '%s\n' "$gpath" | sed 's|/|%2F|g')
    url="https://archive.org/download/$ITEM/$ITEM%2F$remote/$encpath"

    tmp="/tmp/guide-dl.txt"
    if $CURL -o "$tmp" "$url" && [ -s "$tmp" ] && [ "$(head -c1 "$tmp")" != "<" ]; then
      mkdir -p "$gdir"
      mv "$tmp" "$gdir$base.txt"
      echo "OK: ${gdir#$ROMS/}$base.txt" >> "$LOG"
      new=$((new + 1))
    else
      rm -f "$tmp"
      echo "DOWNLOAD FAILED: $folder/$base" >> "$LOG"
      fail=$((fail + 1))
    fi
  done

  echo "folder $folder: $n entries, $nfiles files, $ncand candidates" >> "$LOG"
}

# ---- cancel --------------------------------------------------------------

cancelled=0

# Only advertise cancelling if the binary that implements it is actually
# there - promising a button that does nothing is worse than saying nothing.
cancel_hint=""
[ -x "$DETECT" ] && cancel_hint="Hold B to cancel"

# true when B has been held down for CANCEL_HOLD seconds
check_cancel() {
  [ -x "$DETECT" ] || return 1
  "$DETECT" "$CANCEL_BTN" || return 1        # exit 0 = pressed

  # down once - confirm it stays down, and say so on screen meanwhile so a
  # deliberate press gets feedback instead of an unexplained pause
  show_panel "Keep holding B to cancel...\n\nRelease to carry on."

  i=0
  while [ "$i" -lt "$CANCEL_HOLD" ]; do
    sleep 1
    if ! "$DETECT" "$CANCEL_BTN"; then
      # let go - put the progress panel back on the next tick
      prog_last=0
      prog_time=0
      return 1
    fi
    i=$((i + 1))
  done

  return 0
}

# ---- progress ------------------------------------------------------------
#
# ndone counts guides STARTED, so the name on screen is the one being fetched
# rather than the last one finished - the download is the slow part and the
# panel would otherwise sit on a finished name for a second or two.
ndone=0
prog_last=0
prog_time=0
prog_t0=0

# "[#####...............]" - ASCII on purpose: theme fonts are not guaranteed
# to carry block-drawing glyphs, and a missing glyph renders as a blank box.
progress_bar() {
  w=20
  filled=0
  [ "$2" -gt 0 ] && filled=$(($1 * w / $2))
  [ "$filled" -gt "$w" ] && filled=$w
  bar=""
  i=0
  while [ "$i" -lt "$filled" ]; do bar="$bar#"; i=$((i + 1)); done
  while [ "$i" -lt "$w" ]; do bar="$bar."; i=$((i + 1)); done
  printf '[%s]' "$bar"
}

# $1 = console folder, $2 = game being fetched
progress_tick() {
  [ "$PROGRESS_OFF" = "1" ] && return 0

  now=$(date +%s)

  # always draw the first one, then throttle
  if [ "$ndone" -gt 1 ] \
     && [ $((ndone - prog_last)) -lt "$PROGRESS_EVERY" ] \
     && [ $((now - prog_time)) -lt "$PROGRESS_SECS" ]; then
    return 0
  fi
  prog_last=$ndone
  prog_time=$now

  pct=0
  [ "$total" -gt 0 ] && pct=$((ndone * 100 / total))
  [ "$pct" -gt 100 ] && pct=100

  # ETA from measured rate, not a guess - and only once there is enough of a
  # sample for it not to swing wildly
  eta=""
  elapsed=$((now - prog_t0))
  if [ "$ndone" -gt 3 ] && [ "$elapsed" -gt 0 ]; then
    rem=$(((total - ndone) * elapsed / ndone))
    if [ "$rem" -gt 90 ]; then
      eta="   ~$(((rem + 59) / 60)) min left"
    elif [ "$rem" -gt 0 ]; then
      eta="   ~${rem}s left"
    fi
  fi

  game="$2"
  while [ ${#game} -gt 32 ]; do game="${game%?}"; done

  hint=""
  [ -n "$cancel_hint" ] && hint="\n$cancel_hint"

  show_panel "$(progress_bar "$ndone" "$total")  ${pct}%\n$ndone of $total$eta\n\n$1\n$game\n\nnew $new   no match $miss   failed $fail$hint"
}

# "12 guides" -> rough honest wall-clock text (about 1-2s per guide)
estimate() {
  secs=$(($1 * 2))
  if [ "$secs" -le 60 ]; then
    echo "under a minute"
  else
    echo "about $(((secs + 59) / 60)) min"
  fi
}

mkdir -p "$IDXCACHE"
: > "$LOG"
echo "$(date) start" >> "$LOG"

# feedback on screen from second one, so a slow card scan never looks dead
show_panel "Scanning your card..."

# wifi / site reachability check
if ! $CURL --head -o /dev/null "$SITE/nes.json"; then
  close_panel
  [ -x "$PANEL" ] && $PANEL --title "Guide Downloader" \
    --message "Cannot reach guides.retromodlab.com.\nConnect to wifi first (Tweaks or quick menu)." --auto
  echo "ERROR: no connection to $SITE" >> "$LOG"
  exit 1
fi

# pre-scan: how many ROMs are missing a guide, per mapped console folder
: > "$SCANLIST"
total=0
for dir in "$ROMS"/*/; do
  folder=${dir%/}; folder=${folder##*/}
  [ -n "$(folder_platforms "$(echo "$folder" | tr 'a-z' 'A-Z')")" ] || continue
  gdir=$(guides_dir "$dir" "$folder")

  # before counting, so a guide moved into place isn't counted as missing
  [ "$MIGRATE" = "1" ] && migrate_folder "$dir" "$gdir"

  cnt=$(count_missing "$dir" "$gdir")
  if [ "$cnt" -gt 0 ]; then
    echo "$folder $cnt" >> "$SCANLIST"
    total=$((total + cnt))
  fi
done
echo "$(date) scan done: $total missing" >> "$LOG"

if [ "$total" -eq 0 ]; then
  close_panel
  [ -x "$PANEL" ] && $PANEL --title "Guide Downloader" \
    --message "Nothing to download.\nEvery ROM in a mapped folder already has a guide." --auto
  echo "DONE: nothing to download" >> "$LOG"
  exit 0
fi

# console picker: All, or one console (selection comes back as the exit code;
# B/cancel returns 255). Falls back to All if prompt is unavailable.
close_panel
want=""
if [ -x "$PROMPT" ]; then
  set -- "All consoles ($total missing)"
  while read -r fol cnt; do
    set -- "$@" "$fol ($cnt missing)"
  done < "$SCANLIST"
  LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so $PROMPT \
    -t "Guide Downloader" -m "Download guides for which console?" "$@"
  rc=$?
  lines=$(wc -l < "$SCANLIST")
  if [ "$rc" -eq 0 ]; then
    want=""
  elif [ "$rc" -le "$lines" ]; then
    want=$(sed -n "${rc}p" "$SCANLIST" | cut -d' ' -f1)
    total=$(sed -n "${rc}p" "$SCANLIST" | cut -d' ' -f2)
  else
    echo "cancelled at picker" >> "$LOG"
    exit 0
  fi
fi

start_hint=""
[ -n "$cancel_hint" ] && start_hint="\n\n$cancel_hint - it stops after the\nguide it is fetching, and running again\npicks up where it left off."

show_panel "Fetching up to $total guides ($(estimate $total)).\nGuides download one per second or two -\nplease leave the device on.\nLog: App/GuideDownload/guide-download.log$start_hint"

new=0; had=0; miss=0; fail=0
prog_t0=$(date +%s)
prog_time=$prog_t0

if [ -n "$want" ]; then
  process_folder "$ROMS/$want/"
else
  for dir in "$ROMS"/*/; do
    [ "$cancelled" = "1" ] && break
    process_folder "$dir"
  done
fi

close_panel
sleep 1

if [ "$cancelled" = "1" ]; then
  echo "$(date) CANCELLED at $ndone of $total: $new new, $miss not matched, $fail failed" >> "$LOG"
  [ -x "$PANEL" ] && $PANEL --title "Guide Downloader" \
    --message "Cancelled at $ndone of $total.\n\nNew guides: $new\nNot matched: $miss  Failed: $fail\n\nRun again to pick up where\nthis left off." --auto
  exit 0
fi

echo "$(date) DONE: $new new, $had already had one, $miss not matched, $fail failed" >> "$LOG"

[ -x "$PANEL" ] && $PANEL --title "Guide Downloader" \
  --message "New guides: $new\nAlready had one: $had\nNot matched: $miss  Failed: $fail\nDetails: guide-download.log" --auto
exit 0
