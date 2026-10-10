#!/bin/sh
# Runs upstream's PowerPC instruction tests on the native build and writes
# what each one did to build/ppc-tests/native.result: one line a test,
# "<name> ok" or "<name> FAILED", sorted. That file is the reference the
# sandboxed build is held to - the SAME tests passing and the same failing -
# and not a claim that the recompiler is right: on this host it is not, for
# about one test in six (docs/PLAN.md).
#
#   waterbox/build-native.sh && waterbox/build-ppc-tests.sh && waterbox/run-ppc-tests.sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
runner="$root/build/native/bin/Linux/xenia-cpu-ppc-tests"
bins="$root/build/ppc-tests/bin"
[ -x "$runner" ] || { echo "no runner: run waterbox/build-native.sh" >&2; exit 1; }
[ -d "$bins" ] || { echo "no tests: run waterbox/build-ppc-tests.sh" >&2; exit 1; }
log="$root/build/ppc-tests/native.log"
# the runner's exit code is "some test failed", which is expected here
(cd "$root/build" && "$runner" --test_bin_path="$bins/" --log_file=stdout > "$log" 2>&1) || true
grep -q '^Total tests:' "$log" || { tail -5 "$log" >&2; echo "the runner did not finish: $log" >&2; exit 1; }
sed -n 's/^ *\[\([^]]*\)\] FAILED.*/\1/p' "$log" | sort -u > "$root/build/ppc-tests/native.failed"
grep -E '^(Total tests|Passed|Failed):' "$log"
echo "failing tests: $(wc -l < "$root/build/ppc-tests/native.failed") names in build/ppc-tests/native.failed, sha256 $(sha256sum "$root/build/ppc-tests/native.failed" | cut -c1-16)"
