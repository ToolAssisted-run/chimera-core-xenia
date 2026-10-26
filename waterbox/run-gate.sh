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
# that game>. One test needs a game that draws at 1920x1080:
# XENIA_TEST_GAME_FULL_HD (see frontend:full-hd).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
run="$root/build/vsched/bin/Linux/run-frames"
wbx="$root/build/wbx"
work="$(mktemp -d)"
# A game runs only in a project (Chimera, 2026-10-26): crun_game <package>
# <game> <movie> [chimera-run's options] makes one of the game and the movie,
# with any --settings and --firmware given, and runs it with the rest.
crun_quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
crun_game() {
	cg_pkg="$1" cg_game="$2" cg_movie="$3" cg_made=""
	shift 3
	cg_n=$#
	while [ "$cg_n" -gt 0 ]; do
		case "$1" in
			--settings) cg_made="$cg_made --settings $(crun_quote "$2")"; shift 2; cg_n=$((cg_n - 2)); continue ;;
			--settings=*) cg_made="$cg_made --settings $(crun_quote "${1#--settings=}")"; shift; cg_n=$((cg_n - 1)); continue ;;
			--firmware) cg_made="$cg_made --firmware $(crun_quote "$2")"; set -- "$@" "$1" "$2"; shift 2; cg_n=$((cg_n - 2)); continue ;;
			--firmware=*) cg_made="$cg_made --firmware $(crun_quote "${1#--firmware=}")"; set -- "$@" "$1"; shift; cg_n=$((cg_n - 1)); continue ;;
		esac
		set -- "$@" "$1"; shift; cg_n=$((cg_n - 1))
	done
	cg_dir="$(mktemp -d "$work/project.XXXXXX")"
	eval "env -u DISPLAY \"\$engine\" --new-project \"\$cg_dir/game.chimeraProject\" \"\$cg_pkg\" \"\$cg_game\" \"\$cg_movie\" $cg_made" \
		> "$cg_dir/make.log" 2>&1 || { echo "chimera-run: no project made: $(tail -1 "$cg_dir/make.log")" >&2; return 2; }
	env -u DISPLAY "$engine" --project "$cg_dir/game.chimeraProject" "$cg_pkg" --files "$(dirname "$cg_game")" "$@"
}
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

# Readback Resolve (chimera#289). A project may turn it on, and the console's
# memory then holds what the graphics card drew: the machine is one of its
# card. On one card it must still be ONE machine. Upstream's "fast" readback
# reads the copy made a frame before without waiting for the card, and a
# buffer made on the device a load replaced is gone: here it waits for that
# copy, and a state carries both buffers. With "fast": two native runs are
# one, a state saved and loaded before every frame in the sandbox changes
# nothing, and the run is another than without the setting.
# (`card`, not `name`: native and sandbox set a `name` of their own.)
if [ "$have_native" = 0 ] || [ "$have_sandbox" = 0 ] || [ ! -s "$work/m0.out" ]; then
	report "card:readback" SKIP "no Vulkan run to hold it to (picture:remade did not run)"
else
	for leg in "readback readback_resolve=fast"; do
		card="${leg%% *}"; set_="${leg#* }"
		native "$card-a" --frames "$short" --every 20 --renderer vulkan --set "$set_"
		native "$card-b" --frames "$short" --every 20 --renderer vulkan --set "$set_"
		sandbox "$card-r" --frames "$short" --every 20 --renderer vulkan --set "$set_" --rerecord
		# what differs from the run without the setting: the console's memory
		# for a readback; Mirror's Edge asks no occlusion query that reaches
		# memory in these frames, and only the GPU's registers move then
		moved=""
		for d in ramHash busHash videoHash; do
			off="$(sed -n "\$s/.* $d=\([0-9a-f]*\).*/\1/p" "$work/m0.out")"
			on="$(sed -n "\$s/.* $d=\([0-9a-f]*\).*/\1/p" "$work/$card-a.out")"
			[ -n "$on" ] && [ "$on" != "$off" ] && moved="$moved $d"
		done
		if [ -s "$work/$card-a.out" ] && cmp -s "$work/$card-a.out" "$work/$card-b.out" &&
			cmp -s "$work/$card-a.out" "$work/$card-r.out" && [ -n "$moved" ]; then
			report "card:$card" PASS "$set_: two runs are one, a state around every frame changes nothing, and the run is another than without it (differs in:$moved)"
		else
			report "card:$card" FAIL "$set_: twice [$(last "$work/$card-a.out")] [$(last "$work/$card-b.out")] with states [$(last "$work/$card-r.out")] without [$(last "$work/m0.out")] $(tail -1 "$work/$card-r.err")"
		fi
	done
fi

# Occlusion Query (chimera#289). With "strict" the console asks the graphics
# card how many samples of some draws passed the depth test, and the answer
# lands in its memory; Mirror's Edge first asks in a way that changes its
# memory between frames 840 and 900. A load makes the renderer again on a
# new device, and the answers must be those of a run that never loaded: what
# was counted before the load is kept, and the copies Xenia makes between its
# own render targets are not counted (the console never drew them, and after
# a load they are other copies - a 1280x416 copy was counted once). The run
# with the renderer made again before every frame is the run without; a
# state saved after frame 820 and carried on with a state saved and loaded
# before every frame ends where they end; and the run is another than with
# the default "fake".
oq=900; oq_at=820
if [ "$have_native" = 0 ] || [ "$have_sandbox" = 0 ] || [ ! -s "$work/m0.out" ]; then
	report "card:occlusion" SKIP "no Vulkan run to hold it to (picture:remade did not run)"
else
	strict="occlusion_query=strict"
	native oq-a --frames "$oq" --every 60 --renderer vulkan --set "$strict"
	native oq-m --frames "$oq" --every 60 --renderer vulkan --set "$strict" --remake-every 1
	native oq-f --frames "$oq" --every 60 --renderer vulkan
	sandbox oq-s --frames "$oq" --renderer vulkan --set "$strict" --save-state "$work/oq.state" --at "$oq_at"
	sandbox oq-r --frames "$oq" --every 60 --renderer vulkan --set "$strict" --state "$work/oq.state" --rerecord
	strict_ram="$(sed -n '$s/.* ramHash=\([0-9a-f]*\).*/\1/p' "$work/oq-a.out")"
	fake_ram="$(sed -n '$s/.* ramHash=\([0-9a-f]*\).*/\1/p' "$work/oq-f.out")"
	if [ -s "$work/oq-a.out" ] && grep -q "^end frame=$oq " "$work/oq-a.out" && cmp -s "$work/oq-a.out" "$work/oq-m.out" &&
		[ "$(last "$work/oq-r.out")" = "$(last "$work/oq-a.out")" ] && [ -n "$strict_ram" ] && [ -n "$fake_ram" ] && [ "$strict_ram" != "$fake_ram" ]; then
		report "card:occlusion" PASS "$strict: the renderer made again before each of $oq frames changes nothing, nor states around every frame after frame $oq_at, and the console's memory is another than with fake answers: $(last "$work/oq-a.out")"
	else
		report "card:occlusion" FAIL "$strict: unbroken [$(last "$work/oq-a.out")] made again [$(last "$work/oq-m.out")] with states [$(last "$work/oq-r.out")] fake [$(last "$work/oq-f.out")] $(tail -1 "$work/oq-r.err")"
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
	crun_game "$package" "$game" "$work/empty.movie" --frames "$fe_frames" --record "$work/fe.movie" --gpu --settings "$fast" > "$work/fe-record.out" 2>&1
	crun_game "$package" "$game" "$work/fe.movie" --gpu --settings "$fast" --screenshot "$shot=$work/fe.tga" > "$work/fe-play.out" 2>&1
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
		crun_game "$package" "$game" "$work/fe.movie" --gpu --settings "$fast" --rerecord --screenshot "$shot=$work/fe-states.tga" > "$work/fe-states.out" 2>&1
		stated=$?
		verdict="$(python3 "$same" "$work/fe-states.tga" "$work/fe.tga" 2>&1)"; same_rc=$?
		if [ "$stated" = 0 ] && grep -q "^frames=$fe_frames" "$work/fe-states.out" && [ "$same_rc" = 0 ]; then
			report "frontend:states" PASS "the same movie with the engine's save and load before every frame, each load a new device: the same picture at frame $shot ($verdict)"
		else
			report "frontend:states" FAIL "chimera-run --rerecord exit $stated; pictures: $verdict; $(tail -1 "$work/fe-states.out")"
		fi
	fi
	crun_game "$package" "$game" "$work/fe.movie" --settings "$fast" > "$work/fe-nocard.out" 2>&1
	refused=$?
	crun_game "$package" "$game" "$work/fe.movie" --settings '{"renderer":"null","cpu_speed":"4000"}' --export-savedata "$work/fe-saved" > "$work/fe-null.out" 2>&1
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
		crun_game "$package" "$game" "$work/empty.movie" --frames 3 --record "$work/pads$1.movie" \
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

# The RUNNER's own default speed is 500 million instructions a second: a run
# that names no speed is the run at 2000 ps an instruction, and is not the run
# at 250. The PACKAGE's default is another thing - 2000 million since
# 2026-10-14 (chimera issue 266), what a new project gets - and is checked by
# the contract tests against waterbox.config.
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
# core: issue 249.) And memory asked for aligned to 8 and 64 KiB is given in
# each of the three places, both ways a game asks for it, with its physical
# address aligned (issue 272: none was given at 0xE0000000, and Marvel vs.
# Capcom 3 wrote to the 0 it got back).
if [ ! -x "$run" ]; then
	report "memory:views" SKIP "no build/vsched/bin/Linux/run-frames: run waterbox/build-native.sh"
elif [ ! -x "$wbx/run-wbx" ] || [ ! -f "$wbx/core.wbx" ]; then
	report "memory:views" SKIP "no build/wbx/core.wbx: run waterbox/build-guest.sh"
else
	timeout 300 "$run" --check-views "$work/views-work" > "$work/views-native.out" 2> "$work/views-native.err"
	timeout 300 "$wbx/run-wbx" "$wbx/core.wbx" --check-views > "$work/views-sandbox.out" 2> "$work/views-sandbox.err"
	writes="$(grep -c 'written at .*: ok ' "$work/views-sandbox.out")"
	aligned="$(grep -c '^aligned to .*: ok ' "$work/views-sandbox.out")"
	if [ "$(tail -1 "$work/views-native.out")" = "views ok" ] && cmp -s "$work/views-native.out" "$work/views-sandbox.out" && [ "$writes" -eq 9 ] && [ "$aligned" -eq 6 ]; then
		report "memory:views" PASS "$writes writes to a watched page, through each of its three addresses, land and are told of once; $aligned aligned allocations are given - natively and in the sandbox"
	else
		report "memory:views" FAIL "native [$(tail -1 "$work/views-native.out")] sandbox [$(tail -1 "$work/views-sandbox.out")]"
	fi
fi

# A thread that waits by reading the clock. A program may wait for a moment
# to come by asking what time it is in a loop, and a thread runs here until
# its turn - a slice of instructions - is spent. Asking the time used to give
# it a whole slice again, so such a thread kept its turn for ever: the
# machine's time ran away under it, no other thread ran and no frame ended
# (issue 258: The Beatles: Rock Band stopped at frame 20). The check needs
# no game: one thread reads the clock in a loop, paying for instructions as
# recompiled code does, while another sleeps five milliseconds of the
# machine's time. The sleeper must wake when its time has come, natively
# and in the sandbox alike.
if [ ! -x "$run" ]; then
	report "clock:turn" SKIP "no build/vsched/bin/Linux/run-frames: run waterbox/build-native.sh"
elif [ ! -x "$wbx/run-wbx" ] || [ ! -f "$wbx/core.wbx" ]; then
	report "clock:turn" SKIP "no build/wbx/core.wbx: run waterbox/build-guest.sh"
else
	timeout 300 "$run" --check-turns "$work/turns-work" > "$work/turns-native.out" 2> "$work/turns-native.err"
	timeout 300 "$wbx/run-wbx" "$wbx/core.wbx" --check-turns > "$work/turns-sandbox.out" 2> "$work/turns-sandbox.err"
	if [ "$(tail -1 "$work/turns-native.out")" = "turns ok" ] && cmp -s "$work/turns-native.out" "$work/turns-sandbox.out"; then
		report "clock:turn" PASS "a thread that reads the clock in a loop gives way when its turn is spent - natively and in the sandbox: $(head -1 "$work/turns-sandbox.out")"
	else
		report "clock:turn" FAIL "native [$(head -1 "$work/turns-native.out")] sandbox [$(head -1 "$work/turns-sandbox.out")]"
	fi
fi

# No randomness from the host. In the sandbox the standard library's random
# device is the processor's own instruction for random numbers, which nothing
# can stand in front of: a program that asked the console for random bytes
# got other bytes in every run (The Beatles: Rock Band asks seven times before
# its title screen, and two runs of one movie left different memory). The
# console's random numbers are a fixed sequence of the machine's own now. The
# check: no object of the sandbox build refers to the random device.
if [ ! -d "$root/build/guest" ]; then
	report "random:own" SKIP "no build/guest: run waterbox/build-guest.sh"
else
	users="$(find "$root/build/guest" -name '*.a' | sort | while read -r a; do
		nm -CA "$a" 2> /dev/null | grep ' U std::random_device::' | cut -d: -f2
	done | sort -u | tr '\n' ' ')"
	if [ -z "$users" ]; then
		report "random:own" PASS "no object of the sandbox build asks the host for random numbers"
	else
		report "random:own" FAIL "these ask the host's random device: $users"
	fi
fi

# No waiting on the host's network. The machine has no network: the sandbox
# refuses to make a socket, so the console's sockets are never made, and a
# wait for one to be ready is a wait on the machine's clock. Xenia asked the
# host's select for it, which the sandbox does not provide, and the machine
# stopped (chimera issue 269: Dead Rising 2: Case Zero at its first menu).
# The check: no object of the sandbox build calls select or poll.
if [ ! -d "$root/build/guest" ]; then
	report "net:own" SKIP "no build/guest: run waterbox/build-guest.sh"
else
	users="$(find "$root/build/guest" -name '*.a' | sort | while read -r a; do
		nm -A "$a" 2> /dev/null | grep -E ' U (select|pselect|poll|ppoll|epoll_wait)$' | cut -d: -f2
	done | sort -u | tr '\n' ' ')"
	if [ -z "$users" ]; then
		report "net:own" PASS "no object of the sandbox build waits on the host's network (select, poll)"
	else
		report "net:own" FAIL "these wait on the host's network: $users"
	fi
fi

# A game that is a container: one file with no extension, as Xbox Live
# Arcade games are (issue 250). This repository has no such game, so one is
# made from the disc image: tests/make-container.py puts the image's own
# program, default.xex, in a container of the format downloaded games have.
# The file is opened, mounted, and the program loaded from it and started,
# through the code an Arcade game goes through. Mirror's Edge with nothing
# but its program asks the console to end it at once, and the machine runs
# on with a blank picture - so what is compared is that the sandbox does
# exactly what the native build does, that the program ran (the console's
# memory changes between frames 30 and 60), and that the program was heard
# asking to be ended. (Before the change the sandbox refused a container:
# "the program did not launch (C000000F)" - it could not map the file.)
container="$work/container"
if [ "$have_native" = 0 ]; then
	report "container:launch" SKIP "$why_native"
elif [ "$have_sandbox" = 0 ]; then
	report "container:launch" SKIP "$why_sandbox"
elif ! python3 "$here/tests/make-container.py" "$game" "$container" > "$work/container.out" 2>&1; then
	report "container:launch" FAIL "the test container could not be made: $(tail -1 "$work/container.out")"
else
	"$run" "$container" "$work/cn-work" --instruction-ps "$cost_fast" --frames 120 --every 30 > "$work/cn.out" 2> "$work/cn.err"
	"$wbx/run-wbx" "$wbx/core.wbx" "$container" --instruction-ps "$cost_fast" --frames 120 --every 30 > "$work/cw.out" 2> "$work/cw.err"
	ram30="$(sed -n 's/^at frame=30 .* ramHash=\([0-9a-f]*\) .*/\1/p' "$work/cw.out")"
	ram60="$(sed -n 's/^at frame=60 .* ramHash=\([0-9a-f]*\) .*/\1/p' "$work/cw.out")"
	if [ -s "$work/cn.out" ] && cmp -s "$work/cn.out" "$work/cw.out" && grep -q "^end frame=120 " "$work/cw.out" &&
		[ -n "$ram30" ] && [ "$ram30" != "$ram60" ] && grep -q "the program asked the console to end it" "$work/cw.err"; then
		report "container:launch" PASS "a container made of the disc's own program ($(sed -n 's/.*: \(.*\)/\1/p' "$work/container.out")) is mounted and its program run, the same natively and in the sandbox: $(last "$work/cw.out")"
	else
		report "container:launch" FAIL "native [$(last "$work/cn.out")] sandbox [$(last "$work/cw.out")] $(tail -1 "$work/cw.err")"
	fi
fi
# And the package takes it from the frontend's engine: a file with no
# extension, named on the command line as a game is.
if [ -z "$chimera_root" ] || [ ! -x "$engine" ] || [ ! -f "$package" ]; then
	report "frontend:container" SKIP "set CHIMERA_ROOT to a Chimera checkout with build/meson-linux/chimera-run and the package (waterbox/build-package.sh -r <it>)"
elif [ ! -s "$container" ]; then
	report "frontend:container" SKIP "no test container (container:launch did not make one)"
else
	: > "$work/empty.movie"
	crun_game "$package" "$container" "$work/empty.movie" --frames 60 --record "$work/fc.movie" --settings '{"renderer":"null","cpu_speed":"4000"}' > "$work/fc.out" 2>&1
	fc=$?
	crun_game "$package" "$container" "$work/empty.movie" --frames 60 --record "$work/fc-trial.movie" --settings '{"renderer":"null","cpu_speed":"4000","arcade_license":"trial"}' > "$work/fc-trial.out" 2>&1
	fct=$?
	crun_game "$package" "$container" "$work/empty.movie" --frames 60 --record "$work/fc-bad.movie" --settings '{"renderer":"null","cpu_speed":"4000","arcade_license":"neither"}' > "$work/fc-bad.out" 2>&1
	fcb=$?
	if [ "$fc" = 0 ] && grep -q "^frames=60" "$work/fc.out" && [ "$fct" = 0 ] && grep -q "^frames=60" "$work/fc-trial.out" &&
		[ "$fcb" != 0 ] && grep -q "no such Arcade Games setting" "$work/fc-bad.out"; then
		report "frontend:container" PASS "the package in chimera-run starts the container and runs its 60 frames, with Arcade Games set to full and to trial; another value is refused (exit $fcb)"
	else
		report "frontend:container" FAIL "full: exit $fc [$(tail -1 "$work/fc.out")]; trial: exit $fct [$(tail -1 "$work/fc-trial.out")]; a wrong value: exit $fcb [$(tail -1 "$work/fc-bad.out")]"
	fi
fi
# And a PROJECT takes it, which is what a person makes in the New Project
# window. The engine checks a project's file against the formats its slot
# declares, and a game named on chimera-run's command line (the check above)
# never goes through that. The slot used to declare .iso and nothing else, so
# an Arcade game - a file with no extension - was refused when the project was
# made: "file '...' is not a format slot 'game' accepts" (chimera issue 250).
# The slot now declares the empty format too, which is a file with no
# extension.
if [ -z "$chimera_root" ] || [ ! -x "$engine" ] || [ ! -f "$package" ]; then
	report "project:container" SKIP "set CHIMERA_ROOT to a Chimera checkout with build/meson-linux/chimera-run and the package (waterbox/build-package.sh -r <it>)"
elif [ ! -s "$container" ]; then
	report "project:container" SKIP "no test container (container:launch did not make one)"
else
	mkdir -p "$work/arcade"
	arcade="$work/arcade/834312072F4985F9D33D0B9549FFAA32C505FDAD58"
	cp "$container" "$arcade"
	python3 "$here/tests/make-project.py" "$package" "$work/arcade.chimeraProject" "$arcade" --set=renderer=null --set=cpu_speed=4000 > "$work/pc-make.out" 2>&1
	env -u DISPLAY "$engine" --project "$work/arcade.chimeraProject" "$package" --files "$work/arcade" --frames 60 --record "$work/pc.movie" > "$work/pc.out" 2>&1
	pc=$?
	if [ "$pc" = 0 ] && grep -q "^frames=60" "$work/pc.out"; then
		report "project:container" PASS "a project whose game is a file with no extension is opened by chimera-run and runs its 60 frames"
	else
		report "project:container" FAIL "exit $pc [$(tail -1 "$work/pc.out")] $(tail -1 "$work/pc-make.out")"
	fi
fi

# Per-game settings (chimera issue 289). Before a project exists, the New
# Project window asks the core what it suggests for the game (chimera-run
# --project ... --suggest calls the same export, of a project made of the
# game, as a game runs only in a project): the game is found by its title id, read
# from the disc's default.xex or from a container's header, in Xenia
# Manager's list of optimized settings, which the core carries. Mirror's
# Edge (45410850) is in the list. The container made above holds the same
# program, so it must be found as the same game.
if [ -z "$chimera_root" ] || [ ! -x "$engine" ] || [ ! -f "$package" ]; then
	report "frontend:suggest" SKIP "set CHIMERA_ROOT to a Chimera checkout with build/meson-linux/chimera-run and the package (waterbox/build-package.sh -r <it>)"
elif [ ! -f "$game" ]; then
	report "frontend:suggest" SKIP "$why_native"
else
	sgdir="$(mktemp -d "$work/project.XXXXXX")"
	"$engine" --new-project "$sgdir/disc.chimeraProject" "$package" "$game" > "$sgdir/disc.make.log" 2>&1 \
		&& "$engine" --project "$sgdir/disc.chimeraProject" "$package" --files "$(dirname "$game")" --suggest > "$work/sg-disc.out" 2>&1
	sgd=$?
	sgc=1
	[ -s "$container" ] && { "$engine" --new-project "$sgdir/container.chimeraProject" "$package" "$container" > "$sgdir/container.make.log" 2>&1 \
		&& "$engine" --project "$sgdir/container.chimeraProject" "$package" --files "$(dirname "$container")" --suggest > "$work/sg-cont.out" 2>&1; sgc=$?; }
	if [ "$sgd" = 0 ] && grep -q '"title_id":"45410850"' "$work/sg-disc.out" && grep -q "optimized settings for Mirror's Edge" "$work/sg-disc.out" &&
		[ "$sgc" = 0 ] && grep -q '"title_id":"45410850"' "$work/sg-cont.out"; then
		report "frontend:suggest" PASS "the disc and the container made from it are both found as 45410850 in the list: $(sed -n 's/.*"note":"\([^"]*\)".*/\1/p' "$work/sg-disc.out" | cut -c1-100)..."
	else
		report "frontend:suggest" FAIL "disc: exit $sgd [$(tail -1 "$work/sg-disc.out")]; container: exit $sgc [$(tail -1 "$work/sg-cont.out" 2> /dev/null)]"
	fi
fi
# And a setting of Xenia's that a project sets reaches Xenia. Fill New Memory
# (scribble_heap) fills what the program is given before it runs, so the
# console's memory differs from the first frames: in the sandbox, a run with
# it ends with another digest of the memory than a run without, which the
# runs above showed is the same every time. Through the frontend's engine, a
# value of the wrong kind is refused with the setting's name, and the
# scratch partition, which this core mounts itself, does not stop the run.
if [ "$have_sandbox" = 0 ] || [ ! -s "$container" ]; then
	report "xenia-settings" SKIP "${why_sandbox} (or no test container)"
else
	"$wbx/run-wbx" "$wbx/core.wbx" "$container" --instruction-ps "$cost_fast" --frames 30 > "$work/xs-a.out" 2> "$work/xs-a.err"
	"$wbx/run-wbx" "$wbx/core.wbx" "$container" --instruction-ps "$cost_fast" --frames 30 --set scribble_heap=true --set scribble_heap_value=90 > "$work/xs-f.out" 2> "$work/xs-f.err"
	ram_a="$(sed -n 's/^end .*ramHash=\([0-9a-f]*\) .*/\1/p' "$work/xs-a.out")"
	ram_f="$(sed -n 's/^end .*ramHash=\([0-9a-f]*\) .*/\1/p' "$work/xs-f.out")"
	if [ -n "$ram_a" ] && [ -n "$ram_f" ] && [ "$ram_a" != "$ram_f" ]; then
		report "xenia-settings" PASS "with Fill New Memory the console's memory after 30 frames is another (ramHash $ram_a without, $ram_f with)"
	else
		report "xenia-settings" FAIL "ramHash without [$ram_a], with [$ram_f]: $(tail -1 "$work/xs-f.err")"
	fi
fi
if [ -z "$chimera_root" ] || [ ! -x "$engine" ] || [ ! -f "$package" ]; then
	report "frontend:xenia-settings" SKIP "set CHIMERA_ROOT to a Chimera checkout with build/meson-linux/chimera-run and the package (waterbox/build-package.sh -r <it>)"
elif [ ! -s "$container" ]; then
	report "frontend:xenia-settings" SKIP "no test container (container:launch did not make one)"
else
	: > "$work/empty.movie"
	xs() { # <name> <settings>
		crun_game "$package" "$container" "$work/empty.movie" --frames 30 --record "$work/$1.movie" --settings "$2" > "$work/$1.out" 2>&1
	}
	xs xs-w '{"renderer":"null","cpu_speed":"4000","scribble_heap":"often"}'; xw=$?
	xs xs-s '{"renderer":"null","cpu_speed":"4000","mount_scratch":true}'; xsc=$?
	if [ "$xw" != 0 ] && grep -q "scribble_heap is true or false" "$work/xs-w.out" && [ "$xsc" = 0 ] && grep -q "^frames=30" "$work/xs-s.out"; then
		report "frontend:xenia-settings" PASS "a value of the wrong kind is refused (exit $xw); with the scratch partition mounted the run goes on"
	else
		report "frontend:xenia-settings" FAIL "wrong kind: exit $xw [$(tail -1 "$work/xs-w.out")]; scratch: exit $xsc [$(tail -1 "$work/xs-s.out")]"
	fi
fi

# Packages put in the console's storage before the game starts, as Xenia's
# Install Content puts them (chimera issue 271): add-on content, title
# updates, saved games. This repository has no package, so one is made from
# the disc image: tests/make-container.py --kind 00000002 writes the disc's
# program into a package that says it is the disc game's add-on content. The
# core puts it in and then asks the console to list that title's add-on
# content, as a game does, and says so when it is there; natively and in the
# sandbox alike. A package of a kind the slot does not take (the Arcade game
# container made above) is refused by name.
if [ "$have_native" = 0 ]; then
	report "content:install" SKIP "$why_native"
elif [ "$have_sandbox" = 0 ]; then
	report "content:install" SKIP "$why_sandbox"
elif [ ! -s "$container" ]; then
	report "content:install" SKIP "no test container (container:launch did not make one)"
elif ! python3 "$here/tests/make-container.py" --kind 00000002 "$game" "$work/addon" > "$work/addon.out" 2>&1; then
	report "content:install" FAIL "the test package could not be made: $(tail -1 "$work/addon.out")"
else
	"$run" "$game" "$work/ad-work" --instruction-ps "$cost_fast" --frames 10 --log --content "$work/addon" > "$work/ad-native.out" 2>&1
	"$wbx/run-wbx" "$wbx/core.wbx" "$game" --instruction-ps "$cost_fast" --frames 10 --log --content "$work/addon" > "$work/ad-sandbox.out" 2>&1
	"$run" "$game" "$work/aw-work" --instruction-ps "$cost_fast" --frames 10 --content "$container" > "$work/ad-wrong.out" 2>&1
	aw=$?
	said="addon is in the console's storage: title $(sed -n 's/.*title \([0-9A-F]*\),.*/\1/p' "$work/addon.out"), kind 00000002"
	if grep -q "$said" "$work/ad-native.out" && grep -q "$said" "$work/ad-sandbox.out" &&
		grep -q "^end frame=10 " "$work/ad-native.out" && grep -q "^end frame=10 " "$work/ad-sandbox.out" &&
		[ "$aw" != 0 ] && grep -q "which the Content slot does not take" "$work/ad-wrong.out"; then
		report "content:install" PASS "add-on content made from the disc is put in the console's storage and listed where a game asks for it ($said), natively and in the sandbox; a package of another kind is refused"
	else
		report "content:install" FAIL "native [$(grep -o 'chimera: .*' "$work/ad-native.out" | tail -1)] sandbox [$(grep -o 'chimera: .*' "$work/ad-sandbox.out" | tail -1)] wrong kind: exit $aw [$(tail -1 "$work/ad-wrong.out")]"
	fi
fi
# A game on several discs (chimera issue 281): the project holds every disc
# in order, and when the game asks for disc N (XamSwapDisc) the core puts
# that disc in where standalone Xenia asks a person to pick a file. With no
# game on two discs here, the test disc is given twice and the runners put
# disc 2 in after frame 10 the way the game's request does; natively and in
# the sandbox the run carries on and both say it is in. A disc the project
# does not have is not put in.
if [ "$have_native" = 0 ]; then
	report "disc:insert" SKIP "$why_native"
elif [ "$have_sandbox" = 0 ]; then
	report "disc:insert" SKIP "$why_sandbox"
else
	mkdir -p "$work/discs"
	ln -sf "$game" "$work/discs/second.iso"
	"$run" "$game" "$work/di-work" --instruction-ps "$cost_fast" --frames 30 --every 10 --disc "$work/discs/second.iso" --insert-disc 10:2 > "$work/di-native.out" 2>&1
	"$wbx/run-wbx" "$wbx/core.wbx" "$game" --instruction-ps "$cost_fast" --frames 30 --every 10 --disc "$work/discs/second.iso" --insert-disc 10:2 > "$work/di-sandbox.out" 2>&1
	"$run" "$game" "$work/dx-work" --instruction-ps "$cost_fast" --frames 15 --disc "$work/discs/second.iso" --insert-disc 10:3 > "$work/dx-native.out" 2>&1
	grep -E '^(at|end) frame|^disc ' "$work/di-native.out" > "$work/di-native.lines"
	grep -E '^(at|end) frame|^disc ' "$work/di-sandbox.out" > "$work/di-sandbox.lines"
	if grep -q "^disc 2 after frame 10: in" "$work/di-native.lines" && grep -q "^end frame=30 " "$work/di-native.lines" &&
		cmp -s "$work/di-native.lines" "$work/di-sandbox.lines" && grep -q "^disc 3 after frame 10: not put in" "$work/dx-native.out"; then
		report "disc:insert" PASS "the project's disc 2 is put in after frame 10 as the game's request does, and the run carries on, the same natively and in the sandbox; a disc 3 the project does not have is not put in"
	else
		report "disc:insert" FAIL "native [$(grep '^disc' "$work/di-native.out")] sandbox [$(grep '^disc' "$work/di-sandbox.out")] disc 3 [$(grep '^disc' "$work/dx-native.out")] $(tail -1 "$work/di-sandbox.out")"
	fi
fi
# And the Save data slot through the frontend's engine: a zip as Emulator >
# Export Save Data writes it goes back into the storage before the game
# starts, and the next export has it, byte for byte. A zip holding anything
# not under content/ is refused by name. The project's add-on content is not
# save data and is not in the export.
if [ -z "$chimera_root" ] || [ ! -x "$engine" ] || [ ! -f "$package" ]; then
	report "frontend:savedata-in" SKIP "set CHIMERA_ROOT to a Chimera checkout with build/meson-linux/chimera-run and the package (waterbox/build-package.sh -r <it>)"
elif [ ! -s "$work/addon" ]; then
	report "frontend:savedata-in" SKIP "no test package (content:install did not make one)"
else
	mkdir -p "$work/sd-in" "$work/sd-bad"
	python3 - "$work/sd-in/saved.zip" "$work/sd-bad/saved.zip" <<'SAVEZIP'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("content/B13EBABEBABEBABE/45410850/00000001/chimera-test/progress.bin", bytes(range(256)) * 9)
    z.writestr("content/0000000000000000/45410850/00000001/stored.txt", b"kept as it was\n", compress_type=zipfile.ZIP_STORED)
with zipfile.ZipFile(sys.argv[2], "w") as z:
    z.writestr("savedata/elsewhere.bin", b"not the console's storage")
SAVEZIP
	cp "$work/addon" "$work/sd-in/addon"
	python3 "$here/tests/make-project.py" "$package" "$work/sd.chimeraProject" "$game" --set=renderer=null --set=cpu_speed=4000 \
		--file=savedata="$work/sd-in/saved.zip" --file=content="$work/sd-in/addon" > "$work/sd-make.out" 2>&1
	env -u DISPLAY "$engine" --project "$work/sd.chimeraProject" "$package" --files "$(dirname "$game")" --files "$work/sd-in" \
		--frames 30 --record "$work/sd.movie" --export-savedata "$work/sd-out" > "$work/sd.out" 2>&1
	sd=$?
	python3 "$here/tests/make-project.py" "$package" "$work/sd-bad.chimeraProject" "$game" --set=renderer=null --set=cpu_speed=4000 \
		--file=savedata="$work/sd-bad/saved.zip" > "$work/sd-bad-make.out" 2>&1
	env -u DISPLAY "$engine" --project "$work/sd-bad.chimeraProject" "$package" --files "$(dirname "$game")" --files "$work/sd-bad" \
		--frames 5 --record "$work/sd-bad.movie" > "$work/sd-bad.out" 2>&1
	bad=$?
	back="$(python3 - "$work/sd-in/saved.zip" "$work/sd-out" <<'SAVECMP'
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
print(sum(1 for n in z.namelist() if open(os.path.join(sys.argv[2], n), "rb").read() == z.read(n)) if all(os.path.exists(os.path.join(sys.argv[2], n)) for n in z.namelist()) else 0)
SAVECMP
)"
	if [ "$sd" = 0 ] && [ "$back" = 2 ] && ! find "$work/sd-out" -name addon -o -name addon.header | grep -q . &&
		[ "$bad" != 0 ] && grep -q "which is not the console's storage" "$work/sd-bad.out"; then
		report "frontend:savedata-in" PASS "a save data zip in the project is back in the next export, both files byte for byte (one deflated, one stored); the project's add-on content is not exported; a zip with anything else is refused"
	else
		report "frontend:savedata-in" FAIL "exit $sd, $back of 2 files back; a wrong zip: exit $bad [$(tail -1 "$work/sd-bad.out")]; $(tail -1 "$work/sd.out")"
	fi
fi

# A game that draws its picture at 1920x1080. The console tells every game
# the television is 1280x720, and most draw at that size, but some draw at
# 1920x1080 and let the console scale the picture down. The package declared
# a 1280x720 picture until chimera issue 270, so the engine copied 1280x720
# pixels out of a picture 1920 wide: two squeezed copies side by side, with
# dark lines between. Here the picture chimera-run shows must be this
# repository's runner's, size and pixels. The test needs such a game, which
# this repository does not have: XENIA_TEST_GAME_FULL_HD=<DuckTales:
# Remastered, the Arcade file 3BE5F89B69A86BECAFB4B11F87843D12649E1DF558>.
hd_game="${XENIA_TEST_GAME_FULL_HD:-}"
if [ -z "$chimera_root" ] || [ ! -x "$engine" ] || [ ! -f "$package" ]; then
	report "frontend:full-hd" SKIP "set CHIMERA_ROOT to a Chimera checkout with build/meson-linux/chimera-run and the package (waterbox/build-package.sh -r <it>)"
elif [ -z "$hd_game" ] || [ ! -f "$hd_game" ]; then
	report "frontend:full-hd" SKIP "needs XENIA_TEST_GAME_FULL_HD=<a game that draws at 1920x1080: DuckTales: Remastered>"
elif [ ! -x "$wbx/run-wbx" ] || [ ! -f "$wbx/core.wbx" ]; then
	report "frontend:full-hd" SKIP "no build/wbx/core.wbx: run waterbox/build-guest.sh"
else
	: > "$work/empty.movie"
	crun_game "$package" "$hd_game" "$work/empty.movie" --frames 100 --record "$work/hd.movie" \
		--gpu --settings '{"cpu_speed":"4000"}' --screenshot "80=$work/hd.tga" > "$work/hd.out" 2>&1
	hd=$?
	# the engine counts frames from 0, the runner from 1
	"$wbx/run-wbx" "$wbx/core.wbx" "$hd_game" --instruction-ps "$cost_fast" --frames 81 --renderer vulkan --ppm "$work/hd.ppm" > "$work/hd-wbx.out" 2> "$work/hd-wbx.err"
	verdict="$(python3 "$same" "$work/hd.tga" "$work/hd.ppm" 2>&1)"; same_rc=$?
	if grep -q "no Vulkan on this machine" "$work/hd.out"; then
		report "frontend:full-hd" SKIP "no Vulkan on this machine"
	elif [ "$hd" = 0 ] && [ "$same_rc" = 0 ] && [ "${verdict#same 1920x1080}" != "$verdict" ]; then
		report "frontend:full-hd" PASS "a game that draws at 1920x1080: chimera-run's picture at frame 80 is this repository's runner's ($verdict)"
	else
		report "frontend:full-hd" FAIL "chimera-run exit $hd; pictures: $verdict; $(tail -1 "$work/hd.out")"
	fi
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
