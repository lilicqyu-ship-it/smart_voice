#!/bin/zsh
# Regenerate the CJK UI font (main/fonts/font_cjk_20.c).
#
# Requires: node/npx (any recent version) and network access for npx to fetch
# lv_font_conv. Node is NOT installed system-wide here: download a portable
# build into /tmp first, e.g.:
#   curl -L https://nodejs.org/dist/v22.14.0/node-v22.14.0-darwin-arm64.tar.gz | tar xz -C /tmp
#   export PATH=/tmp/node-v22.14.0-darwin-arm64/bin:$PATH
#
# Coverage: ASCII + CJK punctuation + full GB2312 (6763 hanzi) at 20px / 2bpp.
set -e

FONT_OTF=${1:-/tmp/fontgen/SourceHanSansSC-Regular.otf}
OUT_DIR="$(dirname "$0")/../main/fonts"

if [ ! -f "$FONT_OTF" ]; then
  curl -sL --max-time 180 "https://github.com/adobe-fonts/source-han-sans/raw/release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf" -o "$FONT_OTF"
fi

python3 - <<'EOF'
chars = []
for cp in list(range(0x3000, 0x3040)) + list(range(0xFF01, 0xFF65)) + \
          [0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2026,0x00B7,0x00D7,0x00B0,0x2116,0xFFE5,0x2103]:
    chars.append(chr(cp))
for hi in range(0xB0, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.append(bytes([hi, lo]).decode('gb2312'))
        except (UnicodeDecodeError, UnicodeError):
            pass
seen = set(); out = []
for c in chars:
    if c not in seen:
        seen.add(c); out.append(c)
open('/tmp/fontgen_symbols.txt', 'w', encoding='utf-8').write(''.join(out))
print("symbols:", len(out))
EOF

npx --yes lv_font_conv@1.5.3 \
  --font "$FONT_OTF" --size 20 --bpp 2 --format lvgl \
  --no-compress --no-prefilter \
  -r 0x20-0x7E \
  --symbols "$(cat /tmp/fontgen_symbols.txt)" \
  --lv-font-name font_cjk_20 \
  -o "$OUT_DIR/font_cjk_20.c"

echo "written: $OUT_DIR/font_cjk_20.c"
