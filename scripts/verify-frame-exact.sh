#!/usr/bin/env bash
# Compares zaro-frame's decoded output against FFmpeg's, byte for byte.
#
# This is the Phase 1 exit criterion. The frame ladder fixture proves seeks land
# on the right frame using nothing but our own code; this proves the pixels we
# hand back are the same pixels FFmpeg's own decoder produces, which is a
# stronger and entirely independent claim.
#
# Both sides write raw packed planes in the file's native pixel format, so
# nothing is compared through a colour conversion that could mask a difference.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
frame_tool="${ZARO_FRAME:-$root/build/debug/bin/zaro-frame}"
media="$root/testdata/media"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

[[ -x "$frame_tool" ]] || { echo "build zaro-frame first (looked in $frame_tool)"; exit 2; }

# file : native pixel format : frame indices to check
cases=(
    "ladder_prores.mov:yuv422p10le:0 1 7 42 137 150 151 298 299"
    "ladder_h264.mp4:yuv420p:0 1 11 12 13 47 200 250 299"
    "ladder_hevc.mp4:yuv420p:0 1 12 47 133 200 299"
    "vfr_sample.mp4:yuv420p:0 1 6 7 20 50 100 150"
    "pattern_2997df.mov:yuv420p:0 1 29 30 100 300 598"
    "sync_click_flash.mov:yuv420p:0 24 25 26 100 249"
)

total=0
failures=0

for entry in "${cases[@]}"; do
    file="${entry%%:*}"
    rest="${entry#*:}"
    pix="${rest%%:*}"
    frames="${rest#*:}"

    if [[ ! -f "$media/$file" ]]; then
        echo "skip  $file (missing -- run testdata/generate.sh)"
        continue
    fi

    file_failures=0
    for n in $frames; do
        total=$((total + 1))

        ffmpeg -hide_banner -loglevel error -y -i "$media/$file" \
            -vf "select='eq(n,$n)'" -fps_mode passthrough -frames:v 1 \
            -f rawvideo -pix_fmt "$pix" "$work/reference.raw" 2>/dev/null

        "$frame_tool" "$media/$file" "$n" "$work/ours.raw" --raw --software --quiet 2>/dev/null

        if [[ ! -s "$work/reference.raw" ]]; then
            echo "  FAIL $file frame $n: ffmpeg produced nothing to compare against"
            file_failures=$((file_failures + 1))
        elif ! cmp -s "$work/reference.raw" "$work/ours.raw"; then
            echo "  FAIL $file frame $n: $(stat -f%z "$work/ours.raw" 2>/dev/null || stat -c%s "$work/ours.raw") bytes ours vs $(stat -f%z "$work/reference.raw" 2>/dev/null || stat -c%s "$work/reference.raw") ffmpeg"
            file_failures=$((file_failures + 1))
        fi
    done

    count=$(echo "$frames" | wc -w | tr -d ' ')
    if [[ $file_failures -eq 0 ]]; then
        printf "  ok    %-22s %2d frames identical (%s)\n" "$file" "$count" "$pix"
    fi
    failures=$((failures + file_failures))
done

echo
if [[ $failures -eq 0 ]]; then
    echo "$total frames byte-identical to FFmpeg."
else
    echo "$failures of $total frames differ."
fi
exit $((failures > 0))
