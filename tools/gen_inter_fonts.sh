#!/usr/bin/env bash
# Regenerate the Inter bitmap fonts used by the firmware UI.
#
#   tools/gen_inter_fonts.sh <dir-with-Inter-static-ttfs>
#
# The TTFs come from the Inter release zip (extras/ttf/), github.com/rsms/inter.
# Needs: node (npx lv_font_conv), python3 with fontTools.
#
# Two things the stock lv_font_conv output gets wrong for us, both fixed here:
#   1. Inter's default digits are proportional, so a live readout jitters as
#      it updates. tools/freeze_tnum.py bakes the tabular figures into cmap.
#   2. lv_font_conv emits LVGL 8 compatible C (see CLAUDE.md gotcha 4). The
#      python step below resolves its version #if blocks for LVGL 9 and adds
#      the fields LVGL 9 expects, matching the hand-patched font_*.c files.
set -euo pipefail

SRC=${1:?usage: $0 <dir-with-Inter-static-ttfs>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/firmware/src/fonts"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Text: ASCII + Latin-1 (Spanish accents, ·, °) + – — • ‹ › … ← ↑ → ↓ ⌫ ⚠ ✓
TEXT_RANGE="0x20-0x7E,0xA0-0xFF,0x2013,0x2014,0x2022,0x2039,0x203A,0x2026,0x2190-0x2193,0x232B,0x26A0,0x2713"
# The big readout only ever shows a number.
NUM_SYMBOLS="0123456789.,-–— "

for f in Inter-Regular Inter-Medium Inter-SemiBold InterDisplay-Regular; do
    python3 "$ROOT/tools/freeze_tnum.py" "$SRC/$f.ttf" "$TMP/$f.ttf"
done

gen() {  # name ttf size (--range R | --symbols S)
    local name=$1 ttf=$2 size=$3; shift 3
    npx -y lv_font_conv@1.5.3 --font "$TMP/$ttf.ttf" "$@" --size "$size" \
        --format lvgl --bpp 4 --no-compress --lv-include lvgl.h \
        -o "$TMP/$name.c"
    python3 - "$TMP/$name.c" "$OUT/$name.c" "$ttf" <<'PY'
import re, sys
src, dst, ttf = sys.argv[1:4]
lines = open(src).read().split("\n")

def truth(cond):
    def vc(m):
        a, b, c = (int(x) for x in m.group(1).split(","))
        return str((9, 2, 0) >= (a, b, c))
    cond = re.sub(r"LV_VERSION_CHECK\(([^)]*)\)", vc, cond)
    cond = cond.replace("LVGL_VERSION_MAJOR", "9").replace("LVGL_VERSION_MINOR", "2")
    cond = cond.replace("&&", " and ").replace("||", " or ")
    cond = re.sub(r"!(?!=)", " not ", cond)
    return eval(cond)

out, stack = [], []          # stack of (keep_this_branch, parent_keep)
for ln in lines:
    m = re.match(r"#if (.*(LVGL_VERSION|LV_VERSION_CHECK).*)$", ln)
    if m:
        parent = all(k for k, _ in stack) if stack else True
        stack.append((truth(m.group(1)), parent))
        continue
    if stack and ln.startswith("#else"):
        k, p = stack.pop(); stack.append((not k, p)); continue
    if stack and ln.startswith("#endif") and not ln.startswith("#endif /*#if"):
        stack.pop(); continue
    if stack and not all(k for k, _ in stack):
        continue
    out.append(ln)

text = "\n".join(out)
text = re.sub(r"(\.get_glyph_bitmap = [^\n]*\n)", r"\1    .release_glyph = NULL,\n", text)
text = re.sub(r"(\.subpx = [^\n]*\n)",
              r"\1    .kerning = LV_FONT_KERNING_NORMAL,\n    .static_bitmap = 0,\n", text)
text = text.replace("lv_font_fmt_txt_glyph_cache_t", "#error unpatched")
# Replace the converter's command line (it embeds temp paths) with provenance.
text = re.sub(r" \* Opts: .*", " * Source: " + ttf + ".ttf (Inter 4.1), tabular figures; tools/gen_inter_fonts.sh", text, count=1)
open(dst, "w").write(text)
PY
    echo "  $name.c"
}

echo "Generating Inter fonts into $OUT"
gen font_inter_12    Inter-Regular        12 -r "$TEXT_RANGE"
gen font_inter_14    Inter-Regular        14 -r "$TEXT_RANGE"
gen font_inter_md_14 Inter-Medium         14 -r "$TEXT_RANGE"
gen font_inter_md_16 Inter-Medium         16 -r "$TEXT_RANGE"
gen font_inter_md_20 Inter-Medium         20 -r "$TEXT_RANGE"
gen font_inter_sb_20 Inter-SemiBold       20 -r "$TEXT_RANGE"
gen font_inter_num_48 InterDisplay-Regular 48 --symbols "$NUM_SYMBOLS"
