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

- **M0 - a native headless reference.** In progress. The build: DONE
  2026-10-10 (below). A driver that boots a program with the null renderer
  and no window: not started.
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

## What is the owner's

- **Content.** There is no Xbox 360 game on the development machine. Nothing
  past M0's build can be proved without one.
- **The repository.** Local only. A public repository is the owner's to ask
  for.
