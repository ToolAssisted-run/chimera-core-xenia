#!/bin/sh
# The core's gate, as far as the core goes. Each leg says what it compared.
# It must end with "0 failed", and a SKIP is not a pass.
#
#   waterbox/run-gate.sh
#
# CHIMERA_ROOT=<a Chimera checkout> adds the frontend legs (see them below).
# Needs waterbox/build-native.sh to have run; the sandbox legs need
# waterbox/build-guest.sh, and the instruction tests' leg
# waterbox/build-ppc-tests.sh and waterbox/run-ppc-tests.sh (the reference).
# The game legs need a disc image this repository does not have:
# tests/roms/Mirrors Edge [NTSCU][NTSCJ].iso, or XENIA_TEST_GAME=<an image of
# that game>.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
run="$root/build/vsched/bin/Linux/run-frames"
wbx="$root/build/wbx"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
ok=0; failed=0; skipped=0
report() { # <leg> <PASS|FAIL|SKIP> <what>
	printf '%-22s %-6s %s\n' "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok + 1)) ;; FAIL) failed=$((failed + 1)) ;; *) skipped=$((skipped + 1)) ;; esac
}
# what a run ended with, without the word that says how it got there
last() { tail -1 "$1" 2> /dev/null | cut -d' ' -f2-; }
# Every run below uses the fastest CPU Speed (4000 million instructions a
# second, 250 ps an instruction) unless it says otherwise: at the default
# (500) the game shows nothing for its first three seconds, and these tests
# compare pictures. XENIA_GATE_COST is the cost for one run.
cost_fast=250
native() { # <name> <options...>: a native run, its lines in $work/<name>.out
	name="$1"; shift
	"$run" "$game" "$work/$name-work" --instruction-ps "${XENIA_GATE_COST:-$cost_fast}" "$@" > "$work/$name.out" 2> "$work/$name.err"
}
sandbox() { # <name> <options...>
	name="$1"; shift
	"$wbx/run-wbx" "$wbx/core.wbx" "$game" --instruction-ps "${XENIA_GATE_COST:-$cost_fast}" "$@" > "$work/$name.out" 2> "$work/$name.err"
}

game="${XENIA_TEST_GAME:-$root/tests/roms/Mirrors Edge [NTSCU][NTSCJ].iso}"
frames="${XENIA_GATE_FRAMES:-300}"
half=$((frames / 2))
# a button held for a while in the middle of the run: A on the first pad
# (the twelfth button a pad declares)
press="--press $half:10:800"
have_native=0; have_sandbox=0
[ -x "$run" ] && [ -f "$game" ] && have_native=1
[ -x "$wbx/run-wbx" ] && [ -f "$wbx/core.wbx" ] && [ -f "$game" ] && have_sandbox=1
why_native="no build/vsched/bin/Linux/run-frames: run waterbox/build-native.sh"
why_sandbox="no build/wbx/core.wbx: run waterbox/build-guest.sh"
[ -f "$game" ] || why_native="needs tests/roms/Mirrors Edge [NTSCU][NTSCJ].iso, or XENIA_TEST_GAME=<that image>"
[ -f "$game" ] || why_sandbox="$why_native"

# One thread at a time on the emulator's clock: two runs of a game are one
# run - the same handing over of the baton, the same vertical blanks, the
# same sound, the same 512 MiB, at every sixtieth frame - and a run in which
# an instruction costs something else is another. The second half is the
# control: without it, "the same" could be two runs that both did nothing.
if [ "$have_native" = 0 ]; then
	report "game:repeats" SKIP "$why_native"
else
	native a --frames "$frames" --every 60
	native b --frames "$frames" --every 60
	XENIA_GATE_COST=260 native c --frames "$frames" --every 60
	if [ -s "$work/a.out" ] && cmp -s "$work/a.out" "$work/b.out" && ! cmp -s "$work/a.out" "$work/c.out" &&
		grep -q "^end frame=$frames " "$work/a.out" && ! grep -q "audioPairs=0 " "$work/a.out"; then
		report "game:repeats" PASS "$frames frames, twice: $(last "$work/a.out") - and another run when an instruction costs 260 ps instead of 250"
	else
		report "game:repeats" FAIL "first [$(last "$work/a.out")] second [$(last "$work/b.out")] with another instruction cost [$(last "$work/c.out")] $(tail -1 "$work/a.err")"
	fi
fi

# The same game in the sandbox is the same run: the native build above is the
# reference, made by the same compiler from the same sources, and the guest
# must be where it is at every sixtieth frame. (Not the same compiler, not
# the same run: a build by clang ends with a few bytes of the console's
# memory different after fifteen seconds - docs/PLAN.md.)
if [ "$have_sandbox" = 0 ]; then
	report "game:sandbox" SKIP "$why_sandbox"
elif [ ! -s "$work/a.out" ]; then
	report "game:sandbox" SKIP "no native run to hold it to (game:repeats did not run)"
else
	sandbox w --frames "$frames" --every 60
	if cmp -s "$work/a.out" "$work/w.out"; then
		report "game:sandbox" PASS "$frames frames in the sandbox are the native build's: $(last "$work/w.out")"
	else
		report "game:sandbox" FAIL "native [$(last "$work/a.out")] sandbox [$(last "$work/w.out")] $(tail -1 "$work/w.err")"
	fi
fi

# A pad reaches the machine: a run with a button held is another run than
# one without, the same one natively and in the sandbox, and the program
# asked for the pad in some frames and not in others (which is what the
# frontend's lag counter is made of).
if [ "$have_native" = 0 ]; then
	report "input:pad" SKIP "$why_native"
elif [ "$have_sandbox" = 0 ]; then
	report "input:pad" SKIP "$why_sandbox"
else
	# shellcheck disable=SC2086
	native p --frames "$frames" --every 60 $press
	# shellcheck disable=SC2086
	sandbox q --frames "$frames" --every 60 $press
	asked="$(sed -n 's/^end .* inputFrames=\([0-9]*\) .*/\1/p' "$work/p.out")"
	if [ -s "$work/p.out" ] && cmp -s "$work/p.out" "$work/q.out" && ! cmp -s "$work/p.out" "$work/a.out" &&
		[ "${asked:-0}" -gt 0 ] && [ "${asked:-0}" -lt "$frames" ]; then
		report "input:pad" PASS "A held for ten frames from frame $half makes another run, the same one in the sandbox: $(last "$work/q.out")"
	else
		report "input:pad" FAIL "native [$(last "$work/p.out")] sandbox [$(last "$work/q.out")] no button [$(last "$work/a.out")]"
	fi
fi

# A state is the whole machine. Saved and loaded before every frame, the run
# is the run that was never stopped; and saved by one process half way and
# loaded by another, it ends where that run ends. Both with the button held
# across the save, so that a pad's state is seen to be in a state.
if [ "$have_sandbox" = 0 ]; then
	report "state:every-frame" SKIP "$why_sandbox"
	report "state:other-process" SKIP "$why_sandbox"
elif [ ! -s "$work/q.out" ]; then
	report "state:every-frame" SKIP "no unbroken run to hold it to (input:pad did not run)"
	report "state:other-process" SKIP "no unbroken run to hold it to (input:pad did not run)"
else
	# shellcheck disable=SC2086
	sandbox r --frames "$frames" --every 60 $press --rerecord
	if cmp -s "$work/q.out" "$work/r.out"; then
		report "state:every-frame" PASS "a state saved and loaded before each of $frames frames: $(last "$work/r.out")"
	else
		report "state:every-frame" FAIL "unbroken [$(last "$work/q.out")] with states [$(last "$work/r.out")] $(tail -1 "$work/r.err")"
	fi
	at=$((half + 5))
	# shellcheck disable=SC2086
	sandbox s1 --frames "$frames" $press --save-state "$work/half.state" --at "$at"
	# shellcheck disable=SC2086
	sandbox s2 --frames "$frames" $press --state "$work/half.state"
	if [ -s "$work/s2.out" ] && [ "$(last "$work/s2.out")" = "$(last "$work/q.out")" ]; then
		report "state:other-process" PASS "saved after frame $at ($(sed -n 's/^state: \([0-9]*\) bytes.*/\1/p' "$work/s1.err") bytes), loaded by another process, it ends where the unbroken run ends"
	else
		report "state:other-process" FAIL "unbroken [$(last "$work/q.out")] carried on [$(last "$work/s2.out")] $(tail -1 "$work/s1.err") $(tail -1 "$work/s2.err")"
	fi
fi

# The picture. Xenia's Vulkan renderer on the host's own driver is the
# reference; in the sandbox the same renderer reaches the same driver through
# miniBox's Vulkan bridge, and what it draws must be what the reference
# draws - every sixtieth frame's picture, with the schedule, the sound and
# the memory beside it - with no call refused on the way. Two pictures of
# the run must differ, or "the same" could be two black screens.
# A machine with no Vulkan loader skips this (the runner says so with 5).
if [ "$have_native" = 0 ]; then
	report "picture:bridge" SKIP "$why_native"
elif [ "$have_sandbox" = 0 ]; then
	report "picture:bridge" SKIP "$why_sandbox"
else
	sandbox x --frames "$frames" --every 60 --renderer vulkan
	bridged=$?
	if [ "$bridged" = 5 ]; then
		report "picture:bridge" SKIP "no Vulkan on this machine: $(tail -1 "$work/x.err")"
	else
		native v --frames "$frames" --every 60 --renderer vulkan
		first="$(sed -n '1s/.* videoHash=//p' "$work/x.out")"
		end="$(sed -n '$s/.* videoHash=//p' "$work/x.out")"
		if [ "$bridged" = 0 ] && [ -s "$work/v.out" ] && cmp -s "$work/v.out" "$work/x.out" &&
			grep -q "^end frame=$frames " "$work/x.out" && [ -n "$first" ] && [ "$first" != "$end" ] &&
			grep -q "; 0 calls refused" "$work/x.err"; then
			report "picture:bridge" PASS "$frames frames drawn through the bridge are the frames drawn without it, on $(sed -n 's/^run-wbx: vulkan on \(.*\); 0 calls refused.*/\1/p' "$work/x.err"): $(last "$work/x.out")"
		else
			report "picture:bridge" FAIL "native [$(last "$work/v.out")] sandbox [$(last "$work/x.out")] $(tail -1 "$work/x.err")"
		fi
	fi
fi

# The renderer on another device. A load is a new device (the bridge makes
# it one), and everything the renderer made is gone with the old: it is made
# again, and what only the device held - the console's render targets, what
# it resolved into memory - is put back from the state. Three ways of asking
# whether that is the renderer it was:
#  - natively, with no state at all: made again before every seventh frame,
#    the run is the run in which it never was;
#  - in the sandbox, a state saved and loaded before every frame;
#  - a state saved by one process and carried on by another.
# All three against the native Vulkan run above, pictures included, at every
# twentieth frame of a shorter run (a load with this renderer takes two
# thirds of a second). A run of blank pictures proves nothing and is refused.
short="${XENIA_GATE_SHORT_FRAMES:-120}"
if [ "$have_native" = 0 ] || [ "$have_sandbox" = 0 ] || [ ! -s "$work/v.out" ]; then
	why="$why_native"; [ "$have_native" = 1 ] && why="$why_sandbox"
	[ "$have_native" = 1 ] && [ "$have_sandbox" = 1 ] && why="no Vulkan run to hold it to (picture:bridge did not run)"
	report "picture:remade" SKIP "$why"
	report "picture:every-frame" SKIP "$why"
	report "picture:other-process" SKIP "$why"
else
	native m0 --frames "$short" --every 20 --renderer vulkan
	native m1 --frames "$short" --every 20 --renderer vulkan --remake-every 7
	moved="$(cut -d' ' -f12 "$work/m0.out" | sort -u | wc -l)"
	if [ -s "$work/m0.out" ] && cmp -s "$work/m0.out" "$work/m1.out" && [ "$moved" -gt 2 ]; then
		report "picture:remade" PASS "the renderer made again on a new device before every seventh of $short frames: $(last "$work/m1.out")"
	else
		report "picture:remade" FAIL "never [$(last "$work/m0.out")] every seventh frame [$(last "$work/m1.out")] $(tail -1 "$work/m1.err")"
	fi
	sandbox m2 --frames "$short" --every 20 --renderer vulkan --rerecord
	if [ -s "$work/m0.out" ] && cmp -s "$work/m0.out" "$work/m2.out"; then
		report "picture:every-frame" PASS "a state saved and loaded before each of $short frames, each load a new device: $(last "$work/m2.out")"
	else
		report "picture:every-frame" FAIL "unbroken [$(last "$work/m0.out")] with states [$(last "$work/m2.out")] $(tail -1 "$work/m2.err")"
	fi
	at=$((short / 2 + 3))
	sandbox m3 --frames "$short" --renderer vulkan --save-state "$work/picture.state" --at "$at"
	sandbox m4 --frames "$short" --every 20 --renderer vulkan --state "$work/picture.state"
	if [ -s "$work/m4.out" ] && [ "$(last "$work/m4.out")" = "$(last "$work/m0.out")" ]; then
		report "picture:other-process" PASS "saved after frame $at ($(sed -n 's/^state: \([0-9]*\) bytes.*/\1/p' "$work/m3.err") bytes), loaded by another process on its own device, it ends with the unbroken run's picture"
	else
		report "picture:other-process" FAIL "unbroken [$(last "$work/m0.out")] carried on [$(last "$work/m4.out")] $(tail -1 "$work/m4.err")"
	fi
fi

# The package in the frontend's own engine (chimera-run, from a Chimera
# checkout named by CHIMERA_ROOT that has the package installed by
# waterbox/build-package.sh and an engine with the Vulkan bridge):
#  - a movie recorded there and played back there shows, at a frame, the
#    picture this repository's own runner has at that frame;
#  - the same movie with the engine's own save and load before every frame
#    shows the same picture;
#  - with no graphics card handed over the package refuses to start and says
#    why, and with the renderer that draws nothing it runs.
chimera_root="${CHIMERA_ROOT:-}"
engine="$chimera_root/build/meson-linux/chimera-run"
package="$chimera_root/build/Cores/xenia.chimeraCore"
same="$here/tests/same-picture.py"
if [ -z "$chimera_root" ] || [ ! -x "$engine" ] || [ ! -f "$package" ]; then
	why="set CHIMERA_ROOT to a Chimera checkout with build/meson-linux/chimera-run and the package (waterbox/build-package.sh -r <it>)"
	report "frontend:picture" SKIP "$why"
	report "frontend:states" SKIP "$why"
	report "frontend:no-card" SKIP "$why"
	report "frontend:savedata" SKIP "$why"
	report "frontend:pads" SKIP "$why"
elif [ "$have_sandbox" = 0 ]; then
	report "frontend:picture" SKIP "$why_sandbox"
	report "frontend:states" SKIP "$why_sandbox"
	report "frontend:no-card" SKIP "$why_sandbox"
	report "frontend:savedata" SKIP "$why_sandbox"
	report "frontend:pads" SKIP "$why_sandbox"
else
	# A picture that is blank is refused, whatever it is compared with, so
	# these runs use the fastest CPU Speed too (the game shows its first
	# picture sooner).
	fe_frames=100
	shot=80
	fast='{"cpu_speed":"4000"}'
	: > "$work/empty.movie"
	env -u DISPLAY "$engine" "$package" "$game" "$work/empty.movie" --frames "$fe_frames" --record "$work/fe.movie" --gpu --settings "$fast" > "$work/fe-record.out" 2>&1
	env -u DISPLAY "$engine" "$package" "$game" "$work/fe.movie" --gpu --settings "$fast" --screenshot "$shot=$work/fe.tga" > "$work/fe-play.out" 2>&1
	played=$?
	if grep -q "no Vulkan on this machine" "$work/fe-play.out"; then
		report "frontend:picture" SKIP "no Vulkan on this machine"
		report "frontend:states" SKIP "no Vulkan on this machine"
	else
		# the engine counts frames from 0, the runner from 1
		sandbox fe --frames $((shot + 1)) --renderer vulkan --ppm "$work/fe.ppm"
		verdict="$(python3 "$same" "$work/fe.tga" "$work/fe.ppm" 2>&1)"; same_rc=$?
		if [ "$played" = 0 ] && grep -q "^frames=$fe_frames" "$work/fe-play.out" && grep -q "chimera vk: the bridge was handed over" "$work/fe-play.out" && [ "$same_rc" = 0 ]; then
			report "frontend:picture" PASS "a movie of $fe_frames frames in chimera-run: its picture at frame $shot is this repository's runner's ($verdict)"
		else
			report "frontend:picture" FAIL "chimera-run exit $played; pictures: $verdict; $(tail -1 "$work/fe-play.out")"
		fi
		env -u DISPLAY "$engine" "$package" "$game" "$work/fe.movie" --gpu --settings "$fast" --rerecord --screenshot "$shot=$work/fe-states.tga" > "$work/fe-states.out" 2>&1
		stated=$?
		verdict="$(python3 "$same" "$work/fe-states.tga" "$work/fe.tga" 2>&1)"; same_rc=$?
		if [ "$stated" = 0 ] && grep -q "^frames=$fe_frames" "$work/fe-states.out" && [ "$same_rc" = 0 ]; then
			report "frontend:states" PASS "the same movie with the engine's save and load before every frame, each load a new device: the same picture at frame $shot ($verdict)"
		else
			report "frontend:states" FAIL "chimera-run --rerecord exit $stated; pictures: $verdict; $(tail -1 "$work/fe-states.out")"
		fi
	fi
	env -u DISPLAY "$engine" "$package" "$game" "$work/fe.movie" --settings "$fast" > "$work/fe-nocard.out" 2>&1
	refused=$?
	env -u DISPLAY "$engine" "$package" "$game" "$work/fe.movie" --settings '{"renderer":"null","cpu_speed":"4000"}' --export-savedata "$work/fe-saved" > "$work/fe-null.out" 2>&1
	ran=$?
	if [ "$refused" != 0 ] && grep -q "the Vulkan renderer needs a device" "$work/fe-nocard.out" && [ "$ran" = 0 ] && grep -q "^frames=$fe_frames" "$work/fe-null.out"; then
		report "frontend:no-card" PASS "with no card handed over the package refuses to start and says why (exit $refused); with the renderer that draws nothing it runs its $fe_frames frames"
	else
		report "frontend:no-card" FAIL "no card: exit $refused [$(tail -1 "$work/fe-nocard.out")]; renderer null: exit $ran [$(tail -1 "$work/fe-null.out")]"
	fi
	# What the console keeps comes out through the engine's save data export:
	# the profile that is always signed in is there by name, with its account.
	account="$work/fe-saved/content/B13EBABEBABEBABE/FFFE07D1/00010000/B13EBABEBABEBABE/Account"
	kept="$(find "$work/fe-saved" -type f 2> /dev/null | wc -l)"
	if [ "$ran" = 0 ] && [ -s "$account" ] && grep -q "^savedata=$kept" "$work/fe-null.out"; then
		report "frontend:savedata" PASS "the engine's save data export brings out $kept files of the console's storage, the signed-in profile's account among them ($(wc -c < "$account") bytes)"
	else
		report "frontend:savedata" FAIL "$kept files exported; $(grep '^savedata=' "$work/fe-null.out" | head -1); the profile's account $( [ -s "$account" ] && echo is || echo is not ) there"
	fi
	# The package declares four controllers, and the console says which are
	# plugged in: a movie has columns for those and no others. A row of a
	# recorded movie is "||" and then one group ending in "|" for each
	# controller, so the bars are counted: 3 with one controller, 4 with two.
	bars() { # <how many controllers>: the bars in a recorded row
		env -u DISPLAY "$engine" "$package" "$game" "$work/empty.movie" --frames 3 --record "$work/pads$1.movie" \
			--settings "{\"renderer\":\"null\",\"cpu_speed\":\"4000\",\"pads\":\"$1\"}" > "$work/pads$1.out" 2>&1
		head -1 "$work/pads$1.movie" 2> /dev/null | tr -cd '|' | wc -c
	}
	one="$(bars 1)"; two="$(bars 2)"; four="$(bars 4)"
	if [ "$one" = 3 ] && [ "$two" = 4 ] && [ "$four" = 6 ]; then
		report "frontend:pads" PASS "a recorded movie has one controller's columns with one plugged in, two with two, four with four"
	else
		report "frontend:pads" FAIL "bars in a row: $one with one controller (3 expected), $two with two (4), $four with four (6)"
	fi
fi

# The default CPU Speed is 500 million instructions a second: a run that
# names no speed is the run at 2000 ps an instruction, and is not the run at
# 250. (The runner's own default; the package's default is checked by the
# contract tests against waterbox.config.)
if [ "$have_native" = 0 ]; then
	report "speed:default" SKIP "$why_native"
else
	"$run" "$game" "$work/d0-work" --frames 120 > "$work/d0.out" 2> "$work/d0.err"
	XENIA_GATE_COST=2000 native d1 --frames 120
	native d2 --frames 120
	if [ -s "$work/d0.out" ] && cmp -s "$work/d0.out" "$work/d1.out" && ! cmp -s "$work/d0.out" "$work/d2.out"; then
		report "speed:default" PASS "a run that names no CPU speed is the run at 2000 ps an instruction (500 million a second), and not the run at 250"
	else
		report "speed:default" FAIL "no speed named [$(last "$work/d0.out")] 2000 ps [$(last "$work/d1.out")] 250 ps [$(last "$work/d2.out")]"
	fi
fi

# Upstream's instruction tests in the sandbox: the tests that fail are the
# tests that fail natively, and no others. A part of them - every so-manieth
# file, in the order of their names - because a new recompiler is made for
# every test and the whole set takes the sandbox half an hour
# (XENIA_GATE_PPC_EVERY=1 is the whole set). The reference's failures are cut
# down to the tests that were run by the names in those files' maps. Every
# eighteenth is 32 files and 4,460 tests of which 1,039 fail; a part in which
# nothing fails (every twentieth is one) is refused, because "the same tests
# fail" says nothing of it.
every="${XENIA_GATE_PPC_EVERY:-18}"
# XENIA_PPC_REFERENCE: another list to hold it to (how this leg is seen to fail)
ref="${XENIA_PPC_REFERENCE:-$root/build/ppc-tests/native.failed}"
bins="$root/build/ppc-tests/bin"
if [ ! -x "$wbx/run-ppc-wbx" ] || [ ! -f "$wbx/ppc-tests.wbx" ]; then
	report "ppc:sandbox" SKIP "$why_sandbox"
elif [ ! -s "$ref" ] || [ ! -d "$bins" ]; then
	report "ppc:sandbox" SKIP "no reference: run waterbox/build-ppc-tests.sh and waterbox/run-ppc-tests.sh"
else
	"$wbx/run-ppc-wbx" "$wbx/ppc-tests.wbx" "$root/extern/xenia/src/xenia/cpu/ppc/testing" "$bins" "$every" > "$work/ppc.out" 2> "$work/ppc.log"
	sed -n 's/^mounted //p' "$work/ppc.out" > "$work/ppc.files"
	while read -r stem; do
		sed -n 's/^.* t test_//p' "$bins/$stem.map"
	done < "$work/ppc.files" | sort -u > "$work/ppc.ran"
	sort "$ref" | comm -12 - "$work/ppc.ran" > "$work/ppc.expected"
	sed -n 's/^ *\[\([^]]*\)\] FAILED.*/\1/p' "$work/ppc.log" | sort -u > "$work/ppc.failed"
	total="$(sed -n 's/^Total tests: //p' "$work/ppc.log")"
	ran="$(wc -l < "$work/ppc.ran")"
	nfail="$(wc -l < "$work/ppc.failed")"
	if [ "${total:-0}" -gt 0 ] && [ "$total" -eq "$ran" ] && [ "$nfail" -gt 0 ] && [ "$nfail" -lt "$total" ] && cmp -s "$work/ppc.expected" "$work/ppc.failed"; then
		report "ppc:sandbox" PASS "$(wc -l < "$work/ppc.files") files of instruction tests, $total tests: the $nfail that fail are the reference's"
	else
		report "ppc:sandbox" FAIL "ran ${total:-none} tests of $ran named; $nfail fail, the reference says $(wc -l < "$work/ppc.expected"); first differences: $(diff "$work/ppc.expected" "$work/ppc.failed" | sed -n '2,4p' | tr '\n' ' ')$(tail -1 "$work/ppc.log" | cut -c1-120)"
	fi
fi

# A page the renderer watches, written through any of its addresses. The
# console's 512 MiB are seen at three places in its address space
# (0xA0000000, 0xC0000000 and 0xE0000000), and this core keeps them as ONE
# block, so a write to a watched page arrives without saying which address
# the program used. The check needs no game: the console is switched on
# empty, one page is allocated at each place and watched as a renderer
# watches it, and each is written through all three addresses. Every write
# must land and be told of once, natively and in the sandbox alike. (Before
# the fix a write to a page allocated at 0xC0000000 or 0xE0000000 ended the
# core: issue 249.)
if [ ! -x "$run" ]; then
	report "memory:views" SKIP "no build/vsched/bin/Linux/run-frames: run waterbox/build-native.sh"
elif [ ! -x "$wbx/run-wbx" ] || [ ! -f "$wbx/core.wbx" ]; then
	report "memory:views" SKIP "no build/wbx/core.wbx: run waterbox/build-guest.sh"
else
	timeout 300 "$run" --check-views "$work/views-work" > "$work/views-native.out" 2> "$work/views-native.err"
	timeout 300 "$wbx/run-wbx" "$wbx/core.wbx" --check-views > "$work/views-sandbox.out" 2> "$work/views-sandbox.err"
	writes="$(grep -c ': ok ' "$work/views-sandbox.out")"
	if [ "$(tail -1 "$work/views-native.out")" = "views ok" ] && cmp -s "$work/views-native.out" "$work/views-sandbox.out" && [ "$writes" -eq 9 ]; then
		report "memory:views" PASS "$writes writes to a watched page, through each of its three addresses, land and are told of once - natively and in the sandbox"
	else
		report "memory:views" FAIL "native [$(tail -1 "$work/views-native.out")] sandbox [$(tail -1 "$work/views-sandbox.out")]"
	fi
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
