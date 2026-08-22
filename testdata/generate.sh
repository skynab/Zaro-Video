#!/usr/bin/env bash
# Generates the media fixtures the media tests run against.
#
# These are not checked in. They are deterministic given the same FFmpeg build,
# and regenerating is cheap, which beats carrying a few hundred megabytes of
# binaries in git history forever.
#
#   ./testdata/generate.sh            small fixtures only
#   ./testdata/generate.sh --with-perf  also the 4K ProRes performance clip
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/media"
mkdir -p "$out"

FF="${FFMPEG:-ffmpeg}"
q() { "$FF" -hide_banner -loglevel error -y "$@"; }

# --- Frame ladder -------------------------------------------------------------
# Every frame is a flat field whose luma encodes its own frame number, so a
# decoded frame can be identified from its pixels alone. An off-by-one in
# seeking shows up as a value that is off by one, which is about as legible as
# a test failure gets.
#
# 300 frames, luma = 16 + 4*(n mod 55), chroma pinned to neutral.
#
# The step is 4 rather than 1 on purpose. Lossy codecs perturb a flat field by a
# code value or two, so with unit steps a one-frame seek error is indistinguishable
# from ordinary compression noise -- the test would pass while the decoder
# quietly returned the wrong frame. Four is comfortably outside that noise.
ladder='geq=lum=16+4*mod(N\,55):cb=128:cr=128'

echo "  ladder_prores.mov   (ProRes 422, exact luma)"
q -f lavfi -i "color=c=black:s=320x240:r=25:d=12" \
  -vf "$ladder,format=yuv422p10le" -frames:v 300 \
  -c:v prores_ks -profile:v 3 "$out/ladder_prores.mov"

echo "  ladder_h264.mp4     (H.264, lossy)"
q -f lavfi -i "color=c=black:s=320x240:r=25:d=12" \
  -vf "$ladder,format=yuv420p" -frames:v 300 \
  -c:v libx264 -preset veryfast -crf 12 -g 12 "$out/ladder_h264.mp4"

echo "  ladder_hevc.mp4     (HEVC)"
q -f lavfi -i "color=c=black:s=320x240:r=25:d=12" \
  -vf "$ladder,format=yuv420p" -frames:v 300 \
  -c:v libx265 -preset veryfast -crf 12 -x265-params log-level=error:keyint=12 \
  -tag:v hvc1 "$out/ladder_hevc.mp4" 2>/dev/null \
  || q -f lavfi -i "color=c=black:s=320x240:r=25:d=12" \
       -vf "$ladder,format=yuv420p" -frames:v 300 \
       -c:v hevc_videotoolbox -q:v 60 -g 12 -tag:v hvc1 "$out/ladder_hevc.mp4"

# --- Broadcast rate with drop-frame timecode ---------------------------------
echo "  pattern_2997df.mov  (29.97 with 01:00:00;00 start timecode)"
q -f lavfi -i "testsrc2=s=640x360:r=30000/1001:d=20" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=20" \
  -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \
  -c:a aac -b:a 128k \
  -timecode '01:00:00;00' -shortest "$out/pattern_2997df.mov"

# --- A/V sync reference -------------------------------------------------------
# A white frame and an audio click land on the same frame, every second. This is
# the fixture the playback engine's sync harness will measure against in Phase 3;
# it exists now so that decode-side timestamp handling is already covered.
echo "  sync_click_flash.mov (flash + click every 25 frames)"
q -f lavfi -i "color=c=black:s=320x240:r=25:d=10" \
  -f lavfi -i "aevalsrc=0.9*sin(2*PI*1000*t)*lt(mod(t*25\,25)\,1):s=48000:d=10" \
  -vf "geq=lum='if(lt(mod(N\,25)\,1)\,235\,16)':cb=128:cr=128,format=yuv420p" \
  -frames:v 250 \
  -c:v libx264 -preset veryfast -crf 10 -g 25 \
  -c:a pcm_s16le "$out/sync_click_flash.mov"

# --- Camera shake -------------------------------------------------------------
# A detailed pattern seen through a window that jitters: the content is still,
# the framing is not, which is what hand-held footage is. The pattern is drawn
# from X and Y alone with no frame number in it -- a moving test pattern, which
# is what the obvious sources are, would put motion in the content and there
# would be no way to tell a stabiliser's mistakes from the fixture's. The jitter is two
# sines with different periods, so it is neither periodic at the frame rate nor
# random -- a stabiliser can be measured against it because the shake is known.
#
# Cropped out of a larger frame so the jitter never reaches an edge and there is
# always real picture to track, rather than a border sliding in.
echo "  shaky_texture.mov   (still content, shaky framing)"
q -f lavfi -i "color=c=black:s=400x320:r=25:d=4" \
  -vf "geq=lum='128+90*sin(X/9)*cos(Y/7)+40*sin((X+Y)/3)':cb=128:cr=128,crop=320:240:x='40+8*sin(n*0.9)':y='40+6*cos(n*1.3)',format=yuv420p" \
  -frames:v 100 -c:v libx264 -preset veryfast -crf 10 -g 25 "$out/shaky_texture.mov"

# --- Variable frame rate ------------------------------------------------------
# Dropping three frames out of every ten leaves uneven PTS deltas -- 1/30s
# within a run, 4/30s across the gap -- which is what phone footage looks like
# and what breaks any code that assumes frame index times a constant duration.
# Note select's frame counter is lowercase `n`; geq's is uppercase `N`.
echo "  vfr_sample.mp4      (irregular PTS)"
q -f lavfi -i "color=c=black:s=320x240:r=30:d=10" \
  -vf "$ladder,select='gt(mod(n,10),2)',format=yuv420p" \
  -fps_mode vfr -c:v libx264 -preset veryfast -crf 15 "$out/vfr_sample.mp4"

# --- Audio only ---------------------------------------------------------------
# The sine source generates at 1/8 scale, so it is brought up to full scale here:
# a fixture used to test peak detection should actually have peaks.
echo "  tone_48k.wav        (stereo, full scale, distinct tone per channel)"
q -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=5" \
  -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=5" \
  -filter_complex "[0:a]volume=8[l];[1:a]volume=8[r];[l][r]join=inputs=2:channel_layout=stereo[a]" \
  -map "[a]" -c:a pcm_s16le "$out/tone_48k.wav"

# --- Performance clip ---------------------------------------------------------
if [[ "${1:-}" == "--with-perf" ]]; then
    echo "  perf_4k_prores.mov  (3840x2160 ProRes 422, ~300MB)"
    q -f lavfi -i "testsrc2=s=3840x2160:r=30000/1001:d=5" \
      -vf format=yuv422p10le -c:v prores_ks -profile:v 3 \
      "$out/perf_4k_prores.mov"
fi

echo
echo "fixtures in $out:"
ls -lh "$out" | tail -n +2 | awk '{printf "  %-24s %s\n", $9, $5}'
