# Building the Xenia core

This builds `xenia.chimeraCore` from source and runs its tests. It has been
done on Ubuntu 24.04. A full build from nothing compiles Xenia three times
and takes about three and a half hours of processor time: under half an hour
on a 20-thread machine.

## What you need

- A Chimera checkout, built (its README, "Building"), with miniBox's host
  library and C++ guest toolchain built too. `<chimera>` below is that
  checkout. Chimera must be recent enough to have the Vulkan bridge.
- GCC 14 first on `PATH` for everything, miniBox included. It is the only
  compiler this has been built with. The native reference build and the
  sandboxed core must be made by the same compiler: with a native build made
  by clang, a few bytes of the console's memory were different after fifteen
  seconds of the game (docs/PLAN.md, "M2: in the sandbox").
- GCC 13 as well, as `gcc-13`: the PowerPC assembler used by the instruction
  tests is from 2013 and newer compilers refuse to build it.
- A processor with AVX2 and FMA.

```sh
sudo apt-get install -y --no-install-recommends meson ninja-build \
  build-essential cmake pkg-config python3 curl flex bison \
  gcc-13 gcc-14 g++-14 mono-complete
```

Chimera's own build needs a few more (its README lists them):
`libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev`. This core
links none of them.

For the tests that draw, a Vulkan driver. Mesa's software one will do:
`sudo apt-get install -y mesa-vulkan-drivers libvulkan1`.

miniBox, in the Chimera checkout:

```sh
mb=<chimera>/extern/chimera-common-minibox
meson setup "$mb/build/meson-linux" "$mb" && meson compile -C "$mb/build/meson-linux"
meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true && meson compile -C "$mb/build/meson-cpp"
```

## The sources

```sh
git submodule update --init
git -C extern/xenia submodule update --init --depth 1
```

`extern/xenia` is Xenia Canary at a fixed commit. It is never edited by
hand: every change is a numbered patch in `patches/`, applied by
`waterbox/apply-patches.sh` (the build scripts call it). To change Xenia,
edit the tree under `extern/xenia`, then run `waterbox/make-patches.sh`,
which writes the patches again from the tree.

## The steps

```sh
waterbox/build-native.sh              # two native builds, and the shader tools
waterbox/build-ppc-tests.sh           # the assembler, and Xenia's instruction tests
waterbox/run-ppc-tests.sh             # runs them natively; the result is the reference
waterbox/build-package.sh -r <chimera>   # the sandboxed core, and the package
```

- `build-native.sh` makes `build/native` (Xenia as upstream runs it, used
  only for the instruction tests) and `build/vsched` (one thread at a time
  on the core's own clock: the reference the sandboxed core is compared
  with). `build/vsched/bin/Linux/run-frames` runs a game a frame at a time.
- `build-package.sh` builds Xenia for miniBox's sandbox (`build/guest`),
  links `build/wbx/core.wbx`, and installs
  `<chimera>/build/Cores/xenia.chimeraCore`. It also builds
  `build/wbx/run-wbx`, which runs the core in the sandbox the way Chimera
  does.

## The tests

```sh
CHIMERA_ROOT=<chimera> waterbox/run-gate.sh
```

Each line says what was compared. The script ends with a count of tests that
passed, failed and were skipped, and exits with an error if any failed. A
skipped test is not a pass.

Most tests need a disc image this repository cannot contain: Mirror's Edge
(NTSC), at `tests/roms/Mirrors Edge [NTSCU][NTSCJ].iso` or named by
`XENIA_TEST_GAME`. Without it three tests run: the instruction tests (4,460
of them, in the sandbox, compared with the native result), a check that a
write to a page the renderer watches is noticed whichever of the page's
three addresses it comes through (`memory:views`), and a check that a thread
which waits by reading the clock in a loop still gives way to the others
(`clock:turn`). With it:

- two native runs give the same result, and a run with another instruction
  cost does not;
- the sandboxed core gives the native run's result at every sixtieth frame
  (time, thread switches, sound, the console's memory, the picture);
- a button press changes the run, the same way in both;
- a savestate saved and loaded before every frame changes nothing, and so
  does one saved by one process and loaded by another;
- the same three with the Vulkan renderer, where every load is a new
  graphics device;
- a run that names no CPU speed is the run at the default, 500;
- a container made from the disc image's own program
  (`waterbox/tests/make-container.py`) is mounted and its program run, the
  same natively and in the sandbox, and Chimera's headless runner starts it;
- a setting of Xenia's that a project sets reaches Xenia: with Fill New
  Memory the console's memory is another (the runners take a project
  setting as `--set name=value`);
- with `CHIMERA_ROOT` set, the settings the package suggests: Mirror's
  Edge is found by its title id (45410850) both on the disc and in the
  container made from it; a value of the wrong kind for one of Xenia's
  settings is refused, and the scratch partition does not stop a run;
- with `CHIMERA_ROOT` set, the package in Chimera's headless runner
  (`chimera-run`): the picture, the picture with savestates, the refusal to
  start with no graphics card, the save data export, and that a movie has
  columns only for the controllers that are plugged in.

The tests that draw are skipped on a machine with no Vulkan driver.

Chimera's own checks of an installed package:

```sh
cd <chimera>
CHIMERA_CORES_DIR=$PWD/build/Cores dotnet test \
  source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj -c Release \
  --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests"
```

## Running a game by hand

```sh
build/vsched/bin/Linux/run-frames <game.iso> <a new folder> --frames 600 --every 60
build/wbx/run-wbx build/wbx/core.wbx <game.iso> --frames 600 --every 60 --renderer vulkan --ppm out.ppm
<chimera>/build/meson-linux/chimera-run <chimera>/build/Cores/xenia.chimeraCore <game.iso> <movie> --gpu
```

The options of the first two are listed at the top of
`waterbox/gate-harness.h`. `--pace` says, for each stretch, how long the
host took, how many guest instructions ran and which threads ran them.

## What CI does

`.github/workflows/chimera.yml` builds everything above from a fresh clone
on a public runner, runs the test script and Chimera's package checks, and
publishes the package. A runner has no game, so there the tests that need
one are skipped; they are run where the game is before anything is pushed.
The two tests that need no game must pass there, and the workflow asks for
them by name.

## When something goes wrong

- "no C++ guest toolchain": miniBox's `build/meson-cpp` is not built.
- "this miniBox has no Vulkan bridge": the Chimera checkout is too old.
- `apply-patches.sh` says the tree is "partly patched": a file under
  `extern/xenia` was edited without running `waterbox/make-patches.sh`.
- The sandboxed core stops with "asked for system call N": miniBox ends a
  guest that makes a system call it does not know. The note names the
  function; the answer usually belongs in `waterbox/guest/guest-libc.cc` or
  `waterbox/driver/memfs.cc`.
- A native run that never ends: `build/native` (host threads) has no clock
  of its own and a hung game hangs it. Run it under `timeout`.

## The per-game settings

`waterbox/driver/optimized_settings.inc` is made from a checkout of Xenia
Manager's list, and is committed:

```sh
git clone https://github.com/xenia-manager/optimized-settings <dir>
python3 waterbox/import-optimized-settings.py <dir> > waterbox/driver/optimized_settings.inc
```

The script names the list's commit at the top of the file. A setting of
Xenia's that the list uses and this core does not have is left out, and the
note the New Project window shows says why (`LEFT_OUT` in the script).
