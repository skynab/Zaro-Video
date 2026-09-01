#!/usr/bin/env bash
#
# Rebuild every derived branding file from the two masters.
#
# What is hand-made and what is not: the three files in $SRC below are drawn by
# somebody, and everything this script writes is derived from them. So a new
# icon revision means dropping the masters in and running this, not opening
# thirteen files in an image editor.
#
#   resources/branding/regenerate.sh <asset-directory>
#
# The asset directory is expected to hold, by these names:
#   CutReel-V2-1024px.png     the application mark, square, transparent
#   CutReel-V2-512px.ico      the same mark as a Windows icon, all sizes
#   Installer Copy-512px.ico  the installer's mark as a Windows icon
#   Installer Copy-512px.png  the same installer mark, square, transparent
#
# macOS only: iconutil builds the .icns and ships with Xcode. ImageMagick is
# the other requirement -- brew install imagemagick.
set -euo pipefail

SRC="${1:?usage: regenerate.sh <asset-directory>}"
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

MASTER="$SRC/CutReel-V2-1024px.png"
INSTALLER_MASTER="$SRC/Installer Copy-512px.png"
for f in "$MASTER" "$INSTALLER_MASTER" \
         "$SRC/CutReel-V2-512px.ico" "$SRC/Installer Copy-512px.ico"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

# The Windows icons are used as they were drawn: an .ico already carries every
# size, and rebuilding one from a PNG would throw away the hinting whoever made
# it did at 16 and 32 pixels.
cp "$SRC/CutReel-V2-512px.ico"     "$OUT/CutReel.ico"
cp "$SRC/Installer Copy-512px.ico" "$OUT/CutReel-Installer.ico"

# The window icon, one file per size. See app/CMakeLists.txt.
for size in 16 32 48 64 128 256 512; do
    magick "$MASTER" -resize "${size}x${size}" -strip "PNG32:$OUT/CutReel-${size}.png"
done

# The logo on the bootstrapper .exe's one page. 64 pixels because that is the
# box WixStandardBootstrapperApplication's theme draws it in, and the theme does
# not scale what it is given.
magick "$INSTALLER_MASTER" -resize 64x64 -strip "PNG32:$OUT/CutReel-Installer-64.png"

# The macOS bundle icon. iconutil wants exactly these names and rejects the
# directory outright -- with no word on which file it objected to -- if one is
# missing or misnamed.
ICONSET="$WORK/CutReel.iconset"
mkdir -p "$ICONSET"
for pair in 16:icon_16x16 32:icon_16x16@2x 32:icon_32x32 64:icon_32x32@2x \
            128:icon_128x128 256:icon_128x128@2x 256:icon_256x256 512:icon_256x256@2x \
            512:icon_512x512 1024:icon_512x512@2x; do
    size="${pair%%:*}"
    name="${pair#*:}"
    magick "$MASTER" -resize "${size}x${size}" -strip "PNG32:$ICONSET/$name.png"
done
iconutil -c icns "$ICONSET" -o "$OUT/CutReel.icns"

# The two installer bitmaps. Sizes fixed by WixUI, light grounds because WixUI
# draws its text over them in black, art kept left of x=135 on the dialog
# because that is where its title and body text start.
python3 - "$MASTER" "$WORK" <<'PY'
import sys
from PIL import Image

master, work = sys.argv[1], sys.argv[2]
src = Image.open(master).convert('RGBA')
WHITE = (255, 255, 255)
TINT = (236, 234, 243)

def place(canvas, size, box):
    logo = src.resize((size, size), Image.LANCZOS)
    canvas.paste(logo, box, logo)

banner = Image.new('RGB', (493, 58), WHITE)
place(banner, 46, (493 - 46 - 14, 6))
banner.save(f'{work}/banner.png')

dialog = Image.new('RGB', (493, 312), WHITE)
dialog.paste(Image.new('RGB', (128, 312), TINT), (0, 0))
place(dialog, 96, (16, 108))
dialog.save(f'{work}/dialog.png')
PY
# BMP3 and TrueColor: WiX reads a plain 24-bit bitmap and nothing newer.
magick "$WORK/banner.png" -type TrueColor "BMP3:$OUT/installer-banner.bmp"
magick "$WORK/dialog.png" -type TrueColor "BMP3:$OUT/installer-dialog.bmp"

echo "wrote:"
ls -1 "$OUT"
