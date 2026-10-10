# Xenia core - plan and design log

## What this is

A Chimera core for the Xbox 360, built on Xenia Canary
(xenia-canary/xenia-canary, pinned at `extern/xenia`). Requested in chimera
issue 171. The owner, 2026-10-10: "let's give it a fair try. Developing a
Vulkan bridge for Chimera is something I wanted us to do anyway."

"A fair try" is taken to mean what it says: this may not work, and where it
does not this file will say so.

Nothing here runs a game yet. This file is the survey, the route, and what
has been built so far.

## The pin

Canary, not upstream. xenia-project/xenia's `master` was last pushed
2026-02-18; Canary (`canary_experimental`) is pushed daily, builds with CMake,
and carries the kernel and compatibility work. Pinned at `6e2caa5`
(2026-10-09). Both are BSD-3-Clause (`extern/xenia/LICENSE`).

## The eight questions (chimera docs/new-core.md), answered from the source

- **A new core?** Yes. No core here covers the Xbox 360.
- **Licence.** BSD-3-Clause. Modified and binary redistribution are allowed.
  The bundled third parties (FFmpeg, zstd, fmt and the rest) each have their
  own; `package-licenses.json` will list them when there is a package.
- **One thread on one clock?** Not as it stands. Every guest thread is a host
  thread (`kernel/xthread.cc`), and so are the GPU command processor, the
  vsync, the XMA decoder, the audio worker, the kernel dispatch thread and
  the timer queue. Guest time is the host clock times a scalar
  (`base/clock.cc`); `mftb` reads it. Nothing upstream records input or plays
  it back. The seam is narrow: `base/threading.h` (503 lines) and its POSIX
  backend (1521) are what everything waits and sleeps through.
- **The picture.** `gpu/d3d12`, `gpu/vulkan` and `gpu/null`. No OpenGL and no
  software rasteriser. So the picture comes through a Vulkan bridge, which
  Chimera did not have and is being built for this (miniBox `source/vk`).
  By default nothing the GPU draws is read back into the console's memory
  (`readback_resolve` is "none", `readback_memexport` false, occlusion
  queries are answered with a made-up count), so the machine can be compared
  with the null renderer as its reference.
- **A recompiler?** Only one: x64 through xbyak (and arm64). There is no
  interpreter. It counts no instructions, so virtual time needs a charge
  added to what it emits. It refuses to start without AVX; what else it uses
  of the host CPU is behind one mask (`x64_extension_mask`), which can be
  pinned. Its function table is at host address 0x80000000 and its code at
  0xA0000000, with 32-bit entries - addresses below 4 GiB, which a guest does
  not have; both must move (a slow path without the table exists).
- **Memory.** One mapping viewed at guest 0xA0000000, 0xC0000000 and
  0xE0000000 and again above 4 GiB. A guest has anonymous mappings only, so
  the views must be folded into one in the recompiler's address computation
  (which already compares against 0xE0000000) and in
  `Memory::TranslateVirtual`. MMIO is by page fault (the GPU's registers, the
  XMA decoder's) and so are the GPU's write watches; miniBox forwards a
  guest's faults to it, as the RPCS3 core relies on.
- **What the user brings.** Games. The kernel is emulated: no BIOS, no
  firmware, no keys.
- **A gate on a public runner.** Upstream has 576 PowerPC instruction tests
  (they need a PowerPC assembler); there is no Xbox 360 software that may be
  redistributed. CI can prove the build and the declarations.
- **Size.** 345,000 lines (GPU 122,000, CPU 79,000, kernel 56,000). The
  console has 512 MB of RAM in a 4 GiB map, and the recompiler reserves 768
  MB more.

## The route

The RPCS3 core is the nearest thing (its `docs/PLAN.md`): host threads on a
wall clock made into one thread on a virtual one. Beyond what that needed:
instruction counting in a JIT, the JIT's tables moved, the aliased memory
folded, and a renderer that reaches a real card through a bridge that did not
exist.

- **M0 - a native headless reference.** DONE 2026-10-10: the build, upstream's
  instruction tests on it, and a driver that boots a game with no window
  (all below). It is a reference that runs, not yet one that repeats: the
  clock and the threads are still the host's.
- **M1 - virtual time.** A scheduler behind `xe::threading`, a charge per
  block in the x64 emitter, the clock from it.
- **M2 - the guest build.** The JIT's tables and the memory views.
- **M3 - savestates, input, lag.**
- **M4 - the picture**, through the Vulkan bridge.
- **M5 - the package and the frontend.**

## M0: the build (2026-10-10)

Canary's own documentation calls its Linux support "extremely experimental
and presently incomplete" and names clang 19. Measured here, with clang 20.1.2
on Ubuntu 24.04:

- Its configure stops at once on GTK, which it wants for its window. Patch
  0001 adds `CHIMERA_HEADLESS`: no window system, no toolkit, no application,
  no host audio or pad driver. With it the configure succeeds.
- The first compile of all 645 objects took one minute on 20 threads and
  failed in four places, none of them the emulator: the ImGui drawer asking
  fontconfig for a CJK font and GTK for a clipboard (guarded by the same
  option); a `version.h` that upstream's build script writes
  (`build-native.sh` writes it, with a word where the date was, so that two
  builds of one commit are one binary); and the two shader steps, which want
  `glslangValidator` and `spirv-opt` from a Vulkan SDK.
- The shader tools are built by `build-native.sh` from what Canary already
  pins (`third_party/glslang`, `third_party/SPIRV-Tools`) and the
  SPIRV-Headers revision its SPIRV-Tools names (`extern/spirv-headers`,
  2a611a9), in under a minute. No SDK is installed.
- Result: 34 static libraries, no failure, the Vulkan renderer among them.

Not established by this: that anything runs. The emulator's own setup
(`emulator.cc`) uses its window and its ImGui drawer in several places
without asking whether there is one, which the driver will have to deal with.

## M0: a game, with no window (2026-10-10)

`waterbox/native/run-native` is the emulator set up with no window, no ImGui
drawer, the no-op audio and pad drivers and either renderer, run on a program
for so many seconds of the host's clock. It is built inside Xenia's build
(patch 0001 adds a directory named by `CHIMERA_DRIVER_DIR`).

What it took:

- **Patch 0002: an emulator with no window.** `emulator.cc` already takes a
  null window in some places and not in others; six uses of the window or the
  drawer are guarded.
- **Logging has to be started by hand.** Upstream's console entry point on
  Linux has the call commented out, and a logger that was never started does
  not drop a line - it waits for room that never comes. The first thing to
  log is the Vulkan loader, so the first run sat in `Emulator::Setup` for
  ever, printing nothing. Read off a debugger.
- **The kernel has its own switch for nobody being there** (`headless`),
  which answers its dialogs; the driver sets it.

Measured with the owner's disc image of Mirror's Edge (title 45410850,
`tests/roms`, never committed), twenty seconds each:

| renderer | frames presented | what the log says |
| --- | --- | --- |
| null | 1700 | every draw "Failed in backend", as a renderer that draws nothing would |
| Vulkan, on llvmpipe (Mesa 25.2.8) | 1697 | no draw fails; one kernel call the game makes is not implemented (`IoDismountVolumeByFileHandle`, twice) |

So the game's code runs on the recompiler on Linux with clang 20, and Xenia's
Vulkan renderer runs with no window. About 86 frames a second are presented
in both, which is the game's intro pacing itself on the host's clock and says
nothing about speed.

Not established: that any of it is right. No frame has been looked at - with
no window there is no presenter, and nothing copies a picture out yet. The
"null" renderer is not null either: it makes a Vulkan device of its own
(`NullGraphicsSystem::Setup` creates a provider), which the sandbox's
reference renderer may not. And the device extensions the Vulkan renderer
turned on here - conditional rendering, custom border colour, fragment shader
interlock, memory budget, non-seamless cube map, shader stencil export - are
all ones the bridge offers.

## The recompiler's own tests (2026-10-10)

Upstream has 570 files of PowerPC instruction tests
(`src/xenia/cpu/ppc/testing`), most of them generated on a real console. They
put a few instructions at 0x82010000, run them on the recompiler with the
emulated memory and nothing else - no kernel, no program - and compare the
registers. That makes them the one part of this core that can be proved with
no game, and so the first thing the sandboxed build will be held to.

- `waterbox/build-ppc-tests.sh` builds the assembler upstream uses, binutils
  2.24 with its VMX128 patch, out of the tree (the tarball's SHA-256 is
  pinned and was the same from two mirrors), and assembles the tests the way
  `xb gentests` does: twenty seconds in all.
- The runner (`xenia-cpu-ppc-tests`) needed one more thing of the headless
  build: a file picker to link against, which patch 0001 now gives it - one
  that answers as a user who declines.
- `waterbox/run-ppc-tests.sh` runs them and keeps the names of the ones that
  fail.

Measured, natively, twice with the same answer: **170,417 tests, 142,097
pass, 28,320 fail**, in four minutes. The failures are upstream's and are
almost all floating point: the fused multiply-add family (`fmadd`, `fmsub`,
`fnmadd`, `fnmsub`, single and double, and `vmaddfp` and `vnmsubfp`) is some
25,000 of them, the rest are spread over other float operations and two
vector shifts. So the reference is not "they pass" but "these pass and
these fail": the sandboxed build must fail the same 28,320.

**What the recompiler emits depends on the host's CPU, and the results
with it.** With `--x64_extension_mask=0` (AVX and nothing above it; this
machine has AVX2, FMA, BMI2, F16C and MOVBE) it is 142,434 passing and
27,983 failing - and not a subset: 1,142 tests fail only with the host's
features and 805 only without, nearly all of them `vmaddfp` and `vnmsubfp`,
with a few `fmadd` and `fmsub`. A multiply-add computed by the host's fused
instruction and one computed in two steps round differently. A movie made on
one machine would not replay on another unless the mask is one value
everywhere, so it will be a constant of the core and not a setting. Which
value - nothing above AVX, which every machine that can run this has, or
with FMA, which is nearer the console and faster - is not decided.

## What is the owner's

- **Content.** There is no Xbox 360 game on the development machine. Nothing
  past M0's build can be proved without one.
- **The repository.** Local only. A public repository is the owner's to ask
  for.
