# Building CYMA-1

This page goes from a fresh clone to the release's device package, byte for byte, then on to the host simulator, the
browser build and the tests. Run every command from the repository root.

Nothing on this page flashes, installs or sends MIDI. Installing is a separate step with its own guide:
[notes/INSTALL.md](notes/INSTALL.md).

## What you need

| For | What |
|---|---|
| The device package | `git`, and Docker with a VM that runs linux/amd64 containers. On a Mac: `brew install docker colima` |
| The host simulator, the browser build and the tests | A C compiler, Python 3 with Pillow (the tests also use numpy and scipy), Emscripten (`emcc`) for the browser build, ffmpeg for the demos and media, and node for the browser smoke tests |

On macOS, Pillow finds Homebrew's libraqm through `DYLD_LIBRARY_PATH=/opt/homebrew/lib`, so the fonts get tabular
digits. The device build doesn't need it: its fonts are made inside the container.

## 1. Clone

```sh
git clone https://github.com/TEG-Lizard-Man/cyma-1.git
cd cyma-1
git checkout v1.1.3        # the release's tag; leave this out to build the newest source
```

## 2. The toolchain image (once per machine)

The device build runs inside a pinned linux/amd64 image: JieLi's pi32v2 toolchain 20250324.1, three AC79 SDK tool
files and Python with Pillow 12.2.0. It is Optimist's Dockerfile, with a thin layer on top for libraqm.

```sh
colima start --vm-type vz --vz-rosetta     # Mac: the VM (skip it if `docker info` already answers)
docker image inspect fm1-toolchain:20250324.1-raqm >/dev/null 2>&1 && echo "the image is there: skip the rest of step 2"

git clone https://github.com/w0ts/optimist ../optimist
git -C ../optimist checkout 9ef7b789447ec51becc9f1344ad26783a7542340
docker build --platform linux/amd64 -t fm1-toolchain:20250324.1 ../optimist/tools/docker
docker build --platform linux/amd64 -t fm1-toolchain:20250324.1-raqm -f tools/docker/Dockerfile.raqm tools/docker
```

- The first build downloads the toolchain from JieLi's own server and checks its sha256 (`f686586b…`). It also
  checks the sha256 of each SDK file.
- The toolchain carries no licence to pass it on. Keep the image local and never push it to a registry.

## 3. The platform

CYMA-1 v1.1.1 and later are built on the **Team EG platform**: upstream fm1-base and felucca-bsp at pinned commits,
with the patches in [`platform/patches/`](platform/README.md) on top.

```sh
tools/get_platform.sh
```

It fetches the pinned upstream through the `reference/fm1-base` submodule (fm1-base `086cd0c`, felucca-bsp
`b413cd8`), applies the patches in a fresh copy, `platform/fm1-base-teg`, and checks that the files match the pinned
trees. **Expect** it to end with:

```
  fm1-base   086cd0c + 3 patches = a0f2af2 (branch teg/platform)
  felucca-bsp b413cd8 + 1 patch   = 7f4693a
  files match the pinned trees (d8330ad, bsp 68fa898)
```

## 4. The device package

```sh
tools/build_device.sh instrument --no-ble --release 1.1.3
shasum -a 256 instrument/build/cyma-1-1.1.3.fwsc
```

`tools/build_device.sh` uses `platform/fm1-base-teg` when it is there.

**Expect:**
- every build check prints `ok`;
- the log names the identity `FM-1_94010103`;
- the package is 611,496 bytes;
- the hash is the release's:

```
1c61abd4d1754a5c181cd60ed8895cf98b6d7970b1e42f3c84ddc480f0f413d3  instrument/build/cyma-1-1.1.3.fwsc
```

The build is reproducible: the same source, platform and image give the same bytes in any folder. A different hash
means one of the three differs.
- Built against upstream `reference/fm1-base` instead of the platform, the same source gives a different package.
  That package is not v1.1.3.
- `--release 1.1.3` sets the identity. Without it you get a dev package, `build/cyma-1.fwsc`, identity `FM-1_9400`.

## 5. Host simulator, browser build and tests

```sh
export DYLD_LIBRARY_PATH=/opt/homebrew/lib          # macOS
export FM1_BASE="$PWD/platform/fm1-base-teg"
python3 "$FM1_BASE/tools/build.py" instrument --host   # -> instrument/build/host/cyma-1_host
python3 "$FM1_BASE/tools/build.py" instrument --web    # -> instrument/build/emu/ (app.wasm, index.html, worklet.js)
instrument/tools/run_all.sh --device                   # every test and gate, the demos, the web package
```

- `run_all.sh --device` builds the device package too, as `instrument/build/cyma-1.fwsc`, and writes its hash to
  `instrument/build/SHA256SUMS`.
- `--perf` adds the package's run in the fm1-emu chip emulator, about 3 minutes. Two of its 21 gates are red: the
  panel keys' latency (see the README's Known limits).
- [`instrument/BUILDING.md`](instrument/BUILDING.md) and the instrument's README list every step and flag.

## 6. The browser build in `docs/`

`docs/` is the packaged page that `run_all.sh` writes to `instrument/dist/web`: the page with its WebAssembly, the
twelve demo films and `SPEC.json`, with no fixed share base, so its links point wherever it is served. After step 5:

```sh
rsync -a --delete --exclude .nojekyll instrument/dist/web/ docs/
```

Without the full run, `instrument/tools/render_demos.sh` then `python3 instrument/tools/package_web.py` make the same
folder from the web build.

## 7. The boot media

`media/boot-intro.gif` and `media/boot-intro.mp4` are the boot from power-on, rendered offline by the host simulator.
After the host build in step 5:

```sh
tools/make_boot_media.sh
```
