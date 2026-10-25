#!/bin/sh
# Build an fm1-base app into a device .fwsc package, entirely inside the pinned JieLi toolchain container.
#
#   tools/build_device.sh APP_DIR [build.py options...]      e.g.  tools/build_device.sh build-test/template --no-ble
#   tools/build_device.sh --shell                             a shell in the same container (debugging)
#
# Build tooling only: this never touches USB, never flashes, never sends MIDI.
#
# How it works: fm1-base's tools/build.py runs *inside* a linux/amd64 container of the image built from
# reference/optimist/tools/docker/Dockerfile (JieLi toolchain 20250324.1, sha256 f686586b...; AC79 SDK
# V1.2.1 tool files; Python + pinned Pillow). Inside, build.py sees Linux x86-64 and calls the toolchain
# directly (JIELI_DOCKER=0), so no nested docker-exec calls and no host<->VM file staleness: every file is
# written and read by the same container. The project root is mounted at its own path, and the container
# runs as the host user so build outputs are owned by you, not root.
#
# Environment:
#   FM1_TOOLCHAIN_IMAGE  image to use (default: fm1-toolchain:20250324.1-raqm, see tools/docker/Dockerfile.raqm)
#   FM1_BASE             fm1-base checkout (default: ROOT/reference/fm1-base); must be inside ROOT
#   FM1_AC79_SDK         a full AC79 SDK checkout inside ROOT (only for ble/wifi builds; default: the image's
#                        /opt/ac79, which holds just the three files a --no-ble package needs)
#   FM1B_*, FM1_FONT_FULL, X0X_FONTSET, X0X_FONT_DIRS   passed through to build.py
#
# Requires: docker CLI talking to a VM that can run linux/amd64 (colima start --vm-type vz --vz-rosetta).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
IMAGE="${FM1_TOOLCHAIN_IMAGE:-fm1-toolchain:20250324.1-raqm}"
FM1_BASE="${FM1_BASE:-$ROOT/reference/fm1-base}"
FM1_BASE="$(cd "$FM1_BASE" && pwd -P)"

die() { echo "build_device: $*" >&2; exit 1; }

case "$FM1_BASE/" in "$ROOT"/*) ;; *) die "FM1_BASE ($FM1_BASE) must be inside $ROOT (only ROOT is mounted)";; esac
[ -f "$FM1_BASE/tools/build.py" ] || die "no tools/build.py in $FM1_BASE"
[ -f "$FM1_BASE/bsp/chip/crt0.S" ] || die "$FM1_BASE/bsp is empty: git -C \"$FM1_BASE\" submodule update --init"

command -v docker >/dev/null 2>&1 || die "docker CLI not found (brew install docker colima)"
docker info >/dev/null 2>&1 || die "no docker engine: start the VM with  colima start --vm-type vz --vz-rosetta"
docker image inspect "$IMAGE" >/dev/null 2>&1 || die "image $IMAGE not found: build it with
  docker build --platform linux/amd64 -t fm1-toolchain:20250324.1 \"$ROOT/reference/optimist/tools/docker\"
  docker build --platform linux/amd64 -t fm1-toolchain:20250324.1-raqm \"$ROOT/tools/docker\" -f \"$ROOT/tools/docker/Dockerfile.raqm\""

TTY=""
[ -t 0 ] && [ -t 1 ] && TTY="-it"

# environment for build.py inside the container
ENVARGS="-e HOME=/tmp -e JIELI_DOCKER=0 -e JIELI_TOOLCHAIN=/opt/jieli -e FM1B_JOBS=${FM1B_JOBS:-6} -e PYTHONDONTWRITEBYTECODE=1"
for v in $(env | sed -n 's/^\(FM1B_[A-Z0-9_]*\)=.*/\1/p; s/^\(FM1_FONT_FULL\)=.*/\1/p; s/^\(X0X_FONTSET\)=.*/\1/p; s/^\(X0X_FONT_DIRS\)=.*/\1/p'); do
    [ "$v" = FM1B_JOBS ] || ENVARGS="$ENVARGS -e $v"
done
if [ -n "${FM1_AC79_SDK:-}" ]; then
    SDK="$(cd "$FM1_AC79_SDK" && pwd -P)"
    case "$SDK/" in "$ROOT"/*) ;; *) die "FM1_AC79_SDK ($SDK) must be inside $ROOT";; esac
    ENVARGS="$ENVARGS -e AC79_SDK"
    AC79_SDK="$SDK"; export AC79_SDK
fi

run() {   # docker run with ROOT mounted at its own path (it has spaces: every path stays quoted)
    # shellcheck disable=SC2086
    docker run --rm $TTY --platform linux/amd64 --user "$(id -u):$(id -g)" $ENVARGS \
        -v "$ROOT:$ROOT" -w "$FM1_BASE" "$IMAGE" "$@"
}

if [ "${1:-}" = "--shell" ]; then
    [ -n "$TTY" ] || TTY="-i"     # (stdin attached when piped too: echo CMD | build_device.sh --shell)
    run bash
    exit $?
fi

[ $# -ge 1 ] || die "usage: tools/build_device.sh APP_DIR [--no-ble] [--release X.Y[.Z]] ...  |  --shell"
APP_IN="$1"; shift
[ -d "$APP_IN" ] || die "no such app directory: $APP_IN"
APP="$(cd "$APP_IN" && pwd -P)"
case "$APP/" in "$ROOT"/*) ;; *) die "APP_DIR ($APP) must be inside $ROOT (only ROOT is mounted)";; esac
[ -f "$APP/fm1.toml" ] || die "no fm1.toml in $APP"

STAMP="$APP/build/.build_device.stamp"
mkdir -p "$APP/build"
: > "$STAMP"

echo "build_device: image $IMAGE ($(docker image inspect -f '{{.Id}}' "$IMAGE" | cut -c1-19))"
echo "build_device: fm1-base $FM1_BASE"
echo "build_device: app      $APP"
run sh -c 'echo "build_device: toolchain $(/opt/jieli/pi32v2/bin/clang --version 2>&1 | head -1)"; exec python3 tools/build.py "$@"' \
    build.py "$APP" "$@"

# the packages this build wrote
find "$APP/build" -maxdepth 1 -name '*.fwsc' -newer "$STAMP" | while IFS= read -r f; do
    echo "build_device: $(shasum -a 256 "$f" | cut -d' ' -f1)  $(wc -c < "$f" | tr -d ' ') B  $f"
done
rm -f "$STAMP"
