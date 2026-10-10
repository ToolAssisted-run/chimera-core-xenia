# Xenia core - plan and design log

## What this is

A Chimera core for the Xbox 360, built on Xenia Canary
(xenia-canary/xenia-canary, pinned at `extern/xenia`). Requested in chimera
issue 171. The owner, 2026-10-10: "let's give it a fair try. Developing a
Vulkan bridge for Chimera is something I wanted us to do anyway."

"A fair try" is taken to mean what it says: this may not work, and where it
does not this file will say so.

Where it stands (2026-10-10): the owner's one game runs in the sandbox, a
frame at a time, to the same memory, sound and schedule as the native build,
draws through miniBox's Vulkan bridge the pictures the native build draws,
and loads a state with that renderer - on a new device, in another process -
to the picture it would have had. All of it on a software Vulkan (llvmpipe)
and on one game's first seconds. It is not yet a package the frontend can
open, and it has not met a real card. This file is the survey, the route,
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

Not dealt with: one thing the views gave that a block cannot. Upstream
protects a page in the guest's views to notice a write (the GPU's caches do
this) while reaching the same page itself, unprotected, through the
physical view. With one block the emulator's own access meets its own
protection. The null renderer sets no such watch, so nothing has met it
yet; the Vulkan renderer will.

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
  what `GetVideoBgra` hands the frontend - 1280x720 here. `SetRenderingEnabled`
  off skips the copy and nothing else.
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

**Not established.** A real card: llvmpipe has no fragment shader interlock,
so only the path with host render targets has run, and the other path's
put-back (a plain buffer copy) has never executed. Resolution above the
console's: upstream's dump does not exist there and the fetch says so; the
core draws at 1x. Speed: a save fetches 10 MiB and the resolved ranges from
the device every time. A second game. And host depth: a depth buffer kept
on the host at more than the console's 24 bits goes through the console's
format on its way into a state, which upstream's own snapshot does too.

## The gate (2026-10-10)

`waterbox/run-gate.sh`, 10 legs, six minutes: two native runs are one run
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
- one failing test taken out of the reference (`XENIA_PPC_REFERENCE`): "1039
  fail, the reference says 1038", and the test is named;
- a part of the instruction tests in which nothing fails is refused, which
  the first choice of part (every twentieth file) was.

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
- **The repository.** Local only. A public repository is the owner's to ask
  for.
