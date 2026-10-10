#!/bin/sh
# The core's gate, as far as the core goes. Each leg says what it compared.
# It must end with "0 failed", and a SKIP is not a pass.
#
#   waterbox/run-gate.sh
#
# Needs waterbox/build-native.sh to have run. The game leg needs a disc image
# this repository does not have: tests/roms/Mirrors Edge [NTSCU][NTSCJ].iso,
# or XENIA_TEST_GAME=<an image of that game>.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
run="$root/build/vsched/bin/Linux/run-native"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
ok=0; failed=0; skipped=0
report() { # <leg> <PASS|FAIL|SKIP> <what>
	printf '%-22s %-6s %s\n' "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok + 1)) ;; FAIL) failed=$((failed + 1)) ;; *) skipped=$((skipped + 1)) ;; esac
}
digest() { grep -E '^(virtualNs|switches|frames|ramHash)='; }

game="${XENIA_TEST_GAME:-$root/tests/roms/Mirrors Edge [NTSCU][NTSCJ].iso}"
seconds="${XENIA_GATE_SECONDS:-5}"

# One thread at a time on the emulator's clock: two runs of a game are one
# run - the same handing over of the baton, the same frames, the same 512 MiB
# - and a run in which an instruction costs something else is another. The
# second half is the control: without it, "the same" could be two runs that
# both did nothing.
if [ ! -x "$run" ]; then
	report "game:repeats" SKIP "no build/vsched/bin/Linux/run-native: run waterbox/build-native.sh"
elif [ ! -f "$game" ]; then
	report "game:repeats" SKIP "needs tests/roms/Mirrors Edge [NTSCU][NTSCJ].iso, or XENIA_TEST_GAME=<that image>"
else
	for n in a b; do
		"$run" --renderer=null --seconds="$seconds" --work="$work/$n" "$game" 2> "$work/$n.err" | digest > "$work/$n.out"
	done
	CHIMERA_INSTRUCTION_PS=260 "$run" --renderer=null --seconds="$seconds" --work="$work/c" "$game" 2> "$work/c.err" | digest > "$work/c.out"
	frames="$(sed -n 's/^frames=//p' "$work/a.out")"
	if [ -s "$work/a.out" ] && cmp -s "$work/a.out" "$work/b.out" && ! cmp -s "$work/a.out" "$work/c.out" && [ "${frames:-0}" -gt 0 ]; then
		report "game:repeats" PASS "$seconds seconds of the machine's time, twice: $(tr '\n' ' ' < "$work/a.out")- and another run when an instruction costs 260 ps instead of 250"
	else
		report "game:repeats" FAIL "first [$(tr '\n' ' ' < "$work/a.out")] second [$(tr '\n' ' ' < "$work/b.out")] with another instruction cost [$(tr '\n' ' ' < "$work/c.out")]"
	fi
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
