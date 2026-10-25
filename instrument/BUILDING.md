# Building CYMA-1

Run every command from the project root: the folder that holds `instrument/` and `reference/fm1-base/`.
`DYLD_LIBRARY_PATH` lets Pillow find Homebrew's libraqm, so the fonts get tabular digits. Without it,
`tools/gen_fonts.py` prints a warning and builds anyway.

```sh
export DYLD_LIBRARY_PATH=/opt/homebrew/lib

# host simulator -> instrument/build/host/cyma-1_host
python3 reference/fm1-base/tools/build.py instrument --host
mkdir -p instrument/build/out
instrument/build/host/cyma-1_host instrument/tests/smoke.txt instrument/build/out   # exit 0; boot.png end.png smoke.wav

# browser emulator -> instrument/build/emu/ (app.wasm, index.html, worklet.js)
python3 reference/fm1-base/tools/build.py instrument --web

# device package, built but NEVER flashed -> instrument/build/cyma-1.fwsc (~5 s; pinned toolchain in docker/colima)
tools/build_device.sh instrument --no-ble
```

`python3` can be miniconda's or `.venv312/bin/python`. Both have Pillow.

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
python3 reference/fm1-base/tools/build.py "$SP/build-<module>/instrument" --host
mkdir -p "$SP/build-<module>/instrument/build/out"
"$SP/build-<module>/instrument/build/host/cyma-1_host" "$SP/build-<module>/instrument/tests/smoke.txt" "$SP/build-<module>/instrument/build/out"
python3 reference/fm1-base/tools/build.py "$SP/build-<module>/instrument" --web
```

- Edit only your own files, in `instrument/`, and then re-sync.
- `gen_fonts.py` finds fm1-base on its own. It checks `$FM1_BASE`, then an ancestor's `reference/fm1-base`, then
  the project path. So the copy under `/private/tmp` builds unchanged.
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
