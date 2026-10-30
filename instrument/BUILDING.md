# Building CYMA-1

Run every command from the repository root: the folder that holds `instrument/`, `platform/` and `reference/`. The
step-by-step guide, from a fresh clone to a package whose sha256 matches the release, is [`../BUILDING.md`](../BUILDING.md).
This page is the short form.

`DYLD_LIBRARY_PATH` lets Pillow find Homebrew's libraqm, so the fonts get tabular digits. Without it,
`tools/gen_fonts.py` prints a warning and builds anyway.

```sh
tools/get_platform.sh                              # the Team EG platform -> platform/fm1-base-teg (once)
export DYLD_LIBRARY_PATH=/opt/homebrew/lib
export FM1_BASE="$PWD/platform/fm1-base-teg"

# host simulator -> instrument/build/host/cyma-1_host
python3 "$FM1_BASE/tools/build.py" instrument --host
mkdir -p instrument/build/out
instrument/build/host/cyma-1_host instrument/tests/smoke.txt instrument/build/out   # exit 0; boot.png end.png smoke.wav

# browser emulator -> instrument/build/emu/ (app.wasm, index.html, worklet.js)
python3 "$FM1_BASE/tools/build.py" instrument --web

# device package, built but NEVER flashed -> instrument/build/cyma-1-1.1.3.fwsc (~5 s; pinned toolchain in docker/colima)
tools/build_device.sh instrument --no-ble --release 1.1.3
```

The release (v1.1.3) comes from `fm1.toml`'s `release = "1.1.3"`, a CYMA-1 key that fm1-base ignores. fm1-base's
`--release X.Y[.Z]` makes the package identity `id_prefix` + `XXYYZZ` (`FM-1_94010103`; v1.1.2 was `FM-1_94010102`,
v1.1.1 `FM-1_94010101`, v1.1 `FM-1_94010100`, the dev build and v1.0 are `FM-1_9400`), names the screen reader's
firmware "CYMA-1 1.1.3" and writes `cyma-1-X.Y.Z.fwsc`. `tools/run_all.sh --device` passes it and renames the package
`build/cyma-1.fwsc`, the name `build/SHA256SUMS`, the README and the emulator tools use. Without `--release` the same
command builds a dev package, `build/cyma-1.fwsc`, identity `FM-1_9400`.

**fm1-base for v1.1.1 and later is the Team EG platform, `platform/fm1-base-teg`**: fm1-base `086cd0c` (v1.1's) and
felucca-bsp `b413cd8` with the patches in `platform/patches/`:
- felucca-bsp's `chip/usb.c`: the main loop no longer stalls once a second while no USB host is attached;
- fm1-base's `device/main_fm1.c` (v1.1.3): MASTER is read before the audio starts, so the boot sound plays at the
  knob's level from its first block.

`tools/get_platform.sh` makes it from the pinned upstream commits (`reference/fm1-base`, the git submodule) and checks
the result against pinned trees. Every tool takes the tree from `FM1_BASE`. Unset, `tools/run_all.sh` and
`tools/build_device.sh` use `platform/fm1-base-teg` when it is there (else `reference/fm1-base`), and `run_all.sh`
exports it, so `tools/build_device.sh` builds the same tree. Built against `reference/fm1-base` instead, the package
is not the release's (its hash differs).

`python3` is any recent Python 3 with Pillow (the tests also use numpy and scipy).

The device build must print `ok` for every check:
- `.ram_text` has no calls;
- no 64-bit multiply beside a load;
- no soft-double;
- the pool and RAM fit;
- register access is clean.

Never run `fm1_install.py`, `fm1_remote.py`, `fm1_mirror.py` or any flash, OTA or MIDI tool against the hardware.

## Module agents: build a private copy

Builds write into `APP_DIR/build/`. If two agents build `instrument/` at the same time, they overwrite each other.
So each module agent builds its own copy (`<module>` = m1, m2, ...):

```sh
export DYLD_LIBRARY_PATH=/opt/homebrew/lib
SP="${TMPDIR:-/tmp}/cyma-build"
rsync -a --delete --exclude build "instrument/" "$SP/build-<module>/instrument/"
python3 "$FM1_BASE/tools/build.py" "$SP/build-<module>/instrument" --host
mkdir -p "$SP/build-<module>/instrument/build/out"
"$SP/build-<module>/instrument/build/host/cyma-1_host" "$SP/build-<module>/instrument/tests/smoke.txt" "$SP/build-<module>/instrument/build/out"
python3 "$FM1_BASE/tools/build.py" "$SP/build-<module>/instrument" --web
```

- Edit only your own files, in `instrument/`, and then re-sync.
- `gen_fonts.py` finds fm1-base on its own. It checks `$FM1_BASE`, then an ancestor's `reference/fm1-base`, then
  the project path. So a copy in the system's temp folder builds unchanged.
- `tools/build_device.sh` mounts only the project root. A device build of a private copy must therefore sit inside
  the project, e.g. `build-test/build-<module>/instrument/` (same rsync), then
  `tools/build_device.sh build-test/build-<module>/instrument --no-ble`.

## Unit tests (no fm1-base build needed)

```sh
cc -O2 -std=c99 -ffp-contract=off -DPLAT_HOST -Iinstrument/src -Ireference/fm1-base/include \
   instrument/tests/unit/test_X.c instrument/src/....c -lm
```

## Layout notes

- `src/*.c` are separate -O2 units (fm1.toml `o2`). They include their headers by bare name (`"cy_params.h"`) and
  `"fastmath.h"`.
- `src/` has no libc on the device apart from memset and memcpy. `printf` is allowed only under `#ifdef PLAT_HOST`.
- `app.c` is the unity part. It includes `ui/ui.c`, and `ui/ui.c` includes the other `ui/*.c` files.
- On the host, `plat_host.c` defines `__attribute__(x)` away for the unity part. It does not do that for the
  `src/` units.
- The frozen headers (`src/cy_*.h`, `ui/ui.h`, `app.h`) change only with the integrator's (M7's) approval.
- PHASE 0 stubs say `STUB` in their header comment. Each module replaces only its own files.
