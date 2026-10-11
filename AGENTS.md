# AGENTS.md - Xenia core for Chimera

This repository builds Xenia Canary
(https://github.com/xenia-canary/xenia-canary), an Xbox 360 emulator, as a
core for Chimera (https://github.com/ToolAssisted-run/chimera), a frontend
for tool-assisted speedruns. It produces one file, `xenia.chimeraCore`:
Xenia compiled to run inside Chimera's sandbox (miniBox), a driver that runs
the console one frame at a time on its own clock, and the declarations
Chimera reads (controllers, settings, file types). Xenia is a git submodule
at a fixed commit; every change to it is a numbered patch. The picture is
drawn by Xenia's Vulkan renderer on the host's graphics card, through
Chimera's Vulkan bridge.

Read [docs/BUILDING.md](docs/BUILDING.md) before building and
[docs/PLAN.md](docs/PLAN.md) before changing how the core works: it is the
design log, with what was decided, why, and what was measured.

## Layout

- `extern/xenia` - Xenia Canary, a git submodule at a fixed commit.
  `extern/spirv-headers` - headers the shader tools need.
- `patches/` - the numbered patches applied to `extern/xenia`.
- `waterbox/driver/` - the core itself, the same code natively and in the
  sandbox: starting the console and running a frame (`xenia_driver.cc`), the
  functions Chimera calls (`wbx-entry.cc`), the console's storage kept in
  memory (`memfs.cc`), Xenia's settings for this core (`machine_settings.h`).
- `waterbox/guest/`, `waterbox/native/` - what each build adds: the sandbox's
  missing C library pieces, and `native/run-frames.cc`, the native runner.
- `waterbox/run-wbx.c`, `gate-harness.h` - the sandbox runner, and the
  options both runners share. `run-ppc-wbx.c` runs Xenia's instruction tests
  in the sandbox.
- `waterbox/*.sh` - the builds and the test script (`run-gate.sh`).
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`,
  `package-licenses.json` - what the package declares. Written by hand.
- `.github/workflows/chimera.yml` - CI: build, test, publish.
- `build/` - every build output. `tests/roms/` - test games. Both ignored by
  git.

## Build and test

```sh
git submodule update --init
git -C extern/xenia submodule update --init --depth 1
waterbox/build-native.sh
waterbox/build-ppc-tests.sh && waterbox/run-ppc-tests.sh
waterbox/build-package.sh -r <chimera>
CHIMERA_ROOT=<chimera> waterbox/run-gate.sh
```

GCC 14 must be first on `PATH` for all of it, miniBox included
(docs/BUILDING.md says why and how). The test script must end with
`0 failed`. Run it before every commit that changes the core, and say in the
commit what it printed.

Most of its tests need a disc image of Mirror's Edge, which this repository
cannot contain. Without it they are skipped, and a skipped test proves
nothing: a change to how the console runs must be tested where the game is.
If you do not have it, say so in your report.

## Rules

- **Never edit `patches/` by hand.** Edit the tree under `extern/xenia`,
  then run `waterbox/make-patches.sh`: it writes every patch again from the
  tree. A new file belongs to a patch through the lists in that script.
  `waterbox/apply-patches.sh` must then say "already applied".
- **The native run is the reference.** `build/vsched` and the sandboxed core
  must give the same result for the same input: the same time, sound,
  memory and picture at every checked frame. A change that makes them
  differ is wrong even when both look right.
- **The same input gives the same run, every time.** Nothing may read the
  host's clock, the host's threads, an address the host chose, or a file
  outside the package. One thread of the console runs at a time, and time
  moves only by counting instructions (`threading_chimera.cc`).
- **A savestate is taken only between frames**, and after a load the
  renderer is built again on a new graphics device
  (`ChimeraRemakeContext`). Anything new the renderer keeps on the card must
  survive that: the tests `picture:remade` and `picture:every-frame` check it.
- **A new test is seen to fail once.** Break the thing it checks, on
  purpose, and see the test say so before trusting it.
- **No game, system software or key is committed** or uploaded to CI.
- **Changing what a setting means, or the CPU Speed default, changes every
  movie made with it.** That is the owner's decision.
- **Write in plain language.** Commits, documents and comments are read by
  people who have never seen this repository. `.md` files are ASCII.
- **Commits**: `type(scope): what changed`, a short body saying what
  changed, why, and how it was tested with the numbers. Stage paths by name.
  No amending, no force push.

## Publishing

CI publishes the package: a `dev` release on every push to `main` whose
tests pass, and a dated `nightly` when `main` moved. A package built by hand
is stamped `<commit>+local` and is for testing only.

## What is not done

docs/PLAN.md lists it. The main points: only one game has been tried; the
game runs at a third to a half of real time; a save cannot be put in before
the game starts; Chimera's window (as opposed to its headless runner) has
not been run with this core.
