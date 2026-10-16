# Xenia core - plan and design log

## What this is

A Chimera core for the Xbox 360, built on Xenia Canary
(xenia-canary/xenia-canary, pinned at `extern/xenia`). Requested in chimera
issue 171. The owner, 2026-10-10: "let's give it a fair try. Developing a
Vulkan bridge for Chimera is something I wanted us to do anyway."

"A fair try" is taken to mean what it says: this may not work, and where it
does not this file will say so.

Where it stands (2026-10-11). One game has been tried, Mirror's Edge.

- It runs in Chimera's sandbox one frame at a time, and gives the same
  memory, sound and timing as the native build at every checked frame.
- The picture is drawn through miniBox's Vulkan bridge and is the picture
  the native build draws.
- A savestate can be loaded with that renderer, in the same process or in
  another one, and the picture afterwards is the one the run would have had.
- As a package it runs in Chimera's headless runner (`chimera-run`): on
  Linux on a software Vulkan driver, and on Windows on a GeForce GTX 1060.
  There a movie takes the game from its title screen through the menus,
  "New Game" and the opening into the first level.
- The console has a profile that is always signed in, and its storage is
  kept in memory. Chimera can export what the game saved; it cannot yet put
  a save in.
- It is slow: at the default CPU Speed the game runs at about a third to a
  half of real time. "Speed" below says why.
- Chimera's window has run it only when driven by a script, with nobody at
  it ("Published" below). No second game has been tried.

The Vulkan bridge is in miniBox's `main` (33acd30) and Chimera's engine uses
it from `main` (8fd6114). This repository is public and CI publishes the
package ("Published" below). The rest of this file is the survey, the route,
and what each step measured.

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
- **M1 - virtual time.** DONE 2026-10-10 on the native build, for the one
  game there is and thirty seconds of it (below): a scheduler behind
  `xe::threading`, a charge per block in the x64 emitter, the clock from it.
- **M2 - the guest build.** DONE 2026-10-10 (below): the memory in one
  block, the recompiler from the memory base, and the game and upstream's
  instruction tests in the sandbox with the native build's results.
- **M3 - savestates, input, lag.** DONE 2026-10-10 (below), with no
  picture: the core's exports, a frame, pads, sound, and states that are the
  whole machine.
- **M4 - the picture**, through the Vulkan bridge. DONE 2026-10-10 on
  llvmpipe (below, two sections): it draws in the sandbox what it draws
  natively, and a load puts the renderer back on a new device.
- **M5 - the package and the frontend.** DONE 2026-10-10 as far as the
  frontend's headless runner on Linux goes (below); nothing is published,
  and the engine's half is on a branch that is the owner's to merge.

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

## M1: one thread at a time on one clock (2026-10-10)

Patch 0003. `build/vsched` is the same sources with `CHIMERA_VSCHED`; the
host-thread tree (`build/native`) still builds and is what upstream's
instruction tests run on.

**The scheduler** (`base/threading_chimera.cc`) replaces the POSIX threading
backend and the timer queue. Every thread is still a host thread with its own
stack, but exactly one runs: it holds a baton and gives it up only by asking
the scheduler for something - a wait, a sleep, a yield, its own end. Who gets
it next is the first thread after it, in order of creation, that can run.
Time is a number only the scheduler moves: a fixed cost for every wait or
yield (a microsecond, so that a loop that only polls still makes time pass),
the instructions guest code ran, and a jump to the earliest deadline when
nobody can run. A waiting thread remembers what it waits for and is asked
when the baton is passed, so there are no wake-up lists to get wrong; that
is a scan of every thread at every switch and will want replacing when it
shows up in a profile.

**What else waited on the host, each found as a hang and read off a
debugger** (the run stops with every thread parked, or with one thread that
never lets go):

- The logger. Its writer thread waits for lines by spinning on the host.
  With the scheduler a line is written by whoever logs it; a log must never
  be what the machine waits for.
- `std::mutex`, `std::recursive_mutex` and the two condition variables, 140
  uses in 32 files, and Xenia's own lock types on top of them. A thread that
  finds a lock taken, or waits on a condition, slept on the host holding the
  baton, and the thread that would have woken it never ran. They are now
  `xe::sync::` types (`base/chimera_sync.h`): the standard ones without the
  scheduler, and with it a flag and a wait in the scheduler. The first one
  met was the kernel's dispatch thread.
- The steady clock. Deadlines were made by asking the host what time it
  was; `XE_STEADY_NOW()` is the machine's time on the same scale.
- The system clock: the machine is switched on at 2010-01-01 00:00:00 UTC.
- `logical_processor_count()`: six, whatever the host has.

**The recompiler pays for what it runs.** With one thread running, a guest
thread that never asks the scheduler for anything keeps the baton for ever -
the game's main thread did exactly that, in recompiled code. Each block now
takes its guest instruction count from the running thread's slice (one
global: only one thread runs) and calls the scheduler when the slice is
spent: `mov rax, &budget; sub [rax], n; jns` at the top of every block. A
slice is 100,000 instructions, and an instruction is 250 picoseconds of the
machine's one clock (`CHIMERA_INSTRUCTION_PS`) - both first guesses, neither
tuned. All six hardware threads' instructions move the same clock, so that
figure stands for everything the console does in a second, not for one of
its cores.

**Measured** (Mirror's Edge, null renderer, each run a new process, address
space layout randomisation left on):

| the machine's time | runs | baton handed over | frames | 512 MiB of RAM |
| --- | --- | --- | --- | --- |
| 5 s | 3, and 2 more from a tree rebuilt from the patches | 257,171 | 437 | ed611adef65dbf20, every time |
| 30 s | 2 | 1,778,448 | 2680 | 489c3a26d743dc0b, both |
| 5 s, an instruction costing 260 ps | 1 | 254,075 | 436 | 50af94a76d29de60 |

`waterbox/run-gate.sh` holds the first row and the last (`game:repeats`):
two runs are one run, and a run with another instruction cost is another.

Thirty seconds of the machine's time took 176 on the host, at the lowest
priority beside other work: about a sixth of real time. Nothing has been
done about speed; the instruction cost alone decides how much the host must
execute per second of the machine's time.

**Not established.** That it holds past thirty seconds, in a menu, in play,
or with input - there is no input yet. That it holds with the Vulkan
renderer, whose host threads are the driver's and are not in the schedule.
That the picture is right: none has been looked at. And three kinds of host
thread are still outside the schedule and have not been met yet because the
null renderer and this stretch of the game do not use them: the Vulkan
pipeline compilers, the audio media player's worker, and FFmpeg's own.
Priorities and affinities are recorded and ignored.

## GCC (2026-10-10)

A guest is built with miniBox's toolchain, which is GCC 13 over musl; Canary
names clang, and says GCC "should work in theory". Measured, natively, before
any of the sandbox is involved: three things stop it, and patch 0004 and a
line of 0001 deal with them.

- Thin LTO and lld are asked for by name whatever the compiler is (now only
  for clang).
- Two constructors declared with their template arguments
  (`ConfigVar<T>(...)`), which C++20 does not allow and GCC refuses - in
  `cvar.h`, so in 223 files. And a struct with constructors in an anonymous
  struct in a union (`KernelVersion`): the union overlaid four fields with
  one value nobody read, and is gone.
- Two forced inlines GCC cannot do: an intrinsic chosen at run time, and a
  virtual function whose body is elsewhere.

With those it builds, and the emulator GCC built runs the game to the same
digest as the one clang built - 257,171, 437, ed611adef65dbf20. That was not
a given: everything outside the recompiler is the compiler's arithmetic.

`waterbox/make-patches.sh` writes `patches/` from the tree, by path, so that
the series is always what the tree is.

## M2, first steps: it compiles for the sandbox (2026-10-10)

`waterbox/guest-toolchain.cmake` is miniBox's guest as a CMake toolchain
(musl, a static libstdc++, the large code model, nothing of the host's;
`MINIBOX_DIR` names the checkout). The guest compiler on this machine is GCC
14.2, a version stricter than the 13 the native probe used. With it the whole
of Xenia - 34 libraries, the Vulkan renderer and FFmpeg among them -
compiles for the guest once three things of the host's are kept out:

- SDL, which the third-party build looks up with the host's `pkg-config`
  and nothing of a headless build uses;
- the X window system: the Vulkan headers were told to bring XCB in, the
  presenter knew how to make an X surface, and the UI library had one;
- `linux/futex.h`, for locks the scheduler build does not have.

Nothing of it has RUN in the sandbox yet. What is known to stand in the way,
from the source: the emulated memory is views of one shared mapping, the
recompiler's tables sit at fixed addresses below 4 GiB, and its faults are
caught by a signal handler that reads the machine context.

**The instruction tests are the proof to hold it to, and they had to run in
one process first.** Upstream forks a child per test on POSIX, which a guest
cannot. Patch 0005 gives the runner the in-process path it has on Windows
(and, for a guest, a list of tests to read where it would have listed a
directory). Under the scheduler, natively, that first failed 28,321 tests
where the forked run fails 28,320. The one was upstream's own test of the
time base, `mftb_1`: two reads in a row must not be the same moment, and on
virtual time they were - both in one block, a quarter of a nanosecond apart.
A read of the time base now costs one of its ticks (20 ns), as an
instruction that slow does. With that the in-process run under the scheduler
fails the forked reference's set exactly, and the game still repeats (the
same memory at five seconds; 258,326 handings over where it was 257,171,
because clock reads now move the clock).

## M2: the memory in one block (2026-10-10)

Upstream's emulated memory is one shared file seen in several places: the
512 MiB of physical memory at 0xA0000000, 0xC0000000 and 0xE0000000 (4 KiB
on), its first 16 MiB again from 0x7F000000, and the 0x80000000 code range
again at 0x90000000. A guest has anonymous memory only; a page is at one
address.

Patch 0006 (`CHIMERA_FLATMEM`, on in the scheduler tree) makes the address
space ONE block of 4 GiB. Each alias is folded onto what it is an alias of
before it is used, and physical memory is the block's own 0xA0000000 range.
The fold is a table of 256 numbers, one for each top byte of an address,
kept just below the block: `folded = address + table[address >> 24]` in 32
bits. Every way from a guest address to a host one turned out to pass
through four places: `Memory::TranslateVirtual` (421 uses), the same in the
recompiler's context, the heaps' `TranslateRelative`, and the recompiler's
two address functions with three stragglers (the compare-exchanges and the
cache-control sequence). The recompiler's fold is four instructions and one
register: `mov eax, a; shr eax, 24; mov eax, [membase + rax*4 - 0x400]; add
eax, a`.

**Device registers are found by address, not by fault.** A guest cannot
catch a fault and read the processor's state, which is how upstream emulates
a load or store that lands on a register. Canary has the other way already
(`emit_inline_mmio_checks`: every 32-bit load and store checks whether its
address is in 0x7FC00000-0x7FFFFFFF); the driver turns it on. It also keeps
the register ranges from being protected, which matters here: 0x7F...... is
an alias of physical memory, and protecting the folded page would have
taken 64 KiB out of the console's RAM.

Proved natively, where both layouts can be had: with the block and the
inline checks the game's run is the run it was - 258,326, 437,
ed611adef65dbf20 - and the instruction tests fail the reference's 28,320.

One thing the views gave that a block cannot. Upstream protects a page in
the guest's views to notice a write (the GPU's caches do this) while
reaching the same page itself, unprotected, through the physical view. With
one block the emulator's own access meets its own protection. What that
needed is under "M4: the picture, through the bridge" (a watched page) and
under "A watched page, written through another address" (issue 249).

## M2: the recompiler from the memory base (2026-10-10)

Upstream's x64 backend is built on 32-bit absolute host addresses: its table
of function addresses is AT host address 0x80000000 (so that the slot for a
guest function is at the function's own guest address, read with a 32-bit
load), its code at 0xA0000000, its constants at 0x20000000, its trampolines
somewhere below 2 GiB, and every "host address" it stores is 32 bits. A
guest has nothing below 4 GiB.

Patch 0007 (part of `CHIMERA_FLATMEM`) puts all four at fixed places
relative to the console's memory block, and generated code reaches them from
the register that always holds the block's address:

- the constants just below the block (`[membase - 0x8000 + n]`);
- the trampolines and the code in the block's top GiB, which the fold leaves
  untouched (0xC0100000 and 0xC1000000);
- the table from the block's last 64 KiB onwards, 512 MiB past its end, so
  that the slot for guest address g is `[membase + g + 0x7FFF0000]`.

And the block begins on a multiple of 4 GiB (4 GiB more are reserved to
find one), which is what makes this small: the low 32 bits of any address in
the block are its distance from the base, and that is exactly what the
backend already keeps wherever it keeps a 32-bit host address. A call
through the table is `mov eax, [membase + rbx + 0x7FFF0000]; add rax,
membase; call rax`.

Two things found by running it. xbyak takes a displacement as an unsigned
number and a direction, so a negative one handed over as a sum throws
"offset is too big" - twice, the second time from two helpers that named
their constants without going through the function everything else uses.
And an indirect call's target register may be rbx itself, with anything in
its upper half: upstream's 32-bit load did not care, the 64-bit one jumped
into the console's RAM and ran it as code (the run hung with one thread in
unnamed code and time standing still). It is zero-extended always.

Proved natively: the game's run is the run it was (258,326, 437,
ed611adef65dbf20) and the instruction tests - a new recompiler for every
test, in the places the last one left - fail the reference's 28,320.

So the three things known from the source to stand between the recompiler
and a sandbox are done on the native build, where each could be compared
with what it replaced: the memory, the registers, the recompiler's own
addresses. What is left of M2 is the sandbox itself.

## M2: in the sandbox (2026-10-10)

`waterbox/build-guest.sh -m <miniBox>` builds Xenia with miniBox's guest
toolchain and links two programs: `build/wbx/ppc-tests.wbx`, upstream's
instruction tests behind one exported call, and the core. Each has a host
runner beside it (`run-ppc-wbx`, `run-wbx`).

What a guest needed that the native build had from the host:

- **A handful of system calls answered in the guest**
  (`waterbox/guest/guest-libc.cc`). miniBox ends a guest that makes a call it
  does not know, and the first one was `getcwd`. The working directory is
  "/", making a directory or removing a file fails as on a read-only disk,
  the processor affinity and the scheduling calls succeed and do nothing, and
  the kernel's random bytes are a fixed stream.
- **No Vulkan device for the renderer that draws nothing.** Upstream's
  "null" graphics system makes a Vulkan provider anyway; with
  `CHIMERA_HEADLESS` it does not.
- **A disc image that is read, not mapped** (patch 0008,
  `CHIMERA_NOFILEMAP`). Upstream maps the whole image; a guest has anonymous
  memory only, and the image is 7.3 GiB. The device now seeks and reads.
- **No directories.** The emulator's roots are names nothing is under, its
  per-game cache of instruction facts is off (it would also make a run
  depend on the runs before it), and nothing is created at start.

**The game.** Mirror's Edge, null renderer, native build against sandbox,
each a new process:

| the machine's time | baton handed over | frames | 512 MiB of RAM | host seconds, native / sandbox |
| --- | --- | --- | --- | --- |
| 5 s | 258,326 | 437 | ed611adef65dbf20, both | 12, both at once |
| 30 s | 1,783,398 | 2,681 | ea99a588e538d7ee, both | 185, both at once |

The sandbox is not slower than the native build.

**The compiler is part of the machine.** The first thirty-second comparison
failed: the same schedule and the same frames, and one MiB of the console's
memory (physical 0x135xxxxx) different in single bytes, ff in one run and 00
in the other. The native build was clang's and the guest is GCC's; a native
build made with GCC 14 ends with the sandbox's memory exactly (at fifteen
seconds c9904a6bbb738513, where clang's build has 3ffdad3712978627). At five
and ten seconds all three agree, which is why the GCC section above could say
they did. So `build-native.sh` builds the reference with GCC, the guest's
compiler, and the earlier tables in this file that say 489c3a26d743dc0b for
thirty seconds are clang's. WHICH code the two compilers disagree on is not
found. The bytes look like packed vertex data, which points at a conversion
from floating point to integer in one of the emulator's own helpers whose
result for an out-of-range value the language leaves open. It matters beyond
tidiness: a core built by another compiler version could be another machine,
and nothing here would notice before a movie desynchronised.

**The instruction tests.** A new recompiler is made for every test, and in
the sandbox that is slow: the whole set takes half an hour where the native
runner takes four minutes with a process per test, or one minute in one
process. Most of that is miniBox finding room for each recompiler's large
allocations by scanning its page table; it is not the recompiled code.

The first whole run failed 28,321 tests, the reference's 28,320 and
`stdcx_fail`. That one is not the sandbox: the test reads three bytes more
of the scratch memory at 0x10001000 than it writes, upstream runs each test
in a new process where that memory is zero, and in one process it holds what
the test before left - and "the test before" depends on the order the
directory is listed in. The runner now clears the scratch range before every
test (patch 0005).

With that, the whole set in the sandbox: **170,417 tests, 142,097 pass and
28,320 fail, the reference's 28,320 by name** (the names' SHA-256 begins
d1195e69efa280d0, as natively), in 29 minutes. The reference itself was made
again with the GCC build, forked and in one process under the scheduler, and
is the same set both ways.

## M3: a frame, a pad, a state (2026-10-10)

`waterbox/driver` is the machine behind the exports the frontend calls
(`wbx-entry.cc`: `Init`, `FrameAdvance`, `SetAxis`, the picture, the sound,
`InputWasRead`, a memory domain and a bus). The same two files are the native
reference (`build/vsched/bin/Linux/run-frames`) and the core
(`build/wbx/core.wbx`, run by `build/wbx/run-wbx`), and one harness
(`waterbox/gate-harness.h`) drives both, so the two runs are asked the same
things in the same order.

- **The frame** is a sixtieth of a second of the machine's time, counted
  from when it was switched on: frame k ends two microseconds after
  k/60 s. The vertical blank is raised on the same count (the thread that
  raises it upstream paces itself from wherever it first woke; under the
  scheduler it sleeps to the next multiple instead), so the frontend's frame
  and the console's cannot drift apart.
- **The pads** are a driver that answers with what the frontend set: four
  ports, the console's own sixteen-bit button word for each (sixteen bits a
  port in the frontend's 64) and six axes (the triggers, then each stick's x
  and y). Keystrokes - the other way a program reads a pad - are made from
  the same state by upstream's rules, with the repeat on the machine's
  clock. A frame in which the program asked for a pad says so, which is what
  the frontend's lag counter is made of.
- **The sound** is what the program hands the console's output: 256 samples
  of six channels at 48 kHz, mixed down to stereo as upstream's drivers do
  it, and taken by the frame. Nothing is ever waiting to be played, so the
  program is never held back by sound. The media player's output (music
  from the dashboard's library) is heard by nobody.
- **The bus** is the console's 4 GiB address space. There is no memory
  domain: see "an observer could hang the machine" under M4.

Measured, Mirror's Edge, 300 frames (five seconds), a digest at every
sixtieth - time, handings over, the GPU's counter, frames in which a pad was
asked for, the sound so far, the 512 MiB:

| run | what it ended with |
| --- | --- |
| native, twice | 258,708 handings over, a pad asked for in 173 frames, 238,592 sample pairs (c241628de0ac8e6c), RAM 64bc985a2c3c692b - the same lines both times |
| in the sandbox | the same lines; seven host seconds |
| A held for ten frames from frame 150, native and sandbox | 258,706, and 247 ns earlier - another run, the same one in both |
| a state saved and loaded before every frame (sandbox) | the lines of the run that was never stopped |
| a state saved after frame 155 by one process (359 MB), loaded by another | ends where the unbroken run ends |

States needed nothing written for them: every thread of the machine is a
thread of the guest, parked in the scheduler when a frame ends, and miniBox
keeps the guest's threads with its memory.

What the pad leg does and does not show. The program is seen to be handed
the pad (a trace of the driver's answers, taken once by hand, has the button
and the stick in every poll of the ten frames), and the run with a button is
another run. But the game does nothing with a pad in its first ten seconds:
its memory at the end is the memory of the run without (ten seconds with A
pressed every half second: 44 more handings over, the same 512 MiB). A leg
in which the game ACTS on input needs a run that reaches its menu, which is
past where anything here has run.

## M4: the picture, through the bridge (2026-10-10)

Xenia's Vulkan renderer needed three things to draw with no window, and one
more to draw in the sandbox.

- **A presenter with nothing to present to.** Upstream makes its presenter
  only for a window, though the presenter's image of what the console shows
  does not need one (upstream says so in a comment, "for offscreen use").
  With `CHIMERA_HEADLESS` the emulator asks for presentation, and the
  provider makes its instance and device with no surface, no swapchain and
  no queue that can present.
- **The picture read back** by the frame (`waterbox/driver`): when a frame
  ends, the presenter copies its image out through the device, and that is
  what `GetVideoBgra` hands the frontend, at the size the game draws it:
  1280x720 for most games, 1920x1080 for some (the package declares a
  1920x1080 buffer; chimera issue 270). `SetRenderingEnabled` off skips the
  copy and nothing else.
- **The loader is the bridge** (patch 0009): in a guest there is no library
  to load, and `vkGetInstanceProcAddr` comes from the bridge's guest half
  (`chimera_vk_lookup`). `build-guest.sh` generates both halves from the
  registry miniBox carries; the guest's is an archive of the core, the
  host's is compiled into `run-wbx`, which hands the bridge over before
  `Init` (`SetVulkanBridge`).
- **A watched page** (patch 0006). The renderer's caches protect pages of
  the console's memory to hear of writes to them, and natively the fault
  arrives in a signal handler. A guest is handed the address and whether it
  was a write (`GuestFaultHandler`), which is all that path needs once the
  device registers are found by address.

The first run through the bridge ended with one call refused: the version
query a renderer makes before it has an instance, which the host half could
not answer until an instance existed, so Xenia took the device for Vulkan
1.0. Fixed in miniBox (vk-bridge 710768f), with a test that fails without it.

**Measured**, Mirror's Edge, 300 frames, llvmpipe (Mesa 25.2.8), a digest at
every sixtieth frame:

| run | result |
| --- | --- |
| native, Vulkan, three times | the same lines, pictures included |
| sandbox, through the bridge, twice | the same lines as each other and as the native run: schedule, sound, memory and every picture (frame 300 is bc114f4003fc0030, the publisher's logo, and the file written out is the native run's file byte for byte); no call refused; 12 host seconds |
| either, against the renderer that draws nothing | the same 512 MiB at every sixtieth frame; the schedule differs by a few handings over |

So the machine does not depend on what draws it, as the survey expected: by
default nothing the GPU draws is read back into the console's memory.

**Two things found on the way that were not the renderer's.**

*An observer could hang the machine.* The gate's digest read the console's
512 MiB from outside, and with one block that is not safe: a page the
program has closed (decommitted, or protected against itself) faults for a
reader as it would for the program, and the fault was answered "handled"
with nothing changed - for ever. It first showed as a per-frame digest that
never reached frame 3. There is now an observer's way in
(`MMIOHandler::BeginObserving`): between the two calls a closed page that
faults on a read is opened, and closed again at the end, and nobody in the
machine is told. The digest and the bus use it, and the core has NO memory
domain - a tool reading a pointer from outside would meet the same fault -
only the bus. The gate reads the bus at six awkward places at every digest
(nothing at all, the program, the GPU's registers, physical memory, a
megabyte near the top, the last page).

*A thread the sandbox could not have.* With the Vulkan renderer the native
run and the sandboxed one differed by a few handings over from the first
frame. A trace of every handing over (`--trace-schedule`) put the first
difference on line 14: a thread named "Shader Storage Writer", which
upstream starts to write the game's shaders beside its cache for the next
run, and which a guest with nowhere to write never starts. The store is off
(`store_shaders`): it would also have made a run depend on the runs before.

Not done here: the frontend's side (an engine export to hand the bridge
over, as `SetGpuBridge` is for OpenGL - a change to what Chimera is, so the
owner's), a real card (the GTX 1060 is on the Windows side, and the gate's
runner is Linux-only), and a second game.

## M4: the renderer on another device (2026-10-10)

The bridge makes every load a new device: nothing a savestate names can be
trusted to exist in the process that loads it. Upstream answers a lost
device with a fatal error (its own comment lists what would have to be
reset, as a TODO). Patch 0010 is that TODO, for this one case.

**Made again.** `CommandProcessor::ChimeraRemakeContext`: the context is
shut down (upstream's own `ShutdownContext`, which already lets go of
everything), the provider and the presenter are made again, the four members
of the Vulkan command processor that were constructed with the device are
taken apart before it goes and made in place after, the context is set up,
and the two shaders in use - which belonged to the context - are loaded
again from their own code. After a load the old device answers nothing, and
that is harmless by the bridge's design: a call that names something gone is
refused (some 180 of them a load, all teardown).

**When.** Not from the renderer's own thread. Only one thread of the
machine runs at a time, so the frame's thread can use the renderer as its
own thread would, provided that thread is parked with nothing of a draw half
done. It says when it is (`ChimeraQuiet`): out of packets, or inside the one
packet that waits for the program (`WAIT_REG_MEM`, where a loading game
keeps it for whole frames). A frame ends by waiting for that - it is nearly
always true already, and the wait is given up after one frame's time - and a
remake is done at the start of the next frame, before anybody runs. So no
thread is ever told to stop, and a frame's end did not move: the digests of
a run with all of this are the digests of the run before it existed.

Two things upstream had to give up for it:

- **Pipelines are made by the thread that needs them**
  (`vulkan_pipeline_creation_threads` 0). The threads upstream makes them on
  would have to be ended and started again at every remake, and a thread
  started later sits in another place in the order threads run in - another
  machine. In a sandbox they were no faster anyway. With them gone the
  machine's schedule with the Vulkan renderer is, to the last handing over,
  its schedule with the renderer that draws nothing.
- **A wait that spun.** `WAIT_REG_MEM` with a short wait is an empty loop
  upstream, waiting for another thread to write a value; with one thread
  running it would never have ended. It yields.

**What only the device holds.** Two things, and a state must have both or a
load shows a picture the run never had:

- what the device wrote into the console's memory by resolving a render
  target - which upstream keeps on the device and never writes to the
  console's RAM (the front buffer the program presents is one of these: the
  first trial showed black after every remake);
- the render targets themselves, the EDRAM, 10 MiB.

`ChimeraKeepDeviceState` fetches both into the emulator's own memory - the
ranges of the shared memory marked as written by the GPU, and the EDRAM by
upstream's own dump of every host render target into its buffer, the recipe
the Direct3D renderer uses for a trace's snapshot. `ChimeraPutBackDeviceState`
uploads the ranges as GPU-written ones and restores the EDRAM into one
render target that owns every tile (a port of the Direct3D renderer's
`RestoreEdramSnapshot`, which the Vulkan one had as an empty function).

The alternative was upstream's `readback_resolve`, which writes resolved
data into the console's RAM as the console does. It was not taken: the
machine's memory would then hold pixels, and pixels are the card's - a
movie made on one card would be another machine on another. As it is, the
512 MiB are the same whatever draws, and what the device holds travels in
the state beside them.

The frontend's engine says when a state is about to be saved (`StateSaving`,
an export it already calls for another core), and that is when the fetch is
made; the gate's harness calls it as the engine does.

**Measured**, Mirror's Edge, llvmpipe, digests at every twentieth frame,
pictures included:

| run | result |
| --- | --- |
| native, the renderer made again on a new device before every frame, 300 frames | the lines of the run in which it never was |
| sandbox, a state saved and loaded before every frame, 300 frames | the same lines; 54,613 calls refused, all of them teardown; 195 s |
| a state saved after frame 30 by one process, carried on by another | the same lines |
| a load before the renderer's thread has ever run | found by the second row at its first frame: there is no context to take down yet, only a device to replace |

Without the fetch (the first trial, and the gate's control): the same
schedule and memory, and a black picture wherever a remake came between a
resolve and the frame that showed it.

**Not established.** A real card. The path with host render targets is the
one every card takes (upstream's other path, fragment shader interlock, is
only taken when asked for, and its put-back - a plain buffer copy - has
never executed); the renderer's copy of the console's memory is one plain
buffer on every card (`vulkan_sparse_shared_memory` off). So what a real
card adds is its own driver, not another path of this code. Resolution above the
console's: upstream's dump does not exist there and the fetch says so; the
core draws at 1x. Speed: a save fetches 10 MiB and the resolved ranges from
the device every time. A second game. And host depth: a depth buffer kept
on the host at more than the console's 24 bits goes through the console's
format on its way into a state, which upstream's own snapshot does too.

## M5: the package, and the frontend's engine (2026-10-10)

`waterbox/build-package.sh -r <a Chimera checkout>` builds the guest and
installs `build/Cores/xenia.chimeraCore` there: the core, its declaration
(`waterbox.config`), the keys (`default_keybinds.json`), the project form
(`file_slots.json`) and the licences of what is in it
(`package-licenses.json`).

What the package declares:

- one system, the Xbox 360, and one kind of file: a disc image (.iso);
- four controllers of fifteen buttons and six axes each, named "P1 A" to
  "P4 Right Stick Y"; the Controllers setting says how many are plugged in;
- a Renderer setting: "vulkan-hw", Xenia's Vulkan renderer on the machine's
  card (the name's ending is how a frontend knows to hand the card over), or
  "null", which draws nothing and needs no card - the same machine either
  way, so a movie made with one plays with the other;
- a picture of up to 1920x1080, shown as 1280x720, at 60 Hz, sound at 48 kHz, a bus and no memory
  domain, and that its states survive a new graphics device.

**The engine's half** is not in this repository. A frontend has to hand the
Vulkan bridge to a core that asks, tell the bridge when a state was loaded,
and call `StateSaving` before it makes one (which it already does). That is
some sixty lines of Chimera's engine and one more export a core may have
(`SetVulkanBridge`), written on a branch (`vk-bridge`) of the checkout these
legs run against, with its own section in Chimera's docs/gpu-bridge.md and
design log. A new export is a change to what Chimera is, so merging it is
the owner's.

Measured through `chimera-run`, the frontend's headless runner, with that
branch's engine, llvmpipe:

| what | result |
| --- | --- |
| a movie of idle frames recorded by the engine and played back by it, `--gpu` | the bridge is handed over; the picture the engine is given at its frame 80 is this repository's runner's picture at its frame 81 (the engine counts from 0), pixel for pixel |
| the same movie with the engine's own save and load before every frame | the same picture |
| the same movie with no card asked for | refused before the machine starts: "the Vulkan renderer needs a device, and none was handed over" |
| the same movie with the renderer "null" | runs to the end |

The frontend's contract tests for installed packages found one thing: the
`extensions` declaration is a map from an extension to a system, and the
first draft had a list.

**Not established.** The interface: only the headless runner has opened the
package. Windows, where the owner's card is: the engine branch compiles for
it and has not run there. A project (the wizard's form has not been looked
at). Save data in and out (see "Storage" below). The network: miniBox
answers a socket with an error, which is what "there is no network" needs,
but no game that asks has been run. The licence list was
written from the tree and has not been reviewed; nothing may be published
on it as it stands.

## A real card, and the first thing the game asked for (2026-10-10)

The package, in `chimera-run.exe` cross-built from the Chimera branch and
run on Windows through WSL's interop (staged in a folder of its own, the
disc image read from where the owner keeps it):

    chimera vk: the bridge was handed over
    chimera vk: NVIDIA GeForce GTX 1060 6GB (Vulkan 1.4.312, driver 69.168.0)

| what | result |
| --- | --- |
| 100 idle frames recorded, then played back | runs; 8 host seconds |
| the same with the engine's save and load before every frame | runs; the picture at frame 80 is the picture without |
| the card's picture of a frame against llvmpipe's of the same frame | the same picture from another rasteriser: no channel differs by more than 4 in 255 |
| 2400 idle frames (forty seconds of the machine's time) | the publisher's logo, the developer's, and from frame 1200 the title screen - the logo, "Press START button" and the city behind it, drawn right; 373 host seconds |
| a movie that presses Start at frame 1300 | at first, the end of the machine (below); then the game's own dialog: "You are not signed in. In order to save progress, you will need to be signed in. Do you want to sign in now?" |

So a pad reaches the game and the game acts on it - which the gate's pad
leg, on the first five seconds, could not show.

**The crash was the emulator's, and was two things.** The game answered
Start by asking the console for its sign-in screen. Upstream has an answer
for when nobody is there (`headless`), and that answer was given through
the window - of which there is none: a null pointer, and miniBox's note
named the function. The same answer then starts a host thread to say, 200
ms later, that the console's screen has closed: a thread outside the
schedule, sleeping on the host. Now the answer is given where it is asked
when there is no window, and "closed" is said by a thread of the machine's,
on the machine's clock (patch 0002, with the same guard for an achievement's
notice, which would have ended the machine at the first achievement).

**What the game wanted next** was a profile to be signed in as and storage
to save to. Upstream keeps both in directories of the host, and a guest has
none. See the next section.

**Speed** was a ninth of real time on that run. Where it went is its own
section, below.

## Storage, and somebody signed in (2026-10-10)

The emulator's three roots - storage, content (profiles, saved games) and
cache - are now a filesystem in the machine's own memory
(`waterbox/driver/memfs.cc`, the Vita3K core's, where it was written and is
tested, with two additions). It sits under the C library: every path under
`/chimera` is served from a tree in memory, in both flavours of the core, so
the native reference and the sandbox see the same files in the same order
with the machine's own dates, and every savestate has them. It is empty when
the machine is switched on.

The two additions, both found by the sandbox ending the machine: the C
library's `remove` makes its system calls itself and went past the tree
(the C++ library's `std::filesystem::remove` is that call); and a path
outside the tree, which in a sandbox can only be read, now answers "the disk
cannot be written" to anything that would write, where it would have made a
system call the sandbox does not have.

One profile is made when the machine is switched on and signed in on the
first pad: "Player", with upstream's own fixed identity for a default
profile. A game that will not save, or will not start, without somebody
signed in has somebody - the same somebody in every run.

Measured: the native run and the sandbox still agree at every sixtieth
frame (the console's memory is what it was; the schedule moved a little, a
signed-in console being another console). And on the GeForce GTX 1060, a
movie that presses Start at frame 1300 and A at 1600 and 1800:

| frame | what the game shows |
| --- | --- |
| 1290 | the title screen, "Press START button" |
| 1450, 1590 | its autosave notice: "Please choose device to store your game progress on" |
| 1750 | the main menu: Story (New Game), Race, Options, Extras |
| 1950, 2099 | Story - New Game: choose difficulty |

**Out, and not yet in.** The frontend's Export Save Data brings out every
file of the console's content storage, by its path there
(`content/B13EBABEBABEBABE/...`): after a hundred frames that is four
files, the profile's account and its two records among them. Nothing puts a
save IN before the game starts (a project's save slot); the Vita3K core has
that to copy, and it needs a zip reader this core does not link yet.

**Not done.** Nobody has played past the menu, so no level has been drawn
and no game has been seen to save. The profile's name and identity are
fixed and are not settings.

## Speed: what the machine's clock costs the host (2026-10-10)

The first runs on the card were ten to twelve times slower than real time
from the title screen on. Measured before anything was changed, natively,
with the renderer that draws nothing (`run-frames --every 300 --pace`,
which writes for each stretch the host's seconds, the guest's instructions,
the handings over, and the four threads that ran the most):

| stretch (5 s of the machine each) | host seconds | guest instructions | who |
| --- | --- | --- | --- |
| the logos | 4.5 to 6.7 | 1.9 to 2.1 thousand million | one thread, nearly all of it |
| the title screen and the menus | 52 | 18.5 thousand million | the same thread, 17.4 of them |
| loading, the opening, the first rooftop | 5 | 2.6 thousand million | the same thread |

And the same sixty seconds at other costs of an instruction:

| an instruction costs | instructions the machine runs a second | host seconds for 70 s of the machine | pictures the game presents |
| --- | --- | --- | --- |
| 250 ps | 4,000 million | about 370 | 30 a second throughout |
| 1000 ps | 1,000 million | 139 | 30 a second; the first logos come later |
| 2000 ps | 500 million | 76 | 30 a second once started; it starts much later |
| 4000 ps | 250 million | 32 | the title is reached 40 seconds in |

Three things follow.

- **The host runs 0.3 to 0.57 thousand million guest instructions a
  second**, one thread at a time. That is the ceiling: a second of the
  machine in which the program runs N of them costs N over that.
- **This game never waits.** One thread - the one that plays the movies,
  polls the pad and starts the others: its main loop - runs flat out between
  frames, in whole slices with no call into the kernel. Whatever the clock
  allows in a second, it takes. So the host's cost is the clock's rate and
  nothing else, and the game's own work is a few percent of it: it holds
  its thirty pictures a second on a quarter of the processor.
- **The card is not the cost, nor is Windows.** Six hundred frames at the
  logos take 7.1 s in the sandbox on Linux with nothing drawn, 8.2 with
  llvmpipe, 8.3 on Windows with nothing drawn and 9.0 on the GeForce GTX
  1060. The bridge's own count (`CHIMERA_VK_STATS=1`, miniBox) for fifty
  seconds at the title: 2.07 million calls, 2.2 s in the host half and the
  driver together.

So how fast the console's processor runs is a setting of the core, **CPU
Speed**: how many million instructions the processor runs in a second, all
six of its threads together. The choices are 500, 1000, 2000 and 4000. A
movie made at one speed does not play back at another, because the game
does a different amount of work in each frame.

The default is 500 (user-decided, 2026-10-11): "Set 500 as default. But say
in the description that 2000 is closer to actual console performance and
500 is chosen for better TASing experience". The setting's description in
`waterbox/waterbox.config` says both. It was 1000 for one day before that.

What was measured at each speed, on the GTX 1060 unless said otherwise:

- 1000: fifty seconds of the game, from power-on to the title screen, take
  149 seconds. Ninety seconds, through the menus, the loading screen and
  the animated opening, take 226: two and a half seconds for each second of
  the game, where the first runs took ten to twelve. In those ninety
  seconds the bridge counted 1.93 million Vulkan calls, which cost 2.7
  seconds in the bridge and the driver together.
- 500: natively with nothing drawn, 70 seconds of the game take 76 (the
  table above). The game starts later at this speed: the picture is blank
  for the first 180 frames, the first logo stands still from frame 200 to
  280, and the game first asks for the controller around frame 290.

What this is not: a model of the console. One clock for six hardware
threads is one processor shared, where the console has three cores; a
thread that spins on the console burns one of six, and here it burns the
clock. A scheduler with a clock for each thread would let the spinner cost
what it costs the console - which, at one thread's real rate, is still more
than this host runs in a second. And nothing knows what the main thread is
waiting for; if it is the vertical blank, a wait that could be named could
be made to cost nothing.

## The gate (2026-10-10)

`waterbox/run-gate.sh`, 14 legs with `CHIMERA_ROOT` set and 10 without,
eight minutes. The four it adds are the frontend's: three described under
M5, and the save data export. The
ten: two native runs are one run
and a run with another instruction cost is another; the sandbox is the
native run at every sixtieth frame; a button makes another run, the same in
both; a state saved and loaded before each of 300 frames is the run that
was never stopped; a state saved by one process is carried on by another;
300 frames drawn through the bridge are the 300 drawn without it; the
renderer made again on a new device before every seventh frame changes
nothing, a state saved and loaded before every frame with that renderer
changes nothing, and a state carried into another process ends with the
unbroken run's picture; and every eighteenth file of upstream's instruction
tests fails, in the sandbox, what the reference fails.

Each leg has been seen to fail, by hand, once:

- the instruction cost and the button are in the gate itself (a run that
  must differ);
- a state that is not this moment's (`HARNESS_BREAK_STALE_STATE`: half way,
  the state loaded is the one from the frame before): the run ends 16.7 ms
  behind with other memory;
- a bridge whose flush writes nothing (miniBox's `VKB_TEST_BREAK_FLUSH`
  built into the runner): every picture is the blank one, and the machine
  is untouched;
- a state made without asking the device for what only it holds
  (`HARNESS_BREAK_NO_STATE_SAVING`): the same schedule and the same memory,
  and every picture after a load the blank one;
- the frontend's picture against the runner's picture one frame away (the
  comparison, `waterbox/tests/same-picture.py`, by hand): 118,639 of 921,600
  pixels differ; and two blank pictures are refused as "same, and blank";
- one failing test taken out of the reference (`XENIA_PPC_REFERENCE`): "1039
  fail, the reference says 1038", and the test is named;
- a part of the instruction tests in which nothing fails is refused, which
  the first choice of part (every twentieth file) was.

- the default speed (added 2026-10-11, the test `speed:default`): a run
  that names no speed against a run at 1000 instead of 500, by hand - the
  two differ after 120 frames (22,906 thread switches against 11,959), so
  the test would say FAIL if the default moved.

Changed on 2026-10-11, with the default speed. Every test now runs the game
at the fastest speed (4000, which is 250 ps an instruction) and says so. At
the default the game shows nothing for three seconds, and the tests that
compare pictures would be comparing blank ones, which they refuse. One new
test, `speed:default`, checks that a run that names no speed is the run at
500 and is not the run at 4000. With `frontend:pads`, added the same day
("Published" below), that makes 16 tests with `CHIMERA_ROOT` set, about
seven minutes.

Not controlled: the sandbox leg against a sandbox that is subtly another
machine. What stands in for it is that it did fail, for real, three times
on the way here - a clang-built reference, a test order, a thread the guest
could not have - and each time named the frame.

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
everywhere, so it is a constant of the core and not a setting.

**AVX2 and FMA, and nothing else (user-decided, 2026-10-10).** The choice
was put to the owner as nothing above AVX (every machine that can run this
has it), AVX2 with FMA (nearer the console's fused arithmetic, and faster,
but a processor of 2013 or later), or measuring first; he chose "Require
AVX2 + FMA". The driver sets `x64_extension_mask` to those two bits and
refuses a host that lacks either; `run-ppc-tests.sh` runs upstream's tests
with the same mask, and that result is the reference from here on: 142,097
pass and 28,320 fail (the names' SHA-256 begins d1195e69efa280d0) - the
every-feature run's set exactly. Of what this machine has, AVX2 and FMA are
the only features that change a result; the rest only change the code.

## What is the owner's

- **Content.** One game, the owner's disc image of Mirror's Edge, outside
  the repository (`tests/roms`, ignored). Every game leg of the gate needs
  it and is skipped without it; what is proved is proved on that one game's
  first thirty seconds.
- **The repository.** Public since 2026-10-11, at the owner's word ("Create
  a public repo"): `ToolAssisted-run/chimera-core-xenia`.
- **The list of official cores.** Chimera's `official-cores.json` and the
  README's table do not name this core. Adding it is the owner's decision
  and has not been asked for.

## Published (user-decided, 2026-10-11)

Three decisions of the owner's, the same day: merge the Vulkan bridge into
miniBox's and Chimera's `main`, make this repository public, and make 500
the default CPU Speed.

What CI proves and what it does not. A public runner may not have an Xbox
360 game, so on a runner every test that runs a game is skipped. What runs
there: the whole build from a fresh clone, Xenia's instruction tests in the
sandbox compared with the native build's results, and Chimera's own checks
that the package loads and declares its controllers, settings and file
types properly. The workflow fails if the instruction tests did not run,
so that a run that skipped everything cannot pass. The tests with the game
are run on the developer's machine before a push, and the commit says what
they printed.

What happened when it was published, 2026-10-11:

- A build from a fresh clone in another folder, tested with the game, gave
  the results of the developer's own tree: 15 tests passed, none failed,
  none skipped, with the same time, thread switches, sound and memory at
  frame 300. So the folder a package is built in does not change how a game
  runs, although the two core files are not the same bytes.
- The first CI run failed before it reached this repository's code:
  Chimera's own Linux build needs five system libraries (EGL among them)
  that the workflow did not install. A local fresh clone could not show
  that, because it was built beside a Chimera that was already built.
- The second run passed in 64 minutes: on the runner 1 test passed and 14
  were skipped for want of a game, Chimera's package checks passed 17 of
  17, and the release "Development build 494a351c" was published.
- Chimera's window was run with the package on Windows on the GeForce GTX
  1060, driven by a script (`Chimera.exe --headless` with a Lua script):
  it started the game through the bridge, ran 420 frames, took screenshots,
  saved the project, and opened the saved project again.
- That run showed a defect: the input log had columns for all four
  controllers with one plugged in. Fixed the same day: the core now tells
  Chimera which controllers are plugged in (the optional exports
  `IsButtonActive` and `IsAxisActive`), and a movie has columns for those
  only. A movie recorded before that fix has four controllers' columns and
  does not match. The test `frontend:pads` checks it; before the fix a
  recorded row had four controllers' columns whatever the setting, which
  is what the test refuses.

The licence list (`waterbox/package-licenses.json`) was checked against
what is actually linked into the core. Three entries were for libraries
that are not linked (snappy, RapidJSON, the DXBC checksum) and were
removed; three that are linked were missing and were added (cxxopts, and
Xenia's copies of a DES and a SHA-256 implementation). 27 components.


## A watched page, written through another address (2026-10-12, issue 249)

Reported with four games (Dead or Alive 4, Battle Fantasia, Burnout
Revenge, Dead or Alive 5): after a few hundred or a few thousand frames
"the core crashed: it wrote to address ...". The four logs show the same
thing. The instruction that faulted was the game's own code storing eight
bytes of what looks like picture data, at the first byte of a page, and the
page was read-only. In each log one register holds an address just below
the store's, in the 0xE0000000 range.

What was wrong. The console's 512 MiB of physical memory are seen at three
places in its address space: 0xA0000000, 0xC0000000 and 0xE0000000 (and the
first 16 MiB again at 0x7F000000). Each place has its own heap in Xenia, and
a page is allocated in one of them. The renderer keeps copies of what the
game draws from, and to hear when the game changes a page it makes the page
read-only; each heap keeps its own list of the pages watched this way.
Upstream each place is a separate mapping, so a fault's address says which
heap to ask. This core keeps the memory as one block ("M2: the memory in one
block"), every address of a page is folded onto the 0xA0000000 one, and the
fault handler asked the 0xA0000000 heap only. A game that allocates at
0xE0000000 has its pages on that heap's list: the first heap answered "not
watched, and not mine to write", and the core was stopped. Mirror's Edge
allocates what the renderer watches at 0xA0000000, which is why it never
showed.

The fix (patch 0006, `Memory::AccessViolationCallback`): a fault on a page
of physical memory asks every heap that has an address for that page. The
one that watches it tells the renderer and opens the page; the others
answer as before.

The test, `memory:views`, needs no game, so it also runs on a CI runner.
The console is switched on with nothing in it. One page is allocated at
each of the three places and watched as a renderer watches it, and each is
written through all three of its addresses: nine writes. Each must land,
and the watcher must be told exactly once. It runs natively and in the
sandbox, and the two must print the same lines
(`run-frames --check-views`, `run-wbx <core.wbx> --check-views`; the core's
export is `CheckWatchedViews`).

- Before the fix: in the sandbox the core died at the first write to the
  page allocated at 0xC0000000, with the report's own message; natively the
  same write never returned.
- After it: nine writes, each told of once, the same lines in both.
- Broken on purpose (only the first heap asked again, by hand): the test
  fails with the report's message.
- Two mistakes in the first version of the check are worth knowing. A
  compiler does not see a fault handler coming: the count it changes must
  be `volatile`, or the value read after the write is the one from before;
  and two neighbouring one-byte writes were made one two-byte store placed
  after the count between them was read, so the writes are `volatile` too.

Not proved: none of the four games is here, so none was run. What is proved
is that the fault in their logs is this one - a write, by the game's code,
to a watched page allocated at 0xE0000000 - and that such a write no longer
ends the core. A game may of course stop for another reason further on.

The game that is here still runs with the change. The whole test script
passes (17 tests, none skipped). On Windows on the GeForce GTX 1060, in
Chimera's headless runner, a movie of 3,600 frames takes Mirror's Edge
through its menus into the first level, which is drawn as before.

With `memory:views` the test script has 17 tests with `CHIMERA_ROOT` set,
and two of them run on a machine with no game.

## A game that is a container: Xbox Live Arcade (2026-10-12, issue 250)

Asked for: Xbox Live Arcade games, with Xenia's `license_mask = 1` as an
option. An Arcade game is one file with no extension: a container (the
console's STFS format, beginning "LIVE") with the program and its data
inside. Xenia tells a container from a disc image by how the file begins,
so nothing in the driver had to be told which it is.

What a container needed in the sandbox:

- **The file is read, not mapped** (patch 0008,
  `base/mapped_memory_posix.cc`). Xenia opens a container by mapping the
  whole file into memory, and a sandbox maps no file. Under
  `CHIMERA_NOFILEMAP` the mapping is a block of memory the file was read
  into. Nothing written to it goes back to the file. Before this the
  sandbox refused a container: "the program did not launch (C000000F)". An
  Arcade game is small (the first ones 50 MB, the last ones up to 2 GB), and
  being read before the machine is sealed it is not part of every savestate.
- **A program that ends itself** (patch 0003, `KernelState::TerminateTitle`).
  A program can ask the console to end it: to go back to the console's
  menu, or to start another program. Upstream shows "please restart Xenia"
  and leaves. Here the request then waited for every other thread to reach a
  safe point, which with one thread running at a time never happens: the
  frame never ended, and Chimera would have hung. Now the thread that asked
  goes no further, a line on standard error says so, and the machine runs
  on with the last picture. This is not Arcade-specific - a disc game that
  quits to the console's menu met the same hang. NOT done: starting the
  other program. A game made of a launcher and a second program stops at
  the launcher.
- **The setting "Arcade Games"** (`arcade_license`: `full`, the default, or
  `trial`). An Arcade game and its trial are one file; the game asks the
  console which licences it has, and the first bit means "bought". `full`
  is Xenia's `license_mask = 1`, `trial` is 0. The driver sets it only when
  the program is a container, so a disc game is answered as it was before
  the setting existed and its movies still play back.
- **The file slot's help** says how to pick a file with no extension. The
  package still declares only `.iso`: Chimera matches files to cores by
  extension and such a file has none, so the core is chosen by hand and the
  New Project window notes that the file "is not one of the .iso" while
  accepting it.

Not supported: Games on Demand (a small file beside a `.data` folder of
data files: more than one file, which the game slot does not take), title
updates and downloadable content.

**The test, and what it does not show.** No Arcade game is here, and none
may be in this repository. `waterbox/tests/make-container.py` makes a
container out of the disc image that is here: it finds `default.xex` in the
image and writes it into a container of the read-only layout downloaded
games have. The tests `container:launch` and `frontend:container` use it.

- Natively and in the sandbox the container is mounted, the program is
  loaded from it and started, and 120 frames give the same lines in both
  (frame 120: 8,313 thread switches, 180 pictures and blanks counted). The
  program is Mirror's Edge with none of its data, so it asks the console to
  end it at once; that request is what showed the hang described above.
- With the core as it was before, the same container is refused in the
  sandbox with "the program did not launch (C000000F)".
- Chimera's headless runner starts the package with the container, with
  Arcade Games at `full` and at `trial`, and refuses another value.

With these two the test script has 19 tests with `CHIMERA_ROOT` set; all
pass, and the disc game's results are what they were (frame 300: 259,480
thread switches, memory 64bc985a2c3c692b). Chimera's own package checks
pass with the new setting (17 of 17).

So what is shown is the way in: opening, mounting, loading, starting. What
is NOT shown: a real Arcade game running, its picture, its saves, and
whether `full` really gives the whole game - the container here is written
by this repository's own script and read by Xenia's reader, and its program
never asks about a licence. That needs an Arcade game. Nor has a file with
no extension been picked in Chimera's New Project window; only the headless
runner has been given one.


## An Arcade game could not be made into a project (2026-10-13, issue 250)

The section before this one let the core start a game that is a container.
A person still could not use one: the game slot declared `.iso` and nothing
else, Chimera's New Project window said the file "is not one of the .iso
this slot expects", and when the project was made the engine refused it -
"file '...' is not a format slot 'game' accepts". An Arcade game has no
extension. The reporter's screenshot is that window.

It was missed because the two checks for containers run the core's own
runner and `chimera-run <package> <file>`, and neither goes through a
project: only a project's files are checked against the slot's formats.

The slot now declares the empty format beside `iso`, which is a file with no
extension (Chimera documents and shows it from dc0e8d0 on; engines before it
already accept it). New check `project:container`: a project whose game is a
file named like an Arcade game is opened by `chimera-run --project` and runs
60 frames. Run by hand against the package published before this, the same
steps end with the engine's refusal.

Still not proved: a real Arcade game. The test container is made from a
disc game's own program.


## A game that waits by reading the clock never gave way (2026-10-13, issue 258)

The Beatles: Rock Band stopped at frame 20 with the published core, and a
user's copy of Onechanbara: Bikini Samurai Squad at frame 10. Here the
Beatles did not crash: the run never reached frame 21, on Linux with no
renderer and on Windows with Vulkan alike (600 seconds waited on a GeForce
GTX 1060).

**What it was.** One thread runs at a time and keeps its turn until it has
spent a slice of 100,000 instructions ("M1" above). The instructions it has
run are turned into the machine's time whenever the scheduler is asked
anything - and that same step gave the thread a whole slice again. Asking
what time it is is one of those questions, and it hands the turn to nobody.
So a thread that waits for a moment by reading the clock in a loop, which is
what the game's main thread does there, never came to the end of its turn:
the clock ran on under it (277 seconds of the machine's time in 40 of the
host's), nobody else ran, and the frame never ended. Read off a debugger:
the count of handovers stood still at 670 while the time and the count of
instructions charged grew, and the slice was always found nearly full.

**The change** (patch 0003, `base/threading_chimera.cc`). Turning
instructions into time no longer touches the slice. A new slice is given
only where the turn can be handed over: at a yield and at a wait. A wait
that is answered at once, with its deadline worked out on the way, had the
same hole and is closed by the same change.

Every game's timing moves with this - a thread that reads the clock is now
interrupted where it was not - so a movie made with an earlier build of this
core will not play back with this one. A project names the build it was made
with, as always.

**Tested.** New check `clock:turn`, with no game: one thread reads the clock
in a loop, paying for instructions as recompiled code does, while another
sleeps five milliseconds of the machine's time. The sleeper wakes 5.23 ms
later, after 52,026 reads, natively and in the sandbox with the same line.
With the old behaviour put back for one build the reader ran its hundred
million reads, ten seconds of the machine's time, and "the sleeper never
ran". The Beatles: Rock Band then runs 600 frames with no renderer on Linux
in 11 seconds, with sound, and 900 frames on Windows on the GTX 1060 with
Vulkan, where it shows its first logo. Bullet Witch, which did not stop
here before either, runs its 900 frames there too.

**Not shown.** The crash the two reports describe - "it read or ran address
0x20" - was not seen here: the Beatles hung at the same frame instead, and
that address is in the middle of an instruction of the core, so something
had already gone wrong before it. It is taken to be the same fault ending
another way on the reporter's machine; that is a guess until they try this
build. Onechanbara was not tried: it is not here.


## A run with a rewind is the run without one (2026-10-13)

Asked by the owner after the same question found a fault in another core:
does a movie played with a rewind in it leave the machine a straight run
leaves? Measured on Windows on a GeForce GTX 1060 with Vulkan, by comparing
all 4 GiB of the console's address space at the end of a run (`chimera-run
--dump Memory=<file>` writes a fingerprint of every 64 KiB of it): a movie
played straight, the same with a savestate taken every few hundred frames and
never loaded, and the same with one rewind (back 500 to 700 frames, then on).

It did not. Three separate things, each found from the bytes that differed:

- **The card's answer written into memory.** A program asks how many pixels
  of something were visible (an occlusion query) and the answer goes into
  its memory. Xenia's default asks the real card and writes the answer "when
  the GPU catches up". A run that only took savestates differed from a
  straight one in the page of those answers (Mirror's Edge, 116 bytes), and
  the answer would be another one on another card. The core now always
  gives the made-up answer, which this file had said it did. Effects that
  depend on the real count (lens flares, culling by visibility) follow the
  made-up one.
- **Random numbers from the host.** The Beatles: Rock Band asks the console
  for 16 random bytes seven times before its title screen, and two straight
  runs of one movie left different bytes. In the sandbox the standard
  library's random device is the processor's own instruction for random
  numbers, which miniBox cannot stand in front of. The console's random
  numbers (`XNetRandom`, a new profile's id, heap scribbling) are now a
  fixed sequence kept with the machine (`xe::chimera::NextRandom`).
- **An address of the emulator's in the console's memory.** Each thread's
  block held the host address of its register context. Nothing reads it,
  and it was another address in a run that took a savestate before the
  thread was made. It is now zero.

**Measured after the three** (same card): Mirror's Edge over 1,500 frames
and The Beatles: Rock Band over 2,400 - straight, straight again, with
states taken, and with a rewind - leave the same 65,536 fingerprints, and
the same picture at the last frame.

New check `random:own`: no object of the sandbox build refers to the
standard library's random device (seen to fail with one use put back). The
other two have no check of their own: nothing in CI has a game, and the
checks here that load a state before every frame run a stretch of Mirror's
Edge that asks for none of the three.

Every game's memory moves with this (the answers to queries and the random
bytes are others), so a movie made with an earlier build will not play back.

Not measured: a game in play rather than its opening, and any other card.

## A game that never finishes loading at the default CPU Speed (chimera issue 266, 2026-10-14)

**Reported:** Prince of Persia Classic (an Xbox Live Arcade game) does not
work. With the container fix of issue 250 it starts, shows its logos and its
story screens, and then stays on "Now loading..." - for 40,000 frames, which
is as long as it was left.

**What it is doing.** Nothing is waiting. Two threads share the processor
half and half: the game's main thread, which draws the loading screen and
never gives way between frames, and the thread that loads the level. The
loading thread works in turns it times itself (it reads the clock, takes
memory, works, gives the memory back, reads the clock again), and at this
speed a turn's fixed work uses up the turn: the heap's addresses keep
moving and the level is never ready.

**It is the CPU Speed setting.** The same input at each speed, with the
renderer that draws nothing:

| CPU Speed | what happens |
| --- | --- |
| 500 (the default) | still loading after 40,000 frames |
| 1000 | still loading after 9,000 frames |
| 2000 | loaded within a few hundred frames |
| 5000 (not a choice of the setting; the runner's `--instruction-ps 200`) | loaded within a few hundred frames |

And on a GeForce GTX 1060 through Chimera's runner, the published core with
CPU Speed 2000: the story screens, then the first level with the prince in
it and the clock at 59:59 (9,000 frames, 722 seconds of the host).

**Changed:** the setting's text and the README say that some games do not
finish loading at the lower speeds, name this one, and say what to do. The
default is not changed: it is the owner's, and it was chosen so that a game
which keeps the processor busy runs near full speed.

**Not done, and worth knowing.** The console has six hardware threads and
this machine has one clock that every thread's instructions are added to,
so a thread that spins takes time from one that works. A clock that let
threads on different hardware threads run at the same moment would give the
loading thread its whole turn; it would also make every game that spins
cost the host more. That is a change to what the machine is, not made here.

**The default changed (user-decided, 2026-10-14).** CPU Speed now defaults to
2000, the closest to a real console, so a game like this one loads in a new
project without anybody having to know to raise it. The cost is host speed
for games that keep the processor busy between frames: about a quarter of
real time instead of near full. 500 and 1000 stay as choices. A project made
before keeps the speed it was made with (a project carries every setting),
so its movies are unchanged. The runners' own default, when no speed is
named, stays 500 (`speed:default`).


## A game that waits on the network (2026-10-14, chimera #269)

Dead Rising 2: Case Zero makes a socket, which the sandbox refuses, and then
waits for sockets to be ready with select. Xenia handed that wait to the
computer's own select, which the sandbox does not provide, and the machine
stopped at frame 129. With no network no socket is ever ready, so the wait
is now a sleep for its timeout on the machine's clock, and with no timeout
it never ends (patch 0003, NetDll_select). The test `net:own` checks that no
part of the sandbox build calls the computer's select or poll. The game now
runs past its logos; the rest of it has not been played.

## Memory asked for aligned (2026-10-14, chimera #272)

Ultimate Marvel vs. Capcom 3 and Marvel vs. Capcom 3 crashed within their
first frames, writing to address 0. Just before, MmAllocatePhysicalMemoryEx
had failed three times: "translated address F6A67000 misaligned (alignment
00002000, physical base offset 00001000)". The console's 0xE0000000 view of
its memory starts one page into physical memory, so memory aligned in
physical memory is one page off in that view. Xenia's test adds a host
offset that is one page only where the host hands out memory in blocks
larger than a page (Windows); the sandbox hands it out by the page, so every
aligned request in that view was refused, and the game wrote to the 0 it got
back. The three tests now ask what the console guarantees, that the
physical address is aligned (patch 0006). `memory:views` asks each of the
three views for memory aligned to 8 and 64 KiB, in a range and anywhere; it
failed at 0xE0000000 before. Ultimate Marvel vs. Capcom 3 now runs 1,200
frames in the sandbox with picture and sound; nothing past its opening was
played.

## Content and save data in a project (2026-10-14, chimera #271)

A project now has two more slots. **Content** takes packages - add-on
content (00000002), title updates (000B0000) and saved games (00000001) -
and puts each where standalone Xenia's Install Content puts it, under
`content/<owner>/<title>/<kind>/`, with its header beside it, before the
game starts. A package the console treats as read-only (add-on content, a
title update) is used as the one file it is, and here that file is the
project's own, served by the machine's file system without a copy: it costs
no memory and no space in a savestate, and it is not exported as save data.
A package a player writes (a saved game) is unpacked into the storage, as
Xenia does, and is the machine's from then on. After putting a package in,
the core asks the console to list that title's content of that kind - the
listing a game's enumeration makes - and refuses the project if the package
is not in it. Other kinds, and packages of the kind that is a file beside a
folder (SVOD), are refused by name.

**Save data** takes the zip Emulator > Export Save Data writes (every entry
under `content/`); it is written back into the storage before the packages.
Stored and deflated entries are read, with the zlib Xenia already has.

Tests: `content:install` makes add-on content from the disc
(`tests/make-container.py --kind 00000002`), has both runners put it in
(their new `--content`) and finds the console listing it; a package of
another kind is refused. `frontend:savedata-in` puts a zip in a project and
finds both its files byte for byte in the next export, the add-on content
not exported, and a zip holding anything outside `content/` refused. With
the add-on content the owner gave (Mirror's Edge, "Pure Time Trials"), the
console lists it under the game's title in both runners. Whether a game then
offers the content it finds was not played.

## A game on several discs (2026-10-14, chimera #281)

The Game slot takes up to four discs, in order. The first is started. When
the game asks for another (XamSwapDisc), upstream asks a person to pick a
file; here the core puts in the project's disc of that number
(`Emulator::ChimeraInsertDisc`, patch 0002 and 0003), at
`\Device\LauncherData` as upstream does. A disc the project does not have is
not put in and the log says so, and the game is told it was done, as
upstream tells it when the picker is cancelled. The machine's devices are in
its memory, so a savestate made after the swap has the second disc in.

The slot's list is now read name by name: a file name with a bracket in it
("Mirrors Edge [NTSCU][NTSCJ].iso") ended the list early, which would also
have dropped Content packages after such a name.

Test `disc:insert`: the test disc given twice, and the runners put disc 2 in
after frame 10 the way the game's request does (their new `--disc` and
`--insert-disc`); natively and in the sandbox the run carries on with the
same lines, and a disc 3 the project lacks is not put in. No game on two
discs was tried: the request itself, from a game, was not seen.

## Per-game settings from Xenia Manager's list (2026-10-15, chimera #289)

The owner asked for full per-game suggestions, taken from Xenia Manager's
list of optimized settings (github.com/xenia-manager/optimized-settings, a
port of A1eNaz's wiki), and carried with credit and a link though the list
states no licence (user-decided, 2026-10-15). Every value stays changeable.

- **Eighteen of Xenia's settings** are the core's own now, under Xenia's
  names (waterbox.config): the ones the list uses that change the machine or
  its picture and that this core's Xenia has. The driver hands each to Xenia
  as its line in a configuration file would be (`apply_xenia_setting`), so
  Xenia reads it as it reads a game's own file; a value of the wrong kind is
  refused with the setting's name rather than passed over.
- **SuggestSettings** reads the title id without starting anything
  (driver/title_id.cc): from the disc's default.xex (its execution
  information) found in the disc's file system, or from an Arcade game's
  container header. The four local disc images and an Arcade game read as
  the title ids their lists and folders give. It looks the id up in the
  list, compiled in (driver/optimized_settings.inc, 193 games, made by
  waterbox/import-optimized-settings.py), and answers the values and a note
  with the list's own reasons, as the RPCS3 core does with its wiki.
- **Left out, and the note says why**: vsync and framerate_limit (a frame
  here is always one of the console's sixty), the Direct3D 12 settings and
  use_fuzzy_alpha_epsilon (Direct3D 12 only), the choice of renderer and
  sound output, controller hotkeys, mount_cache, the resolution scale's
  threshold (this core draws at the console's resolution), time_scalar
  (the clock here is the machine's), and two settings this core's Xenia
  does not have.
- **mount_scratch** is one of them although upstream's desktop app, which
  this core does not build, is what mounts the scratch partition: the
  driver defines the setting and mounts the partition in the machine's own
  filesystem. No game that uses it was tried (Fight Night Champion, Lost
  Odyssey).
- Of the 193 games, 103 get values the core can set.

**Readback Resolve, offered but never suggested (user-decided, 2026-10-16).**
The list's most common entries ask the graphics card for something and
write its answer into the console's memory: Readback Resolve (25 games
"fast", 47 "full"; Max Payne 3's picture is black without it) and the real
occlusion queries (31 games). The core pins both off because the machine
would then be one of its graphics card. Asked, the owner chose to offer
them as settings and never set them from the list: a project is
card-independent unless someone turns one on, and the New Project note names
the list's value for the game. The setting's description says a movie then
depends on the card and does not play back with the null renderer.

Two things had to change for "fast" readback to be one machine even on one
card. Upstream reads the copy made the frame before without waiting for the
card, and the bridge fetches a mapping when it is made, so what was read
depended on how far the card had got: each readback buffer now records the
submission that copied into it, and a "fast" read waits for that one (it is
nearly always finished). And the buffers are the device's, gone after a load,
when upstream reads the current copy instead: the state now carries both
buffers of every key with their bytes (ChimeraKeepReadbacks), and they are
made again on the new device (ChimeraPutBackReadbacks), all in patch 0010.
One more thing was found by making the renderer again before every frame:
the buffers swap when a frame opens, and remaking closes the frame and
starts the frame count again, so the frame that was open opened a second
time and swapped them twice - and after two remakings in a row the second
no longer knew the first had left a frame open. The swap after a remaking
is skipped when the frame was open (or was left to be opened again), and
the buffers' ages are counted apart from the frame count. Measured on
Mirror's Edge with llvmpipe: "fast" readback parted from the run without
states at frame 40, then (with the swap fixed) at frame 47, and now not at
all over 120 frames; "full" was right throughout.

**Occlusion Query, offered too (2026-10-16).** "strict" was the same run
twice, and the same in the sandbox, but not with the renderer made again:
Mirror's Edge asks its first query that reaches memory between frames 840
and 900, and at frame 898 a run with a state before every frame (or,
natively, a remaking before every frame) had other answers than one
without. Two things were wrong.

- Setting the context up again starts every count over (ResetZPDState), so
  a query still counting at a load or a remaking lost what it had counted.
  The counts, the reports waiting for their answers and the open report now
  outlive the context (ChimeraTakeZPDState and ChimeraPutZPDStateBack, patches
  0004 and 0010). A state save first waits for every answer, so nothing is
  left half counted on the device that goes away.
- That alone changed nothing, which is how the second one was found: one
  piece of a report counted 532480 samples (1280 by 416) in the run without
  remaking and 0 after it. Upstream resumes a waiting query whenever a
  render pass begins, including the passes in which Xenia copies EDRAM
  contents from one of its own render targets to another and clears them
  after a resolve. Those copies are quads that pass every test, the Xbox 360
  never drew them, and which copies there are depends on where the host
  render targets are - after a load the EDRAM is put back as one big render
  target, and the copies are others. Now only the pass a guest draw enters
  resumes a waiting query (patch 0010).

Ruled out on the way: the host's depth precision (the guest draws use the
24-bit float depth format, kept as 32-bit floats on the host; converting in
the pixel shader made no difference) and pipelines made in the background
(the core makes them on the spot). Measured on Mirror's Edge with llvmpipe,
900 frames: the run with a remaking before every frame now ends with the
console's memory of the run without (ramHash 879f2c73ff33e0c6), and so does
Occlusion Query Full Counters. The gate's card:occlusion holds it, with a
state saved after frame 820 and carried on with a state around every frame.
Readback Memexport is offered the same way. It copies what a draw's
shaders wrote back into the console's memory and waits for the card at
that draw, so nothing of it is left on the device for a load to lose. The
two games of the list that want it (Splinter Cell: Conviction and
Blacklist) were not tried; Mirror's Edge ends 300 frames with the same
memory with it on as off.
