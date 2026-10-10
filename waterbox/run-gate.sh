#!/bin/sh
# The core's gate, as far as the core goes. Each leg says what it compared.
# It must end with "0 failed", and a SKIP is not a pass.
#
#   waterbox/run-gate.sh
#
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
native() { # <name> <options...>: a native run, its lines in $work/<name>.out
	name="$1"; shift
	"$run" "$game" "$work/$name-work" "$@" > "$work/$name.out" 2> "$work/$name.err"
}
sandbox() { # <name> <options...>
	name="$1"; shift
	"$wbx/run-wbx" "$wbx/core.wbx" "$game" "$@" > "$work/$name.out" 2> "$work/$name.err"
}

game="${XENIA_TEST_GAME:-$root/tests/roms/Mirrors Edge [NTSCU][NTSCJ].iso}"
frames="${XENIA_GATE_FRAMES:-300}"
half=$((frames / 2))
# a button held for a while in the middle of the run: A on the first pad
press="--press $half:10:1000"
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
	CHIMERA_INSTRUCTION_PS=260 native c --frames "$frames" --every 60
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

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
