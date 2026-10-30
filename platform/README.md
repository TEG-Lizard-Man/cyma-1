# The Team EG platform

CYMA-1 runs on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal, which builds on Leo
Kuroshita's [felucca-bsp](https://github.com/hugelton/felucca-bsp). Both are MPL-2.0. From v1.1.1 on, CYMA-1 is built
on a pinned fork of the two with two small fixes. This folder holds that fork as patches, so no extra repository is
needed:

- **Upstream stays upstream.** `reference/fm1-base` is a git submodule pinned at fm1-base `086cd0c`, and fm1-base's own
  `bsp` submodule pins felucca-bsp `b413cd8`.
- **The fixes are `git format-patch` files.** They are in `patches/`, one folder per repository.
- **`tools/get_platform.sh` puts them together.** It copies both pinned repositories to `platform/fm1-base-teg`
  (git-ignored) and applies the patches with `git am`. Then it checks the files against pinned tree hashes.
  `tools/build_device.sh` and `instrument/tools/run_all.sh` use that tree by default.

## The patches

| Patch | Applies to | What it changes |
|---|---|---|
| [`felucca-bsp/0001`](patches/felucca-bsp/0001-usb.c-usb_retry-in-two-steps-so-a-main-loop-with-no-.patch) | felucca-bsp `b413cd8`, `chip/usb.c` | **No main-loop stall without a USB host.** With no host attached (on battery, or on a charger without data lines), the main loop retried USB once a second: a 25 ms wait and six SIE timeouts, 29–41 ms in which keys, TRS MIDI and the screen waited. The retry is now two steps on separate passes, timed instead of waited, and costs 0.5–1.8 ms. In fm1-emu, CYMA-1's MIDI-to-sound p90 went from 39.9 to 7.4 ms |
| [`fm1-base/0001`](patches/fm1-base/0001-bsp-7f4693a-usb_retry-in-two-steps-no-main-loop-stal.patch) | fm1-base `086cd0c`, `bsp` | The bump of `bsp` to the patched commit (`7f4693a`) |
| [`fm1-base/0002`](patches/fm1-base/0002-MASTER-read-before-the-audio-starts-no-boot-sound-at.patch) | `device/main_fm1.c` | **The boot sound at the knob's level.** MASTER is read before the audio starts. Before, the volume started at half and took about 0.25 s to reach the pot, so with MASTER at zero a boot sound played a short burst (v1.1.3) |
| [`fm1-base/0003`](patches/fm1-base/0003-TEG_FORK.md-this-fork-s-base-its-commits-and-where-i.patch) | `TEG_FORK.md` | The fork's own note: its base, its commits, where it comes from |

Unchanged:
- the boot's USB start;
- safe mode;
- everything while USB is up;
- the update loader (its `ota.bin` builds byte for byte the same);
- the host and browser builds, which compile neither change, so their sound is bit for bit upstream's.

## The pins

| | Upstream | After the patches (commit) | After the patches (files: tree) |
|---|---|---|---|
| fm1-base | `086cd0c6fcc1479f81c54b3929095ed81f87c6a7` | `a0f2af24e62fc501aa1b57b7962c74db04f2563c` | `d8330ad7e03e5b8e2a8379b83abebc8643aa47d8` |
| felucca-bsp | `b413cd888424249d417d5549b594e8492d399d3e` | `7f4693af3a469620fbc219c0f40cb74eb4134620` | `68fa89838b6dc1f43e634887a7602bf465a2ca20` |

`git am` writes the commits with the patches' own author, dates and messages, so the commit ids come out the same on
any machine. The script requires the trees to match; it only warns about the commit ids, since the build reads files.

## By hand

The same as `tools/get_platform.sh`, without the checks:

```sh
git submodule update --init --recursive reference/fm1-base
git clone reference/fm1-base platform/fm1-base-teg
git -C platform/fm1-base-teg checkout -b teg/platform 086cd0c6fcc1479f81c54b3929095ed81f87c6a7
git -C platform/fm1-base-teg am --committer-date-is-author-date "$PWD"/platform/patches/fm1-base/*.patch
rmdir platform/fm1-base-teg/bsp
git clone reference/fm1-base/bsp platform/fm1-base-teg/bsp
git -C platform/fm1-base-teg/bsp checkout -b teg/platform b413cd888424249d417d5549b594e8492d399d3e
git -C platform/fm1-base-teg/bsp am --committer-date-is-author-date "$PWD"/platform/patches/felucca-bsp/*.patch
```

fm1-base's patches go in first, while `bsp` is still empty, because git warns when a patch moves the gitlink of a
populated submodule.

## Upstream

Both fixes are written so they can go upstream: the USB retry to felucca-bsp (with the bump to fm1-base), and the
MASTER seed to fm1-base. Neither is in the upstream commits pinned here. Once upstream has them, CYMA-1 goes back to
plain `reference/fm1-base` and this folder is retired.

The patches are MPL-2.0, like the files they change.
