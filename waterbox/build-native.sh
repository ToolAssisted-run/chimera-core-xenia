#!/bin/sh
# Builds Xenia Canary natively and headless: the emulator as static libraries,
# with nothing that needs a desktop. This is the reference the sandboxed build
# will be compared with.
#
#   waterbox/build-native.sh [-j <jobs>]
#
# Needs: clang 20 (CC and CXX may name another), cmake 3.20+, ninja, python3,
# and gcc for the shader tools. Nothing is installed and nothing outside this
# checkout is written.
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
native="$root/build/native"
mkdir -p "$tools/bin" "$native"

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

if [ ! -f "$native/build.ninja" ]; then
	CC="${CC:-clang-20}" CXX="${CXX:-clang++-20}" cmake -S "$xenia" -B "$native" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCHIMERA_HEADLESS=ON > "$root/build/configure-native.log" 2>&1
fi

# What xenia-build.py writes before a build: which commit this is. The date is
# a word and not a date, so that two builds of one commit are one binary.
commit="$(git -C "$xenia" rev-parse HEAD)"
short="$(printf %s "$commit" | cut -c1-9)"
{
	echo "// Written by waterbox/build-native.sh, in place of xenia-build.py."
	echo "#ifndef GENERATED_VERSION_H_"
	echo "#define GENERATED_VERSION_H_"
	echo "#define XE_BUILD_BRANCH \"canary_experimental\""
	echo "#define XE_BUILD_COMMIT \"$commit\""
	echo "#define XE_BUILD_COMMIT_SHORT \"$short\""
	echo "#define XE_BUILD_DATE \"chimera\""
	echo "#endif"
} > "$native/version.h.new"
cmp -s "$native/version.h.new" "$native/version.h" 2>/dev/null || mv "$native/version.h.new" "$native/version.h"
rm -f "$native/version.h.new"

# shellcheck disable=SC2086
VULKAN_SDK="$tools" ninja -C "$native" $jobs > "$root/build/build-native.log" 2>&1 || {
	grep -E "^FAILED|error:" "$root/build/build-native.log" | head -20 >&2
	echo "the native build failed: build/build-native.log" >&2
	exit 1
}
echo "built: $(ls "$native"/obj/Linux/*.a | wc -l) libraries in build/native/obj/Linux"
