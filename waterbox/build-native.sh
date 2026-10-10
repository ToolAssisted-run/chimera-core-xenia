#!/bin/sh
# Builds Xenia Canary natively and headless: the emulator as static libraries,
# with nothing that needs a desktop. This is the reference the sandboxed build
# will be compared with.
#
#   waterbox/build-native.sh [-j <jobs>]
#
# Needs: gcc (CC and CXX may name another), cmake 3.20+, ninja, python3.
# Nothing is installed and nothing outside this checkout is written.
#
# The compiler is the one a guest is built with, on purpose: the same game
# run by a clang build and by a gcc build ends with a few bytes of the
# console's memory different (docs/PLAN.md), so a reference built with
# another compiler than the guest's is a reference for something else.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
jobs=""
while getopts j: opt; do
	case "$opt" in
		j) jobs="-j $OPTARG" ;;
		*) echo "usage: $0 [-j <jobs>]" >&2; exit 2 ;;
	esac
done

"$here/apply-patches.sh"

xenia="$root/extern/xenia"
tools="$root/build/tools"
mkdir -p "$tools/bin"

# The shader tools. Canary's Vulkan shaders are compiled at build time by
# glslangValidator and optimised by spirv-opt, from a Vulkan SDK or the PATH.
# They are built here instead, from the sources Canary already pins
# (third_party/glslang, third_party/SPIRV-Tools) and the SPIRV-Headers revision
# its SPIRV-Tools names (extern/spirv-headers), so the shaders do not depend
# on what the machine happens to have installed.
if [ ! -x "$tools/bin/glslangValidator" ]; then
	cmake -S "$xenia/third_party/glslang" -B "$tools/glslang" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=ON \
		-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ > "$tools/glslang.log" 2>&1
	# shellcheck disable=SC2086
	ninja -C "$tools/glslang" $jobs glslang-standalone >> "$tools/glslang.log" 2>&1
	cp "$tools/glslang/StandAlone/glslang" "$tools/bin/glslangValidator"
fi
if [ ! -x "$tools/bin/spirv-opt" ] || [ ! -x "$tools/bin/spirv-dis" ]; then
	cmake -S "$xenia/third_party/SPIRV-Tools" -B "$tools/spirv-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DSPIRV-Headers_SOURCE_DIR="$root/extern/spirv-headers" -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF \
		-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ > "$tools/spirv-tools.log" 2>&1
	# shellcheck disable=SC2086
	ninja -C "$tools/spirv-tools" $jobs spirv-opt spirv-dis >> "$tools/spirv-tools.log" 2>&1
	cp "$tools/spirv-tools/tools/spirv-opt" "$tools/spirv-tools/tools/spirv-dis" "$tools/bin/"
fi

# Two trees of the same sources:
#
#   build/native   the host's threads on the host's clock, as upstream runs.
#                  Upstream's instruction tests are run on this one.
#   build/vsched   one thread at a time on the emulator's own clock (patch
#                  0003, CHIMERA_VSCHED): the reference that repeats, and
#                  what the sandboxed build will be compared with.
#
# XENIA_BUILD_TESTS brings upstream's instruction-test runner
# (xenia-cpu-ppc-tests), which waterbox/run-ppc-tests.sh runs;
# CHIMERA_DRIVER_DIR brings the core's own driver, waterbox/native/run-native.
commit="$(git -C "$xenia" rev-parse HEAD)"
short="$(printf %s "$commit" | cut -c1-9)"
for tree in native vsched; do
	dir="$root/build/$tree"
	mkdir -p "$dir"
	flags="-DCHIMERA_HEADLESS=ON -DXENIA_BUILD_TESTS=ON"
	[ "$tree" = vsched ] && flags="-DCHIMERA_HEADLESS=ON -DCHIMERA_VSCHED=ON -DCHIMERA_FLATMEM=ON -DCHIMERA_NOFILEMAP=ON -DXENIA_BUILD_TESTS=ON"
	# shellcheck disable=SC2086
	CC="${CC:-gcc}" CXX="${CXX:-g++}" cmake -S "$xenia" -B "$dir" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release $flags -DCHIMERA_DRIVER_DIR="$here/native" \
		> "$root/build/configure-$tree.log" 2>&1

	# What xenia-build.py writes before a build: which commit this is. The
	# date is a word and not a date, so that two builds of one commit are one
	# binary.
	{
		echo "// Written by waterbox/build-native.sh, in place of xenia-build.py."
		echo "#ifndef GENERATED_VERSION_H_"
		echo "#define GENERATED_VERSION_H_"
		echo "#define XE_BUILD_BRANCH \"canary_experimental\""
		echo "#define XE_BUILD_COMMIT \"$commit\""
		echo "#define XE_BUILD_COMMIT_SHORT \"$short\""
		echo "#define XE_BUILD_DATE \"chimera\""
		echo "#endif"
	} > "$dir/version.h.new"
	cmp -s "$dir/version.h.new" "$dir/version.h" 2>/dev/null || mv "$dir/version.h.new" "$dir/version.h"
	rm -f "$dir/version.h.new"

	# shellcheck disable=SC2086
	VULKAN_SDK="$tools" ninja -C "$dir" $jobs > "$root/build/build-$tree.log" 2>&1 || {
		grep -E "^FAILED|error:" "$root/build/build-$tree.log" | head -20 >&2
		echo "the $tree build failed: build/build-$tree.log" >&2
		exit 1
	}
	echo "built: build/$tree/bin/Linux/run-native"
done
