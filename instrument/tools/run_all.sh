#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# CYMA-1, everything (spec §2.3, §8): the unit tests, the host and browser builds, every scenario script and golden
# gate, the wasm smoke, SPEC.json and docs/SPEC.md, the demos (WAV, mp3, GIF, synced MP4), the screens' contact sheet,
# the web package and, when asked, the browser suites, the device package (built, never flashed) and its device-time
# gates in the chip emulator.
#
#   instrument/tools/run_all.sh [--copy DIR] [--device] [--perf] [--chrome] [--artifact] [--no-demos] [--no-web]
#                               [--quick] [--evidence DIR]
#
#   --copy DIR     build and test an rsync'd copy at DIR/instrument (a module agent's private copy; the device build
#                  needs DIR inside the project, which tools/build_device.sh mounts). Default: this tree, in place
#   --device       also tools/build_device.sh APP --no-ble [--release X.Y[.Z], fm1.toml's release]: every check must print
#                  ok, the image must fit fm1pkg's app slot (581,564 B), the identity must be fm1.toml's id_prefix (in
#                  the build log and in the package), and build/SHA256SUMS is written (docs/SPEC.md quotes it, checked
#                  against the files)
#   --perf         also (implies --device) tools/emu_perf.py: the package, unmodified, in fm1-emu at the FM-1's 205
#                  instructions/us; gates on the slowest audio block, xruns, the UI frame, note latency and idle
#                  redraws (about 3 minutes; docker). Its figures go into docs/SPEC.md
#   --chrome       also tools/chrome_test.mjs on dist/web in headless Chrome: switch-on, keys, mouse, lean, knobs,
#                  capture, MIDI, the phrase, themes, 1280 / 2x / 390 px, a sandboxed frame, 48 kHz, file://, the dev
#                  build, and the error paths, every one asserted (about 40 s)
#   --artifact     also tools/artifact_test.mjs: the page converted to the artifact shape, in a cross-site sandboxed
#                  frame under the host's CSP with its downloads runtime stubbed (about 25 s); then artifact_sb: the
#                  same suite on the package that gets published, built with the artifact's URL as its share base
#                  (CYMA_SHARE_BASE overrides it), under build/run_all so dist/ is untouched (about 20 s)
#   --no-demos     skip tools/render_demos.sh;  --no-web: skip the emcc build (and so the wasm checks)
#   --quick        smoke and controls only, no demos (the gates they cannot feed are SKIP, not FAIL)
#   --evidence DIR where the evidence (results, SPEC, contact sheet, logs, a GIF, screenshots, the device-time report)
#                  is copied (default APP/build/evidence/qa, or the source tree's with --copy)
#
# Order: the harness's own selftests and script lint, the builds, the unit tests, the scenarios and gates, the spec
# sheet, the contact sheet, the demos, the web package, Chrome, the artifact frame, the device, the device-time gates,
# the spec sheet again (with the package and the device time). Each step's output goes to APP/build/run_all/STEP.log;
# the summary prints PASS / FAIL / SKIP per step and the gate tallies, and is saved as summary.txt. Exit 0 when
# nothing failed. A failing gate is a FAIL of its step, and the summary says which gates and why.
#
# CYMA-1 Sounds (docs/CYMA-1-sounds-spec.md §11): sounds_vectors (the frozen vectors re-derived, the scenario scripts
# current), unit_xfer + xfer_asan (T1, the second under ASan/UBSan), sounds_scen (T2 and T3 in the host simulator),
# sounds_ring (the USB send ring: fm1-base's own usb.c / ota.c / dispatch around app_sysex, floods judged against the
# platform alone), sounds_smoke (T4, app.wasm in node, with the 10^5-frame fuzz), sounds_cli (T7, the CLI against the
# virtual FM-1), and with --chrome sounds_chrome (T5); T6 is part of the artifact step (and of artifact_sb, on the
# share-base package). Each is SKIP (not FAIL) while its subject has not landed yet (no tools/cyma1_params.json,
# src/cy_xfer.c, app_sysex, Sounds exports, tools/cyma_sounds.py or #sndsave).
#
# Safety: nothing here touches USB, flashes, installs or sends MIDI to hardware. The device step only compiles; the
# emulator runs the package in a container, against no hardware at all.
set -u
TOOLS=$(cd "$(dirname "$0")" && pwd -P)
SRC_APP=$(dirname "$TOOLS")
ROOT=$SRC_APP
while [ "$ROOT" != "/" ] && [ ! -d "$ROOT/reference/fm1-base" ]; do ROOT=$(dirname "$ROOT"); done
[ -d "$ROOT/reference/fm1-base" ] || ROOT=$(dirname "$SRC_APP")   # (the project root: instrument/'s parent)
# fm1-base: FM1_BASE, else the Team EG platform that v1.1.1 and later are built on, platform/fm1-base-teg (made by
# tools/get_platform.sh from the pinned upstream and platform/patches; its TEG_FORK.md: the main loop's USB-retry
# stall and the boot's MASTER level fixed), else reference/fm1-base (upstream: not the release's package). Exported,
# so tools/build_device.sh and the font tools build the same tree
if [ -n "${FM1_BASE:-}" ]; then B="$FM1_BASE"
elif [ -d "$ROOT/platform/fm1-base-teg/tools" ]; then B="$ROOT/platform/fm1-base-teg"
else B="$ROOT/reference/fm1-base"; fi
export FM1_BASE="$B"
PY=python3
[ -x "$ROOT/.venv312/bin/python" ] && PY="$ROOT/.venv312/bin/python"
export DYLD_LIBRARY_PATH="/opt/homebrew/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

COPY="" DEVICE=0 PERF=0 CHROME=0 ARTIFACT=0 DEMOS=1 WEB=1 QUICK=0 EVID=""
while [ $# -gt 0 ]; do
    case "$1" in
        --copy) COPY="$2"; shift 2 ;;
        --device) DEVICE=1; shift ;;
        --perf) PERF=1; DEVICE=1; shift ;;
        --chrome) CHROME=1; shift ;;
        --artifact) ARTIFACT=1; shift ;;
        --no-demos) DEMOS=0; shift ;;
        --no-web) WEB=0; shift ;;
        --quick) QUICK=1; DEMOS=0; shift ;;
        --evidence) EVID="$2"; shift 2 ;;
        -h|--help) sed -n '2,46p' "$0"; exit 0 ;;
        *) echo "run_all: unknown option $1"; exit 2 ;;
    esac
done

APP=$SRC_APP
if [ -n "$COPY" ]; then
    mkdir -p "$COPY/instrument"
    rsync -a --delete --exclude build --exclude dist "$SRC_APP/" "$COPY/instrument/" || { echo "run_all: rsync failed"; exit 2; }
    APP=$(cd "$COPY/instrument" && pwd -P)
fi
[ -n "$EVID" ] || EVID="$SRC_APP/build/evidence/qa"
OUT="$APP/build/run_all"
rm -rf "$OUT"
mkdir -p "$OUT" "$EVID"
SUM="$OUT/summary.txt"
: > "$SUM"
FAILED=0
T_ALL=$(date +%s)
echo "run_all: $APP"
echo "         fm1-base $B, python $PY"

# step NAME CMD...: run, log, record. A step's exit 0 is PASS, 77 is SKIP (with its reason last in the log)
step() {
    name="$1"; shift
    t0=$(date +%s)
    "$@" > "$OUT/$name.log" 2>&1
    rc=$?
    t1=$(date +%s)
    if [ $rc -eq 0 ]; then st=PASS; elif [ $rc -eq 77 ]; then st=SKIP; else st=FAIL; FAILED=1; fi
    why=""
    [ $st = PASS ] || why=$(grep -v '^\s*$' "$OUT/$name.log" | tail -1 | cut -c1-110)
    printf '  %-4s %-18s %4ss  %s\n' "$st" "$name" "$((t1 - t0))" "$why" | tee -a "$SUM"
    return $rc
}
skip() { printf '  %-4s %-18s %4s   %s\n' SKIP "$1" "-" "$2" | tee -a "$SUM"; }

# ---- unit tests (each module's tests/unit/test_*.c; tests/plates/test_plates.py), run after the host build so
# build/gen exists. Each test names its own sources in the cc line of its header comment (spec §2.3:
# "cc ... tests/unit/test_X.c src/....c"): those are linked, nothing else (a test may stub its neighbours). It runs
# from the app's parent directory, so the "instrument/..." paths a test writes evidence to stay inside this tree.
unit_cc() { cc -O2 -std=c99 -ffp-contract=off -DPLAT_HOST -I"$APP/src" -I"$B/include" -I"$APP" "$@"; }
unit_srcs() {
    awk 'NR <= 40 { if (!on && $0 ~ /(^|[ *])cc /) on = 1; if (on) { print; if ($0 !~ /\\[ \t]*$/) exit } }' "$1" |
        grep -o 'src/[A-Za-z0-9_]*\.c' | sort -u
}
unit_one() {
    t="$1"; exe="$OUT/$(basename "$t" .c)"
    srcs=$(unit_srcs "$t")
    set --
    for s in $srcs; do set -- "$@" "$APP/$s"; done
    if [ -d "$APP/build/gen" ]; then set -- -I"$APP/build/gen" "$@"; fi
    if [ -n "$srcs" ]; then
        echo "sources (from its header): $srcs"
        unit_cc -o "$exe" "$t" "$@" -lm 2> "$exe.cc.log" || { cat "$exe.cc.log"; echo "did not compile"; return 1; }
    elif ! unit_cc -o "$exe" "$t" -lm 2> "$exe.cc.log"; then
        unit_cc -o "$exe" "$t" "$APP"/src/*.c -lm 2>> "$exe.cc.log" || { cat "$exe.cc.log"; echo "did not compile"; return 1; }
    fi
    (cd "$(dirname "$APP")" && "$exe")
}
ui_stub_run() {
    "$PY" "$B/tools/build.py" "$APP/tests/ui_stub" --host || return 1
    mkdir -p "$APP/build/ui_stub"
    "$APP/tests/ui_stub/build/host/cyma-ui_host" "$APP/tests/ui_stub/regress.txt" "$APP/build/ui_stub"
}
run_units() {
    NU=0
    for t in "$APP"/tests/unit/test_*.c; do
        [ -f "$t" ] || continue
        NU=$((NU + 1))
        step "unit_$(basename "$t" .c | sed 's/^test_//')" unit_one "$t"
    done
    [ $NU -gt 0 ] || skip unit "no tests/unit/test_*.c yet"
    if [ -f "$APP/tests/plates/test_plates.py" ]; then
        step plates sh -c "cd '$APP' && '$PY' tests/plates/test_plates.py"
    else
        skip plates "no tests/plates/test_plates.py yet"
    fi
    # the UI's own regressions on the UI stub's fake engine (redraw budgets, 0 fps when silent, nodal sand, grains by
    # area): about 3 s. (A function, not sh -c: macOS strips DYLD_LIBRARY_PATH from /bin/sh, and the font generator
    # needs it for libraqm)
    if [ -f "$APP/tests/ui_stub/regress.txt" ]; then
        step ui_stub ui_stub_run
    else
        skip ui_stub "no tests/ui_stub/regress.txt"
    fi
}

# ---- 1b. the harness against itself (synthetic signals; no build): a red gate later means the instrument
step golden_selftest "$PY" "$TOOLS/golden.py" selftest
step emu_perf_selftest "$PY" "$TOOLS/emu_perf.py" selftest
step script_lint "$PY" "$TOOLS/golden.py" lint --app "$APP"
# CYMA-1 Sounds: the frozen vectors re-derived from tools/cyma1_params.json, the reference's checks (the exhaustive JSON
# round trip unless --quick, the 100,000-frame update-safety fuzz), vectors.h and the T2/T3 scripts current
sounds_vectors() {
    q=""
    [ $QUICK -eq 1 ] && q="--quick"
    # shellcheck disable=SC2086
    "$PY" "$APP/tests/sounds/gen_vectors.py" check --app "$APP" $q
}
if [ -f "$APP/tests/sounds/gen_vectors.py" ]; then step sounds_vectors sounds_vectors; else skip sounds_vectors "no tests/sounds/gen_vectors.py"; fi

# ---- 2. builds. The old binaries go first, so a failed build can never be tested by mistake
HOSTBIN="$APP/build/host/cyma-1_host"
rm -f "$HOSTBIN"
step build_host "$PY" "$B/tools/build.py" "$APP" --host
HOST_OK=0
[ -x "$HOSTBIN" ] && HOST_OK=1
WASM_OK=0
if [ $WEB -eq 1 ]; then
    rm -f "$APP/build/emu/app.wasm"
    step build_web "$PY" "$B/tools/build.py" "$APP" --web
    [ -f "$APP/build/emu/app.wasm" ] && WASM_OK=1
else
    skip build_web "--no-web"
fi
run_units
# CYMA-1 Sounds T1 again, built with ASan and UBSan: the 10^6-buffer fuzz of cy_unpack7 / cy_rec_get / cy_name_clean
xfer_asan() {
    [ -f "$APP/src/cy_xfer.c" ] || { echo "no src/cy_xfer.c yet (FW): SKIP"; return 77; }
    exe="$OUT/test_xfer_asan"
    cc -O1 -g -std=c99 -ffp-contract=off -DPLAT_HOST -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I"$APP/src" -I"$B/include" -I"$APP" -o "$exe" "$APP/tests/unit/test_xfer.c" "$APP/src/cy_xfer.c" \
        "$APP/src/params.c" "$APP/src/presets.c" -lm 2> "$exe.cc.log" || { cat "$exe.cc.log"; echo "did not compile"; return 1; }
    (cd "$(dirname "$APP")" && ASAN_OPTIONS=detect_leaks=0 "$exe" --fuzz 1000000)
}
if [ -f "$APP/tests/unit/test_xfer.c" ]; then step xfer_asan xfer_asan; else skip xfer_asan "no tests/unit/test_xfer.c"; fi

# CYMA-1 Sounds, the USB send ring (spec revision 2026-10-24b R4): tests/sounds/ring/harness.c, fm1-base's own usb.c,
# ota.c and main-loop dispatch (extracted verbatim from $B by extract.py at test time) around the real app_sysex. Its
# floods are judged against the same platform without app_sysex: the only test of app_sx_send's ring gate (the host
# simulator and the wasm have no ring). Built like the host simulator, at -O2; --quick (about 25 s)
ring_cc() {
    cc -O2 -ffp-contract=off -DPLAT_HOST -DFM1B_BACKUP=1 -I"$B/host" -I"$B/device" -I"$B/include" -I"$B/lib" \
        -I"$APP" -I"$APP/build/gen" "$@"
}
sounds_ring() {
    [ -f "$APP/app_sounds.c" ] || { echo "no app_sounds.c yet (FW): SKIP"; return 77; }
    R="$OUT/sounds_ring"
    rm -rf "$R"
    mkdir -p "$R/o"
    "$PY" "$APP/tests/sounds/ring/extract.py" --base "$B" --out "$R/gen" || { echo "extract.py failed: did fm1-base change?"; return 1; }
    set --
    for f in "$APP"/src/*.c; do
        o="$R/o/$(basename "$f" .c).o"
        ring_cc -std=c99 -w -I"$APP/src" -c "$f" -o "$o" 2>> "$R/cc.log" || { cat "$R/cc.log"; echo "$f did not compile"; return 1; }
        set -- "$@" "$o"
    done
    ring_cc -std=gnu99 -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
        -I"$R" -I"$APP/tests/sounds/ring" -o "$R/harness" "$APP/tests/sounds/ring/harness.c" "$@" -lm 2>> "$R/cc.log" ||
        { cat "$R/cc.log"; echo "harness.c did not compile"; return 1; }
    (cd "$R" && ./harness --quick)
}

golden_run() {   # (the app path has spaces: it stays one quoted word; the flags have none)
    w=""; o=""
    [ $WASM_OK -eq 1 ] || w="--no-wasm"
    [ $QUICK -eq 1 ] && o="--only=smoke,controls"
    # shellcheck disable=SC2086
    "$PY" "$TOOLS/golden.py" --app "$APP" $w $o
}

# ---- 3. the smoke script as-is (the one the BUILDING.md quotes), then every scenario and gate
if [ $HOST_OK -eq 1 ]; then
    mkdir -p "$APP/build/out"
    step smoke "$HOSTBIN" "$APP/tests/smoke.txt" "$APP/build/out"
    step golden golden_run
    # CYMA-1 Sounds T2 / T3: the SysEx protocol and the update-channel safety, through app_sysex in the host simulator
    if [ -f "$APP/tests/sounds_sysex.txt" ]; then
        step sounds_scen "$PY" "$APP/tests/sounds/gen_vectors.py" scen --app "$APP" --host "$HOSTBIN" --out "$APP/build/sounds_scen"
    else
        skip sounds_scen "no tests/sounds_sysex.txt"
    fi
    if [ -f "$APP/tests/sounds/ring/harness.c" ]; then step sounds_ring sounds_ring; else skip sounds_ring "no tests/sounds/ring"; fi
    step spec "$PY" "$TOOLS/spec.py" --app "$APP"
    if [ -d "$APP/build/golden/screens" ]; then
        step contact_sheet "$PY" "$TOOLS/contact_sheet.py" "$APP/build/golden/screens"
    else
        skip contact_sheet "no screens run"
    fi
    if [ $DEMOS -eq 1 ]; then
        step demos sh "$TOOLS/render_demos.sh" "$APP"
    else
        skip demos "--no-demos"
    fi
else
    for s in smoke golden spec contact_sheet demos; do skip $s "no host simulator (build_host failed)"; done
fi

# ---- 4. the web package (M6's tools/package_web.py: dist/web with SPEC and demos injected)
if [ -f "$APP/tools/package_web.py" ] && [ $WASM_OK -eq 1 ]; then
    step package_web sh -c "cd '$APP' && '$PY' tools/package_web.py"
else
    skip package_web "$([ -f "$APP/tools/package_web.py" ] && echo 'no app.wasm' || echo 'no tools/package_web.py yet')"
fi

# ---- 4b. CYMA-1 Sounds T4 (app.wasm's Sounds exports in node, with the fuzz) and T7 (the CLI, offline and against
# the virtual FM-1: tools/sx_bridge.mjs running this app.wasm; never a MIDI port)
sounds_smoke() {
    command -v node > /dev/null || { echo "no node"; return 77; }
    node "$TOOLS/sounds_smoke.mjs" --wasm "$APP/build/emu/app.wasm" --json "$OUT/sounds_smoke.json"
}
sounds_cli() {
    q=""
    [ $QUICK -eq 1 ] && q="--quick"
    # shellcheck disable=SC2086
    CYMA1_TEST_WASM="$APP/build/emu/app.wasm" "$PY" "$TOOLS/test_cyma_sounds.py" $q
}
if [ $WASM_OK -eq 1 ] && [ -f "$TOOLS/sounds_smoke.mjs" ]; then step sounds_smoke sounds_smoke; else skip sounds_smoke "no app.wasm"; fi
if [ -f "$TOOLS/test_cyma_sounds.py" ]; then step sounds_cli sounds_cli; else skip sounds_cli "no tools/test_cyma_sounds.py"; fi

# ---- 5. headless Chrome: M6's acceptance suite on the packaged page, every check asserted
host_hash() {
    "$PY" -c 'import json, sys
try: print(json.load(open(sys.argv[1]))["spec"].get("demo_hash_host") or "")
except Exception: print("")' "$APP/build/golden/results.json"
}
chrome_suite() {
    CH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    [ -x "$CH" ] || { echo "no Google Chrome"; return 77; }
    command -v node > /dev/null || { echo "no node"; return 77; }
    [ -f "$APP/dist/web/index.html" ] || { echo "no dist/web/index.html (package_web.py)"; return 77; }
    [ -f "$APP/build/emu/index.html" ] || { echo "no build/emu (the --web build)"; return 77; }
    # the root it serves: links to dist/web and build/emu, a dist-nowasm made here, and a tests/ it may write in
    R="$OUT/chrome_root"
    rm -rf "$R" "$OUT/chrome"
    mkdir -p "$R/instrument/build" "$R/tests"
    ln -s "$APP/dist" "$R/instrument/dist"
    ln -s "$APP/build/emu" "$R/instrument/build/emu"
    if ! "$PY" "$APP/tools/package_web.py" --app "$APP" --out "$R/instrument/dist-nowasm" --no-inline-wasm > "$OUT/chrome_nowasm.log" 2>&1; then
        tail -3 "$OUT/chrome_nowasm.log"
        echo "package_web.py --no-inline-wasm failed"
        return 1
    fi
    rm -f "$R/instrument/dist-nowasm/app.wasm"
    node "$TOOLS/chrome_test.mjs" "$R" "$OUT/chrome" $(host_hash)
    rc=$?
    n_ok=$(grep -c '^ok ' "$OUT/chrome.log" 2>/dev/null)
    n_bad=$(grep -c '^FAIL' "$OUT/chrome.log" 2>/dev/null)
    echo "chrome_test: $n_ok ok, $n_bad failed; screenshots in $OUT/chrome"
    return $rc
}
if [ $CHROME -eq 1 ]; then step chrome chrome_suite; else skip chrome "(--chrome to run)"; fi
# CYMA-1 Sounds T5: save / share / import / Keep it / the fake FM-1, in headless Chrome on dist/web (--mute-audio)
sounds_chrome() {
    [ -x "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" ] || { echo "no Google Chrome"; return 77; }
    command -v node > /dev/null || { echo "no node"; return 77; }
    [ -f "$APP/dist/web/index.html" ] || { echo "no dist/web/index.html (package_web.py)"; return 77; }
    node "$TOOLS/sounds_chrome_test.mjs" "$APP" "$OUT/sounds_chrome"
}
if [ $CHROME -eq 1 ] && [ -f "$TOOLS/sounds_chrome_test.mjs" ]; then step sounds_chrome sounds_chrome; else skip sounds_chrome "(--chrome to run)"; fi

# ---- 5b. the page as its publishing target frames it: artifact shape, cross-site sandbox, CSP, host downloads
artifact_suite() {
    CH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    [ -x "$CH" ] || { echo "no Google Chrome"; return 77; }
    command -v node > /dev/null || { echo "no node"; return 77; }
    # the published folder itself when package_web.py wrote it (dist/artifact: already in the artifact shape, which
    # the suite then only wraps in the host's skeleton); else dist/web, converted
    d="$APP/dist/artifact"
    [ -f "$d/index.html" ] || d="$APP/dist/web"
    [ -f "$d/index.html" ] || { echo "no dist/artifact or dist/web (package_web.py)"; return 77; }
    echo "artifact suite on $d"
    node "$TOOLS/artifact_test.mjs" "$d" "$OUT/artifact"
}
if [ $ARTIFACT -eq 1 ]; then step artifact artifact_suite; else skip artifact "(--artifact to run)"; fi
# ---- 5c. the package that actually gets published: built with the artifact's own URL as its share base (Copy link
# then gives <base>#cy2.…, within claude.ai's 128-character anchor), contract-checked, and the frame suite run on it.
# It is written under build/run_all, so dist/ stays the no-base package the suites above tested (CYMA_SHARE_BASE
# overrides the URL)
SHARE_BASE_PUB="${CYMA_SHARE_BASE:-https://claude.ai/artifact/CLw6XpfjKrVVbpJJEY4tX7}"
artifact_sb_suite() {
    [ -x "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" ] || { echo "no Google Chrome"; return 77; }
    command -v node > /dev/null || { echo "no node"; return 77; }
    d="$OUT/artifact_sb_pkg"
    rm -rf "$d" "$OUT/artifact_sb"
    "$PY" "$APP/tools/package_web.py" --app "$APP" --out "$d/web" --artifact "$d/artifact" --share-base "$SHARE_BASE_PUB" ||
        { echo "package_web.py --share-base failed"; return 1; }
    "$PY" "$APP/tools/package_web.py" --app "$APP" --out "$d/web" --artifact "$d/artifact" --check ||
        { echo "package_web.py --check failed on the share-base package"; return 1; }
    grep -q "CY_SHARE_BASE:set\*/\"$SHARE_BASE_PUB\"" "$d/artifact/index.html" || { echo "the share base is not in the artifact page"; return 1; }
    echo "artifact suite on $d/artifact (share base $SHARE_BASE_PUB)"
    node "$TOOLS/artifact_test.mjs" "$d/artifact" "$OUT/artifact_sb"
}
if [ $ARTIFACT -eq 1 ] && [ $WASM_OK -eq 1 ]; then step artifact_sb artifact_sb_suite; else skip artifact_sb "(--artifact to run; needs app.wasm)"; fi

# ---- 6. the device package: built in the pinned toolchain, checks all ok, its identity and hash recorded, never flashed
APP_SLOT=581564      # fm1pkg.py APP_SLOT 0x8DFBC: what build.py enforces (spec §8.6's "570 KB" is 583,680 B, the looser)
device_build() {
    case "$APP/" in "$ROOT"/*) ;; *) echo "APP is outside $ROOT: tools/build_device.sh mounts only the project"; return 77 ;; esac
    # the release (fm1.toml's own release key, v1.1.1): fm1-base's build.py --release X.Y[.Z] gives the package the
    # identity id_prefix + XXYYZZ (FM-1_94010101 for 1.1.1, FM-1_94010100 for 1.1) and the screen reader's name
    # "CYMA-1 1.1.1", and names it cyma-1-X.Y.Z.fwsc; it is renamed build/cyma-1.fwsc, the name the README, SHA256SUMS
    # and the emulator tools use
    rel=$(sed -n 's/^release *= *"\([0-9.]*\)".*/\1/p' "$APP/fm1.toml" | head -1)
    rm -f "$APP/build/cyma-1.fwsc" "$APP/build/SHA256SUMS"
    [ -z "$rel" ] || rm -f "$APP/build/cyma-1-$rel.fwsc"
    if [ -n "$rel" ]; then set -- --release "$rel"; else set --; fi
    if ! "$ROOT/tools/build_device.sh" "$APP" --no-ble "$@"; then
        e=$(grep -m1 -E 'error:|truncated|undefined reference|multiple definition|FAIL' "$OUT/device.log" |
            sed 's|^.*/instrument/||' | cut -c1-140)
        echo "device build failed: ${e:-see device.log}"
        return 1
    fi
    if [ -n "$rel" ]; then
        [ -s "$APP/build/cyma-1-$rel.fwsc" ] || { echo "no cyma-1-$rel.fwsc (build.py --release $rel)"; return 1; }
        mv -f "$APP/build/cyma-1-$rel.fwsc" "$APP/build/cyma-1.fwsc"
    fi
    bad=$(grep -E '^\s+(FAIL|fail|warn|WARN)' "$OUT/device.log" | head -5)
    nok=$(grep -cE '^\s+ok\s' "$OUT/device.log")
    img=$(sed -n 's/.*ok *image \([0-9]*\) B.*/\1/p' "$OUT/device.log" | head -1)
    [ -s "$APP/build/cyma-1.fwsc" ] || { echo "no .fwsc"; return 1; }
    [ -z "$bad" ] || { echo "checks: $bad"; return 1; }
    [ -n "$img" ] && [ "$img" -le $APP_SLOT ] || { echo "image ${img:-?} B over the app slot (581,564 B)"; return 1; }
    # the identity: fm1.toml's id_prefix and a number, the same in the build log and inside the package itself
    pre=$(sed -n 's/^id_prefix *= *"\([^"]*\)".*/\1/p' "$APP/fm1.toml" | head -1)
    ident=$(sed -n 's/.*identity \([A-Za-z0-9_-]*\).*/\1/p' "$OUT/device.log" | tail -1)
    inpkg=$(SPEC_TOOLS="$TOOLS" "$PY" -c 'import os, sys
sys.path.insert(0, os.environ["SPEC_TOOLS"])
import spec
print(spec.package_identity(open(sys.argv[1], "rb").read()) or "")' "$APP/build/cyma-1.fwsc")
    case "$ident" in "$pre"[0-9]*) ;; *) echo "identity ${ident:-?} is not $pre<number> (fm1.toml id_prefix)"; return 1 ;; esac
    [ "$ident" = "$inpkg" ] || { echo "identity: the log says $ident, the package carries ${inpkg:-nothing}"; return 1; }
    # provenance: nothing authenticates a .fwsc, so its hash is recorded where the README and docs/SPEC.md read it
    (cd "$APP/build" && shasum -a 256 cyma-1.fwsc cyma-1.bin > SHA256SUMS) || { echo "SHA256SUMS not written"; return 1; }
    sha=$(cut -c1-64 "$APP/build/SHA256SUMS" | head -1)
    echo "$nok checks ok; image $img B (<= $APP_SLOT, the app slot); $(wc -c < "$APP/build/cyma-1.fwsc" | tr -d ' ') B package, identity $ident${rel:+ (release $rel)}, sha256 $sha (build/SHA256SUMS); never flashed"
}
DEVICE_OK=0
if [ $DEVICE -eq 1 ]; then
    step device device_build && DEVICE_OK=1
else
    skip device "(--device to build the .fwsc; never flashed)"
fi

# ---- 6b. device time: the package in fm1-emu at 205 MHz, timed from breakpoints (tools/emu_perf.py)
if [ $PERF -eq 1 ] && [ $DEVICE_OK -eq 1 ]; then
    step emu_perf "$PY" "$TOOLS/emu_perf.py" --app "$APP"
elif [ $PERF -eq 1 ]; then
    skip emu_perf "no device package (the device step did not pass)"
else
    skip emu_perf "(--perf to time the package in fm1-emu)"
fi

# ---- 6c. docs/SPEC.md again, now with the package's hash and the device time
if [ $DEVICE -eq 1 ] && [ $HOST_OK -eq 1 ]; then
    step spec_final "$PY" "$TOOLS/spec.py" --app "$APP"
fi

# ---- 7. evidence: what a reviewer needs, in one place
cp "$SUM" "$EVID/run_all_summary.txt" 2>/dev/null
for f in build/golden/results.json build/spec/SPEC.json docs/SPEC.md build/golden/contact_sheet.png \
         build/golden/wasm_smoke.json build/demos/demos.json build/SHA256SUMS; do
    [ -f "$APP/$f" ] && cp "$APP/$f" "$EVID/" 2>/dev/null
done
for d in chrome artifact artifact_sb; do           # this run's browser evidence only: an old run's is removed
    rm -rf "${EVID:?}/$d"
    if [ -d "$OUT/$d" ]; then
        mkdir -p "$EVID/$d"
        cp "$OUT/$d"/*.png "$OUT/$d"/*.json "$EVID/$d/" 2>/dev/null
    fi
done
rm -rf "${EVID:?}/sounds"                           # CYMA-1 Sounds: this run's T4 / T5 results and screenshots
mkdir -p "$EVID/sounds"
[ -f "$OUT/sounds_smoke.json" ] && cp "$OUT/sounds_smoke.json" "$EVID/sounds/"
[ -d "$OUT/sounds_chrome" ] && cp "$OUT/sounds_chrome"/*.png "$OUT/sounds_chrome"/*.json "$EVID/sounds/" 2>/dev/null
rm -rf "${EVID:?}/emu_perf"
if [ $PERF -eq 1 ] && [ -f "$APP/build/emu_perf/results.json" ]; then
    mkdir -p "$EVID/emu_perf"
    cp "$APP/build/emu_perf"/report.txt "$APP/build/emu_perf"/results.json "$APP/build/emu_perf"/*.png "$EVID/emu_perf/" 2>/dev/null
fi
mkdir -p "$EVID/logs"
cp "$OUT"/*.log "$EVID/logs/" 2>/dev/null
for s in "$APP"/build/golden/*/run.log; do
    [ -f "$s" ] && cp "$s" "$EVID/logs/scenario_$(basename "$(dirname "$s")").log"
done
G=$(ls "$APP"/build/demos/*.gif 2>/dev/null | head -1)
[ -n "$G" ] && cp "$G" "$EVID/" 2>/dev/null

# ---- summary
echo "" | tee -a "$SUM"
if [ -f "$OUT/golden.log" ]; then
    grep -E '^golden: [0-9]' "$OUT/golden.log" | tee -a "$SUM"
    grep -E '^  (FAIL|WARN)  ' "$OUT/golden.log" | sed 's/^/  /' | head -40 | tee -a "$SUM"
fi
if [ -f "$OUT/emu_perf.log" ]; then
    grep -E '^emu_perf: [0-9]' "$OUT/emu_perf.log" | tee -a "$SUM"
    grep -E '^  FAIL  ' "$OUT/emu_perf.log" | sed 's/^/  /' | head -20 | tee -a "$SUM"
fi
for t in chrome artifact artifact_sb; do
    [ -f "$OUT/$t.log" ] && grep -E '^FAIL ' "$OUT/$t.log" | sed "s/^/  $t: /" | head -12 | tee -a "$SUM"
done
for t in sounds_chrome sounds_smoke sounds_cli sounds_scen sounds_ring; do        # (CYMA-1 Sounds)
    [ -f "$OUT/$t.log" ] && grep -E '^ *FAIL' "$OUT/$t.log" | sed "s/^ */  $t: /" | head -12 | tee -a "$SUM"
done
echo "run_all: $([ $FAILED -eq 0 ] && echo 'everything passed' || echo 'SOME STEPS FAILED') in $(( $(date +%s) - T_ALL )) s; logs in $OUT; evidence in $EVID" | tee -a "$SUM"
echo "         (host build figures are algorithmic, device time is fm1-emu at 205 MHz: nothing here is measured on an FM-1; the .fwsc is never flashed)" | tee -a "$SUM"
exit $FAILED
