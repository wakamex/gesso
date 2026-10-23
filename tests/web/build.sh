#!/usr/bin/env bash
# Builds tests/web/page.c with Emscripten into OUT_DIR (default tests/web/out), for check.py.
#   tests/web/build.sh [OUT_DIR]       (needs emsdk: EMSDK, or ~/.local/opt/emsdk)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../src
out=${1:-$here/out}
source "${EMSDK:-$HOME/.local/opt/emsdk}/emsdk_env.sh" >/dev/null 2>&1
font=
for f in "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf" /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf; do
    [ -f "$f" ] && font=$f && break
done
test -n "$font" || { echo "no font to embed" >&2; exit 1; }
mkdir -p "$out"
emcc -O1 -std=c11 -I"$src" -I"$src/../vendor/stb" -I"$src/../vendor/kb" "$here/page.c" \
    "$src/gs_ui.c" "$src/gs_text.c" "$src/gs_draw.c" "$src/gs_stb.c" "$src/gs_kb.c" "$src/gs_pix.c" "$src/gs_jobs.c" \
    -sUSE_SDL=3 -sALLOW_MEMORY_GROWTH -sENVIRONMENT=web --embed-file "$font@/font.ttf" -o "$out/index.html"
echo "$out/index.html"
