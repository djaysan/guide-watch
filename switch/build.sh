#!/usr/bin/env bash
# Builds GuideWatch.ovl inside the devkitpro/devkita64 Docker image.
# Usage: ./build.sh                       (build)
#        ./build.sh clean                 (any other args are passed to make)
#        ./build.sh deploy                (push GuideWatch.ovl to the Switch over wifi)
#        ./build.sh deploy FILE /sd/dir/  (push any file to a folder on the card)
#        ./build.sh log                   (print the overlay's log from the Switch)
# Host unit tests need no Docker: run `make test` directly on the Mac.
# Wifi push needs sys-ftpd on the Switch and deploy/ftp.env (gitignored):
#   FTP_HOST=... FTP_PORT=5000 FTP_USER=... FTP_PASS=...
set -eu
cd "$(dirname "$0")"

# shellcheck disable=SC1091
[ -f deploy/ftp.env ] && . deploy/ftp.env  # credentials stay out of git
ftp_url() {  # ftp_url /path/on/card -> ftp://host:port/path
  : "${FTP_HOST:?deploy/ftp.env missing}" "${FTP_USER:?}" "${FTP_PASS:?}"
  echo "ftp://$FTP_HOST:${FTP_PORT:-5000}$1"
}
case "${1:-}" in
  deploy)
    src="${2:-GuideWatch.ovl}"; dst="${3:-/switch/.overlays/}"
    url=$(ftp_url "${dst// /%20}")  # spaces in card paths must be percent-encoded for curl
    curl -g -sS --max-time 120 --user "$FTP_USER:$FTP_PASS" --ftp-create-dirs -T "$src" "$url"
    echo "pushed $src -> $dst ($(stat -f %z "$src") bytes)"
    curl -g -s --max-time 20 --user "$FTP_USER:$FTP_PASS" "$url" | grep -F "$(basename "$src")" || echo "WARNING: not listed after upload"
    exit 0 ;;
  log)
    url=$(ftp_url /config/guidewatch/guidewatch.log)
    curl -s --max-time 20 --user "$FTP_USER:$FTP_PASS" "$url" || echo "(no log yet: the overlay has not been launched)"
    exit 0 ;;
esac

# libultrahand is fetched (not vendored) at the commit the spike was verified against
LIBULTRAHAND_COMMIT=2e4df5430045217093dd77751cd9e16c92fe9144
if [ ! -d libultrahand ]; then
  git init -q libultrahand
  git -C libultrahand fetch -q --depth 1 https://github.com/ppkantorski/libultrahand.git "$LIBULTRAHAND_COMMIT"
  git -C libultrahand checkout -q FETCH_HEAD
fi

IMAGE=devkitpro/devkita64:latest
# Runs natively on Apple Silicon; fall back to amd64 emulation if the native run fails
PLATFORM=""
docker run --rm "$IMAGE" true >/dev/null 2>&1 || PLATFORM="--platform linux/amd64"

# The stock image already has every portlib needed (minizip headers ship in switch-zlib)
docker run --rm $PLATFORM -v "$PWD":/work -w /work "$IMAGE" make "$@"

if [ -f GuideWatch.ovl ]; then ls -l GuideWatch.ovl; fi
