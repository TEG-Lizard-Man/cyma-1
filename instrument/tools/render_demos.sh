#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# CYMA-1 demos (spec §8.4): each tests/demos/*.txt through the host simulator, rendered offline by the same C code
# the FM-1 and the browser run. For each demo NAME, into APP/build/demos:
#   NAME.wav   24-bit stereo at 44.1 kHz, master 4096 (the exact output words: the app's wav24 tap). Never processed:
#              the honest engine output, at the level the engine made it
#   NAME.mp4   the screen and the sound in one file, in sync: the screen every 125 ms of the capture (tools/golden.py
#              expand adds the shots) at 8 frames a second, scaled 2x with nearest neighbour (so 4:2:0 chroma keeps
#              one colour per LCD pixel: no smeared gold grains), H.264 (yuv420p, -tune animation, CRF 23), and AAC at
#              256 kbps (Apple's AudioToolbox encoder when ffmpeg has it) from the listening master (below),
#              +faststart. Frame k is the screen at k x 125 ms of the WAV, so the sand moves with what you hear
#   NAME.png   the MP4's poster: the screen two thirds of the way in, 480 x 480 (nearest neighbour)
#   NAME.mp3   320 kbps (ffmpeg, libmp3lame) from the listening master: the audio-only fallback for the page
#   NAME.gif   the same shots at 4 frames a second (every other one), the picture-only fallback
#   NAME.log   the simulator's output (expects: no NaN, peak under the ceiling)
# and demos.json (id, title, about, length, peak, files; and "listening", below) for the page's Listen section, and
# listening.json (the same loudness figures, by id).
#
# The listening copies (the MP3 and the MP4's sound) are loudness-matched, so the page's demo cards play at one
# level: the demos come out of the engine between -31 and -22 LUFS, a 9 LU spread. Each WAV is measured (ffmpeg's
# EBU R128 meter: ITU-R BS.1770-4 integrated loudness, gated, and its 4x oversampled true peak), turned to
# LUFS_TARGET (-18 LUFS integrated) by one fixed gain, and only where that gain would take a true peak over the
# master's ceiling (TP_MAX - 0.5 dB, room for the codecs' overshoot) a look-ahead true-peak limiter takes the excess
# off those peaks alone (1.5 ms look-ahead, so the gain is down before the peak arrives; 20 ms release, so it lets go
# between strokes; at most LIMIT_MAX dB, else the demo stays quieter instead and says so). What the limiter takes in
# loudness goes back as gain, measured, until the master reads the target. The MP3, the AAC and the finished MP4 are
# then decoded and measured again: each must read LUFS_TARGET +-1 LU with a true peak at most TP_MAX (-1 dBTP), or the
# master's ceiling comes down by the overshoot and they are encoded again (3 tries). The figures go into demos.json
# ("listening") and the log.
#
#   instrument/tools/render_demos.sh [APP_DIR] [--only NAME] [--keep-frames] [--gif-scale N] [--no-mp4]
#                                   [--lufs -18] [--tp-max -1]
#
# Needs the host simulator (python3 $FM1_BASE/tools/build.py APP_DIR --host) and ffmpeg (libx264 for the MP4); without
# ffmpeg the WAVs are still rendered and the other files are reported as skipped. The listening masters need numpy and
# scipy (the project's .venv312); without them the MP3 and MP4 carry the WAV's own level, and the run says so. Exit 0
# when every demo rendered cleanly and every listening copy met its loudness and true-peak target.
set -u
TOOLS=$(cd "$(dirname "$0")" && pwd -P)
APP=$(dirname "$TOOLS")
ONLY=""
KEEP=0
SCALE=1
MP4=1
SHOT_MS=125
LUFS_TARGET=-18
TP_MAX=-1
LIMIT_MAX=6
while [ $# -gt 0 ]; do
    case "$1" in
        --only) ONLY="$2"; shift 2 ;;
        --keep-frames) KEEP=1; shift ;;
        --gif-scale) SCALE="$2"; shift 2 ;;
        --no-mp4) MP4=0; shift ;;
        --lufs) LUFS_TARGET="$2"; shift 2 ;;
        --tp-max) TP_MAX="$2"; shift 2 ;;
        -h|--help) sed -n '2,39p' "$0"; exit 0 ;;
        *) APP=$(cd "$1" && pwd -P); shift ;;
    esac
done
PY=python3
d=$APP
while [ "$d" != "/" ]; do
    if [ -x "$d/.venv312/bin/python" ]; then PY="$d/.venv312/bin/python"; break; fi
    d=$(dirname "$d")
done
HOST="$APP/build/host/cyma-1_host"
OUT="$APP/build/demos"
[ -x "$HOST" ] || { echo "render_demos: no host simulator at $HOST (build.py $APP --host)"; exit 2; }
mkdir -p "$OUT"
FF=$(command -v ffmpeg || true)
[ -n "$FF" ] || echo "render_demos: no ffmpeg: WAVs only"
AAC=aac
if [ -n "$FF" ]; then
    "$FF" -hide_banner -encoders 2>/dev/null | grep -q ' aac_at ' && AAC=aac_at
    if [ $MP4 -eq 1 ] && ! "$FF" -hide_banner -encoders 2>/dev/null | grep -q ' libx264 '; then
        echo "render_demos: this ffmpeg has no libx264: no MP4s"
        MP4=0
    fi
fi
MATCH=1
if ! "$PY" -c 'import numpy, scipy.signal, scipy.io.wavfile' 2>/dev/null; then
    echo "render_demos: no numpy/scipy for $PY: the listening copies keep the WAV's own level (not loudness-matched)"
    MATCH=0
fi

# loud FILE: its sound's integrated loudness (LUFS) and true peak (dBTP), by ffmpeg's EBU R128 meter
loud() {
    "$FF" -hide_banner -nostats -i "$1" -map 0:a:0 -af ebur128=peak=true:framelog=quiet -f null - 2>&1 |
        awk '/^ *I:/ { i = $2 } /^ *Peak:/ { p = $2 } END { print (i == "" ? "nan" : i), (p == "" ? "nan" : p) }'
}

# listen_master IN.wav OUT.wav GAIN_DB CEIL_DBTP LIMIT_MAX_DB: the listening master, float32. One fixed gain; where
# that would take a true peak (4x oversampled) over the ceiling, a look-ahead limiter: the gain each sample needs is
# min(1, ceiling / its true peak); its minimum over +-1.5 ms, then the mean over the same +-1.5 ms (so the gain is
# down, smoothly, before and through every peak: each mean is over minima that all saw that peak); then a 20 ms
# release (a peak hold with an exponential fall: never above what the peaks need; at 80 ms, demo 05's strokes would be
# held down 19 % of the time instead of 4 %). Over LIMIT_MAX dB of limiting the gain is lowered instead. Prints the
# figures as one tab-separated line: gain, the limiter's most, the time it takes more than 0.5 dB, the master's true
# peak.
listen_master() {
    "$PY" - "$@" <<'PYEOF'
import math
import sys

import numpy as np
from scipy.io import wavfile
from scipy.ndimage import minimum_filter1d, uniform_filter1d
from scipy.signal import resample_poly

src, dst = sys.argv[1], sys.argv[2]
gain_db, ceil_db, limit_max = float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
fs, a = wavfile.read(src)
if a.dtype == np.int32:                  # 24-bit PCM, left-justified in int32 (scipy)
    x = a.astype(np.float64) / 2147483648.0
elif a.dtype == np.int16:
    x = a.astype(np.float64) / 32768.0
else:
    x = a.astype(np.float64)
if x.ndim == 1:
    x = x[:, None]
n = x.shape[0]


def true_peak(s):
    """per sample: the largest of its own magnitude and its 4x oversampled neighbourhood's, over the channels"""
    up = np.abs(resample_poly(s, 4, 1, axis=0)).max(axis=1)[: 4 * n]
    up = np.pad(up, (0, 4 * n - up.shape[0]))
    return np.maximum(np.abs(s).max(axis=1), up.reshape(n, 4).max(axis=1))


def limit(y, ceil_lin, fs):
    p = true_peak(y)
    if p.max() <= ceil_lin:
        return y, np.ones(n)
    need = np.minimum(1.0, ceil_lin / np.maximum(p, 1e-12))
    L = max(1, int(round(0.0015 * fs)))
    g = uniform_filter1d(minimum_filter1d(need, 2 * L + 1, mode="nearest"), 2 * L + 1, mode="nearest")
    g = np.minimum(g, need)              # (the mean of minima is already under every need; float rounding aside)
    r = 1.0 - g                          # the reduction, held and let go over 20 ms: R[k] = max_j<=k r[j] a^(k-j)
    lna = -1.0 / (0.020 * fs)
    k = np.arange(n, dtype=np.float64)
    with np.errstate(divide="ignore"):
        lr = np.where(r > 0.0, np.log(np.maximum(r, 1e-300)), -np.inf)
    R = np.exp(np.maximum.accumulate(lr - k * lna) + k * lna)
    g = 1.0 - np.minimum(R, 1.0)
    return y * g[:, None], g


ceil_lin = 10.0 ** (ceil_db / 20.0)
gain = gain_db
for _ in range(6):
    y0 = x * 10.0 ** (gain / 20.0)
    y, g = limit(y0, ceil_lin, fs)
    tp = true_peak(y).max()
    for _ in range(4):                   # the limited signal's own true peak, re-checked: tighten by any overshoot
        if tp <= ceil_lin * 1.0001:
            break
        y, g2 = limit(y, ceil_lin * ceil_lin / tp, fs)
        g = g * g2
        tp = true_peak(y).max()
    most = max(0.0, -20.0 * math.log10(max(g.min(), 1e-9)))
    if most <= limit_max + 1e-9:
        break
    gain -= most - limit_max             # more than LIMIT_MAX dB of limiting: quieter instead
held = float(np.count_nonzero(g < 10.0 ** (-0.5 / 20.0))) / fs
wavfile.write(dst, fs, y.astype(np.float32))
print(f"{gain:.2f}\t{most:.2f}\t{held * 1000.0:.0f}\t{20.0 * math.log10(max(tp, 1e-9)):.2f}")
PYEOF
}

FAIL=0
N=0
TSV="$OUT/listening.tsv"
: > "$TSV"
for SCRIPT in "$APP"/tests/demos/*.txt; do
    [ -f "$SCRIPT" ] || continue
    NAME=$(basename "$SCRIPT" .txt)
    [ -z "$ONLY" ] || [ "$ONLY" = "$NAME" ] || continue
    N=$((N + 1))
    FR="frames_$NAME"
    LD="$OUT/listen_$NAME"
    rm -rf "${OUT:?}/$FR" "${LD:?}" "$OUT/$NAME.wav" "$OUT/$NAME.mp3" "$OUT/$NAME.gif" "$OUT/$NAME.mp4" "$OUT/$NAME.png"
    mkdir -p "$OUT/$FR"
    # the shots go in as plain "shot" lines between the script's own, every 125 ms of simulated time
    if ! "$PY" "$TOOLS/golden.py" expand "$SCRIPT" --shots $SHOT_MS --prefix "$FR/f" > "$OUT/$NAME.script.txt" 2> "$OUT/$NAME.expand.log"; then
        echo "  FAIL $NAME: expand ($(cat "$OUT/$NAME.expand.log"))"
        FAIL=1
        continue
    fi
    T0=$(date +%s)
    "$HOST" "$OUT/$NAME.script.txt" "$OUT" > "$OUT/$NAME.log" 2>&1
    RC=$?
    T1=$(date +%s)
    INFO=$("$PY" "$TOOLS/golden.py" wavinfo "$OUT/$NAME.wav" 2>&1)
    WRC=$?
    FAILS=$(grep -c '^FAIL' "$OUT/$NAME.log")
    if [ $RC -ne 0 ] || [ $WRC -ne 0 ] || [ ! -s "$OUT/$NAME.wav" ]; then
        echo "  FAIL $NAME: exit $RC, $FAILS failed expects; $INFO (see $OUT/$NAME.log)"
        FAIL=1
    else
        echo "  ok   $INFO  [$((T1 - T0)) s]"
    fi
    case "$INFO" in *SILENT*) FAIL=1 ;; esac
    if [ -n "$FF" ] && [ -s "$OUT/$NAME.wav" ]; then
        TITLE=$(sed -n 's/^# TITLE: //p' "$SCRIPT" | head -1)
        mkdir -p "$LD"
        # ---- the listening master, then the MP3 and the AAC from it, measured as they will be heard
        SRC="$OUT/$NAME.wav"
        NOTE="rendered offline by the same C code (host build)"
        set -- $(loud "$OUT/$NAME.wav")
        I0=$1 TP0=$2
        if [ $MATCH -eq 1 ] && [ "$I0" != "nan" ] && [ "$I0" != "-inf" ] && [ "$I0" != "-70.0" ]; then
            GAIN=$(awk -v t="$LUFS_TARGET" -v i="$I0" 'BEGIN { printf "%.2f", t - i }')
            CEIL=$(awk -v t="$TP_MAX" 'BEGIN { printf "%.2f", t - 0.5 }')
            NOTE="$NOTE; loudness-matched listening copy ($LUFS_TARGET LUFS, true peak <= $TP_MAX dBTP): the WAV is the engine's exact output"
            SRC="$LD/listen.wav"
        fi
        TRY=0
        IM=nan TPM=nan IA=nan TPA=nan LIM="0.00 0.00 0 $TP0"
        while :; do
            if [ "$SRC" != "$OUT/$NAME.wav" ]; then
                # the master; where the limiter worked it took some loudness with the peaks, so the gain goes up by
                # what it took and the master is made again, until it reads the target within 0.1 LU (5 tries), or
                # the limiter is at its LIMIT_MAX (then listen_master has lowered the gain itself: as loud as it goes)
                MK=0 MOK=1
                while :; do
                    if ! LIM=$(listen_master "$OUT/$NAME.wav" "$SRC" "$GAIN" "$CEIL" "$LIMIT_MAX"); then
                        MOK=0
                        break
                    fi
                    set -- $LIM
                    GOT=$1
                    set -- $(loud "$SRC")
                    D=$(awk -v t="$LUFS_TARGET" -v i="$1" 'BEGIN { printf "%.2f", t - i }')
                    if awk -v d="$D" -v g="$GAIN" -v got="$GOT" 'BEGIN { exit !((d <= 0.1 && d >= -0.1) || got < g - 0.01) }' ||
                            [ $MK -ge 5 ]; then
                        break
                    fi
                    MK=$((MK + 1))
                    GAIN=$(awk -v g="$GAIN" -v d="$D" 'BEGIN { printf "%.2f", g + d }')
                done
                if [ $MOK -eq 0 ]; then
                    echo "  FAIL $NAME: the listening master"
                    FAIL=1
                    break
                fi
            fi
            OKA=1
            "$FF" -y -loglevel error -i "$SRC" -codec:a libmp3lame -b:a 320k -id3v2_version 3 \
                -metadata title="$TITLE" -metadata artist="CYMA-1" -metadata comment="$NOTE" "$OUT/$NAME.mp3" || OKA=0
            "$FF" -y -loglevel error -i "$SRC" -vn -c:a "$AAC" -b:a 256k "$LD/sound.m4a" || OKA=0
            if [ $OKA -eq 0 ]; then
                echo "  FAIL $NAME: mp3 / aac"
                FAIL=1
                break
            fi
            set -- $(loud "$OUT/$NAME.mp3") $(loud "$LD/sound.m4a")
            IM=$1 TPM=$2 IA=$3 TPA=$4
            [ "$SRC" = "$OUT/$NAME.wav" ] && break           # (not matched: nothing to meet)
            OVER=$(awk -v a="$TPM" -v b="$TPA" -v m="$TP_MAX" 'BEGIN { w = a > b ? a : b; printf "%.2f", w - m }')
            if awk -v o="$OVER" 'BEGIN { exit !(o <= 0) }' || [ $TRY -ge 3 ]; then
                break
            fi
            TRY=$((TRY + 1))
            CEIL=$(awk -v c="$CEIL" -v o="$OVER" 'BEGIN { printf "%.2f", c - o - 0.1 }')
        done
        set -- $LIM
        G=$1 LMAX=$2 LMS=$3 TPW=$4
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$NAME" "$I0" "$TP0" "$G" "$LMAX" "$LMS" "$TPW" \
            "$IM" "$TPM" "$IA" "$TPA" "$([ "$SRC" = "$OUT/$NAME.wav" ] && echo 0 || echo 1)" >> "$TSV"
        if [ "$SRC" != "$OUT/$NAME.wav" ]; then
            VERD=$(awk -v t="$LUFS_TARGET" -v m="$TP_MAX" -v im="$IM" -v ia="$IA" -v pm="$TPM" -v pa="$TPA" 'BEGIN {
                bad = ""
                if (pm > m || pa > m) bad = bad " true peak over " m " dBTP"
                d1 = im - t; d2 = ia - t; if (d1 < 0) d1 = -d1; if (d2 < 0) d2 = -d2
                if (d1 > 1 || d2 > 1) bad = bad " loudness off " t " LUFS by more than 1 LU"
                print bad }')
            echo "       listening: $I0 LUFS ${TP0} dBTP -> gain ${G} dB, limiter $LMAX dB max (${LMS} ms over 0.5 dB); mp3 $IM LUFS $TPM dBTP, aac $IA LUFS $TPA dBTP${VERD:+  FAIL:$VERD}"
            if [ -n "$VERD" ]; then
                FAIL=1
            fi
        fi
        NF=$(ls "$OUT/$FR" | wc -l | tr -d ' ')
        if [ "$NF" -gt 0 ]; then
            W=$((240 * SCALE))
            if ! "$FF" -y -loglevel error -framerate $((1000 / SHOT_MS)) -i "$OUT/$FR/f%04d.png" \
                    -vf "fps=4,scale=$W:$W:flags=neighbor,split[a][b];[a]palettegen=max_colors=128:stats_mode=full[p];[b][p]paletteuse=dither=bayer:bayer_scale=3" \
                    -loop 0 "$OUT/$NAME.gif"; then
                echo "  FAIL $NAME: gif"
                FAIL=1
            fi
            # the synced film: frame k is the screen at k x SHOT_MS of the WAV; its sound is the AAC measured above,
            # copied as it is, and both end where the WAV does (-t: with a copied sound, -shortest cut the picture up
            # to half a second early)
            DUR=$(printf '%s' "$INFO" | sed -n 's/^[^:]*: \([0-9][0-9.]*\) s,.*/\1/p')
            if [ -n "$DUR" ]; then set -- -t "$DUR"; else set -- -shortest; fi
            if [ $MP4 -eq 1 ] && [ -s "$LD/sound.m4a" ] && ! "$FF" -y -loglevel error -framerate $((1000 / SHOT_MS)) -i "$OUT/$FR/f%04d.png" \
                    -i "$LD/sound.m4a" -map 0:v:0 -map 1:a:0 \
                    -vf "scale=480:480:flags=neighbor,format=yuv420p" -c:v libx264 -preset slower -tune animation \
                    -crf 23 -c:a copy "$@" -movflags +faststart \
                    -metadata title="$TITLE" -metadata artist="CYMA-1" \
                    -metadata comment="$NOTE; picture and sound from one run" \
                    "$OUT/$NAME.mp4"; then
                echo "  FAIL $NAME: mp4"
                FAIL=1
            elif [ $MP4 -eq 1 ] && [ "$SRC" != "$OUT/$NAME.wav" ] && [ -s "$OUT/$NAME.mp4" ]; then
                set -- $(loud "$OUT/$NAME.mp4")     # the film as the page plays it: the same target
                if ! awk -v t="$LUFS_TARGET" -v m="$TP_MAX" -v i="$1" -v p="$2" 'BEGIN { d = i - t; if (d < 0) d = -d; exit !(d <= 1 && p <= m) }'; then
                    echo "  FAIL $NAME: the mp4's sound reads $1 LUFS $2 dBTP (target $LUFS_TARGET LUFS, <= $TP_MAX dBTP)"
                    FAIL=1
                fi
            fi
            PF=$(printf '%s/%s/f%04d.png' "$OUT" "$FR" $((NF * 2 / 3)))
            [ -f "$PF" ] && "$FF" -y -loglevel error -i "$PF" -vf "scale=480:480:flags=neighbor" "$OUT/$NAME.png"
        else
            echo "  FAIL $NAME: no frames"
            FAIL=1
        fi
    fi
    [ $KEEP -eq 1 ] || rm -rf "${OUT:?}/$FR"
    rm -rf "${LD:?}"
done
"$PY" "$TOOLS/golden.py" demos-json "$OUT" --scripts "$APP/tests/demos" >/dev/null || FAIL=1
# the loudness figures: listening.json by id (a --only run keeps the others'), and each demo's "listening" in demos.json
"$PY" - "$OUT" "$LUFS_TARGET" "$TP_MAX" <<'PYEOF' || FAIL=1
import json
import os
import sys

out, target, tp_max = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
path = os.path.join(out, "listening.json")
try:
    with open(path) as f:
        listening = json.load(f)
    if not isinstance(listening, dict):
        listening = {}
except (OSError, ValueError):
    listening = {}


def num(s):
    try:
        v = float(s)
        return v if v == v and abs(v) != float("inf") else None
    except ValueError:
        return None


with open(os.path.join(out, "listening.tsv")) as f:
    for line in f:
        c = line.rstrip("\n").split("\t")
        if len(c) != 12:
            continue
        listening[c[0]] = {
            "matched": c[11] == "1", "target_lufs": target, "true_peak_max_dbtp": tp_max,
            "wav_lufs": num(c[1]), "wav_true_peak_dbtp": num(c[2]),
            "gain_db": num(c[3]), "limiter_max_db": num(c[4]), "limiter_ms_over_half_db": num(c[5]),
            "master_true_peak_dbtp": num(c[6]),
            "mp3_lufs": num(c[7]), "mp3_true_peak_dbtp": num(c[8]),
            "aac_lufs": num(c[9]), "aac_true_peak_dbtp": num(c[10]),
            "meter": "ffmpeg ebur128 (ITU-R BS.1770-4, true peak 4x oversampled)",
        }
os.remove(os.path.join(out, "listening.tsv"))
with open(path, "w") as f:
    json.dump(listening, f, indent=1, sort_keys=True)
dj = os.path.join(out, "demos.json")
with open(dj) as f:
    demos = json.load(f)
for d in demos:
    if isinstance(d, dict) and d.get("id") in listening:
        d["listening"] = listening[d["id"]]
with open(dj, "w") as f:
    json.dump(demos, f, indent=1)
PYEOF
[ $N -gt 0 ] || { echo "render_demos: no demos in $APP/tests/demos"; exit 2; }
echo "render_demos: $N demos -> $OUT$([ $FAIL -eq 0 ] || echo ', SOME FAILED')"
exit $FAIL
