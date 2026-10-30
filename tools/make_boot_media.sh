#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# CYMA-1's boot from power-on, rendered offline by the host simulator (the same C code as the FM-1 and the browser):
#
#   media/boot-intro.gif   480 x 480, 25 fps, looping: the sand credit as Team EG's bell rings, its hold, its burst,
#                          the CYMA-1 title and tagline and their hold, the sand flowing into the sand table, then a
#                          second of the plate (a first boot: the tour's PLAY A KEY chip)
#   media/boot-intro.mp4   the same frames with the boot's own sound: H.264 and AAC
#
#   tools/make_boot_media.sh [HOST]      HOST: the host simulator (default instrument/build/host/cyma-1_host, from
#                                        python3 "$FM1_BASE/tools/build.py" instrument --host)
#
# The method is the one CYMA-1's other showcase media use:
# - a shot every 40 ms from t = 0 with no shotmode, so the splash runs as on a first boot;
# - frame k is the screen at k x 40 ms. The frames are named b00000.png, b00040.png, ... (five digits, so b10000
#   sorts after b09960) and taken by index, never by a sorted listing;
# - 2x nearest neighbour, so every LCD pixel is a sharp 2 x 2 block;
# - GIF: one palette over all frames (palettegen stats_mode=full, 256 colours), no dither, frame-difference
#   rectangles, gifsicle -O3 when it is installed;
# - MP4: libx264 yuv420p, -preset slower -tune animation, CRF 18, +faststart.
#
# The sound is the engine's own 24-bit output at MASTER full: the bell rings out by itself, about 2 s in. One fixed
# gain brings it to the other media's listening level, -18 LUFS integrated, unless that would take its true peak over
# -1.5 dBTP (then it stays quieter). Nothing else is done to it: the clip starts and ends in exact digital silence,
# so it needs no fades. Nothing here touches USB, MIDI or an FM-1.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
HOST=${1:-$ROOT/instrument/build/host/cyma-1_host}
MEDIA=$ROOT/media
END_MS=10160            # the chrome draws at 9150 ms (instrument/ui/ui.c, UI_SPL_END); then a second of the plate
STEP_MS=40              # 25 fps
LUFS=-18
TPMAX=-1.5
[ -x "$HOST" ] || { echo "make_boot_media: no host simulator at $HOST (build it first: BUILDING.md, step 5)" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "make_boot_media: ffmpeg not found" >&2; exit 2; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/out" "$WORK/seq" "$MEDIA"

# ---- the script: the audio from t = 0, a shot every 40 ms. "master" runs the host 40 ms (the pot's smoothing
# settling), so it stands in for the first frame's wait
{
    echo "# CYMA-1 boot from power-on: the audio (wav24, MASTER full) and a shot every $STEP_MS ms to $END_MS ms"
    echo "wav24 out:boot.wav"
    echo "shot b00000.png"
    echo "master 4096"
    for ((t = STEP_MS; t <= END_MS; t += STEP_MS)); do
        printf 'shot b%05d.png\nwait %d\n' "$t" "$STEP_MS"     # (the last wait: the audio runs to the last frame's end)
    done
    echo "wav24stop"
    echo "expect logo_end 2350"
    echo "expect boot_sound 1"
    echo "expect splash 0"
} > "$WORK/boot.txt"
"$HOST" "$WORK/boot.txt" "$WORK/out" > "$WORK/boot.log" 2>&1 || { cat "$WORK/boot.log" >&2; exit 1; }
if grep -q '^FAIL' "$WORK/boot.log"; then cat "$WORK/boot.log" >&2; exit 1; fi

# ---- the frames by index: f0000 is t = 0, fK is t = 40 K ms
n=0
for ((t = 0; t <= END_MS; t += STEP_MS)); do
    f=$(printf '%s/out/b%05d.png' "$WORK" "$t")
    [ -s "$f" ] || { echo "make_boot_media: no shot at $t ms" >&2; exit 1; }
    ln -s "$f" "$(printf '%s/seq/f%04d.png' "$WORK" "$n")"
    n=$((n + 1))
done
DUR=$(awk -v n="$n" -v s="$STEP_MS" 'BEGIN { printf "%.3f", n * s / 1000 }')

# ---- GIF
ffmpeg -y -loglevel error -framerate 25 -i "$WORK/seq/f%04d.png" -frames:v "$n" \
    -vf "scale=480:480:flags=neighbor,split[a][b];[a]palettegen=max_colors=256:stats_mode=full[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
    -loop 0 "$MEDIA/boot-intro.gif"
if command -v gifsicle >/dev/null 2>&1; then gifsicle -b -O3 "$MEDIA/boot-intro.gif"; fi

# ---- MP4: the audio cut to the frames' length (44.1 kHz: 1764 samples a frame), one fixed gain
loud() {   # FILE: integrated loudness and true peak
    ffmpeg -hide_banner -nostats -i "$1" -map 0:a:0 -af "ebur128=peak=true:framelog=quiet" -f null - 2>&1 |
        awk '/^ *I:/ { i = $2 } /^ *Peak:/ { p = $2 } END { print (i == "" ? "nan" : i), (p == "" ? "nan" : p) }'
}
ffmpeg -y -loglevel error -i "$WORK/out/boot.wav" -af "atrim=end_sample=$((n * 1764))" -c:a pcm_s24le "$WORK/boot-cut.wav"
set -- $(loud "$WORK/boot-cut.wav")
I=$1 TP=$2
G=$(awk -v t="$LUFS" -v i="$I" -v p="$TP" -v m="$TPMAX" 'BEGIN { g = t - i; if (p + g > m) g = m - p; printf "%.2f", g }')
AAC=aac
ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' aac_at ' && AAC=aac_at
ffmpeg -y -loglevel error -framerate 25 -i "$WORK/seq/f%04d.png" -i "$WORK/boot-cut.wav" \
    -filter_complex "[0:v]scale=480:480:flags=neighbor,format=yuv420p[v];[1:a]volume=${G}dB[a]" \
    -map "[v]" -map "[a]" -frames:v "$n" -c:v libx264 -preset slower -tune animation -crf 18 \
    -c:a "$AAC" -b:a 192k -t "$DUR" -movflags +faststart -metadata title="CYMA-1 boot" -metadata artist="CYMA-1" \
    -metadata comment="rendered offline by the host simulator; picture and sound from one run" "$MEDIA/boot-intro.mp4"

echo "make_boot_media: $n frames, $DUR s; the boot's sound $I LUFS, $TP dBTP, gain $G dB -> mp4 $(loud "$MEDIA/boot-intro.mp4")"
for f in "$MEDIA/boot-intro.gif" "$MEDIA/boot-intro.mp4"; do
    printf '  %-20s %9s B\n' "$(basename "$f")" "$(wc -c < "$f" | tr -d ' ')"
done
