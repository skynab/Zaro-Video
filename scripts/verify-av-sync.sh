#!/usr/bin/env bash
# Measures audio/video sync in a rendered file, in samples.
#
# The fixture puts a white flash and an audio click on the same frame, once a
# second. After a render they must still be on the same frame: this extracts
# both independently from the output file and reports the offset between them.
#
# This is the harness the playback engine will be measured against in the next
# phase. It exists now because sync is not something to start checking once
# playback already feels wrong.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cut_tool="${ZARO_CUT:-$root/build/release/bin/zaro-cut}"
render_tool="${ZARO_RENDER:-$root/build/release/bin/zaro-render}"
fixture="$root/testdata/media/sync_click_flash.mov"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

for tool in "$cut_tool" "$render_tool"; do
    [[ -x "$tool" ]] || { echo "build the tools first (missing $tool)"; exit 2; }
done
[[ -f "$fixture" ]] || { echo "missing fixture -- run testdata/generate.sh"; exit 2; }

echo "rendering the sync fixture..."
"$cut_tool" "$work/sync.zaro" "$fixture" > /dev/null
"$render_tool" "$work/sync.zaro" "$work/out.mov" --quiet || exit 1

# Per-frame mean luma: a flash frame is far brighter than the black between.
ffmpeg -hide_banner -loglevel error -i "$work/out.mov" -map 0:v \
    -f rawvideo -pix_fmt gray "$work/video.raw"
# Mono float samples, so a click is a burst of large absolute values.
ffmpeg -hide_banner -loglevel error -i "$work/out.mov" -map 0:a \
    -ac 1 -f f32le -acodec pcm_f32le "$work/audio.raw"

probe() { ffprobe -v error -select_streams "$1" -show_entries stream="$2" -of csv=p=0 "$work/out.mov"; }
width=$(probe v:0 width)
height=$(probe v:0 height)
rate=$(probe v:0 r_frame_rate)
sample_rate=$(probe a:0 sample_rate)

python3 - "$work/video.raw" "$work/audio.raw" "$width" "$height" "$rate" "$sample_rate" <<'PY'
import sys, struct

video_path, audio_path, width, height, rate_text, sample_rate = sys.argv[1:]
width, height, sample_rate = int(width), int(height), int(sample_rate)
num, den = (int(x) for x in rate_text.split('/')) if '/' in rate_text else (int(rate_text), 1)

frame_bytes = width * height
video = open(video_path, 'rb').read()
frames = len(video) // frame_bytes

# A flash frame: mean luma far above the black background.
means = []
for i in range(frames):
    block = video[i * frame_bytes:(i + 1) * frame_bytes]
    means.append(sum(block) / len(block))
threshold = (max(means) + min(means)) / 2
flashes = [i for i, m in enumerate(means) if m > threshold]

audio = open(audio_path, 'rb').read()
samples = struct.unpack(f'<{len(audio) // 4}f', audio)
peak = max(abs(s) for s in samples) if samples else 0.0
# A low gate on purpose. The click is a 1kHz tone burst, so a 50% threshold is
# not crossed until a twelfth of a cycle in -- about four samples -- and that
# lag would be reported as a sync error that is really a detector artefact.
gate = peak * 0.02

# Click onsets: the first loud sample after a run of quiet ones.
clicks = []
quiet_for = sample_rate // 4
since = quiet_for
for i, s in enumerate(samples):
    if abs(s) > gate:
        if since >= quiet_for:
            clicks.append(i)
        since = 0
    else:
        since += 1

print(f"  {frames} video frames, {len(samples)} audio samples")
print(f"  {len(flashes)} flashes, {len(clicks)} clicks")

if not flashes or not clicks:
    print("FAIL: could not find flashes or clicks to compare")
    sys.exit(1)

# Expected sample count for this many frames, computed exactly.
expected_samples = frames * sample_rate * den // num
drift = len(samples) - expected_samples
print(f"  audio length: {len(samples)} samples, {expected_samples} expected -- drift {drift}")

worst = 0
pairs = min(len(flashes), len(clicks))
for flash, click in zip(flashes[:pairs], clicks[:pairs]):
    flash_sample = flash * sample_rate * den // num
    offset = click - flash_sample
    worst = max(worst, abs(offset))
    if abs(offset) > 0:
        print(f"    frame {flash}: click is {offset:+d} samples from the flash")

print(f"  worst offset: {worst} samples ({worst / sample_rate * 1000:.3f} ms)")

# One sample of slack for the click's own onset landing inside the frame.
failed = abs(drift) > 0 or worst > 1
print()
print("FAIL: audio and video are not aligned" if failed else
      f"Aligned: {pairs} flash/click pairs within {worst} sample(s), 0 samples of drift.")
sys.exit(1 if failed else 0)
PY
