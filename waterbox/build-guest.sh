#!/bin/sh
# The guest (sandbox) build: Xenia's own CMake under miniBox's guest
# toolchain, then the .wbx programs linked from its archives, and the host
# runners for them.
#
#   waterbox/build-guest.sh -m <miniBox checkout> [-j <jobs>]
#
# The miniBox checkout (or MINIBOX_DIR) must be built with the C++ guest
# toolchain and its host library:
#   meson setup <miniBox>/build/meson-cpp <miniBox> -Dguest_cpp=true && ninja -C <miniBox>/build/meson-cpp
#   meson setup <miniBox>/build/meson-linux <miniBox> && ninja -C <miniBox>/build/meson-linux
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-}"
jobs=""
while getopts m:j: opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		j) jobs="-j $OPTARG" ;;
		*) echo "usage: $0 -m <miniBox checkout> [-j <jobs>]" >&2; exit 2 ;;
	esac
done
[ -n "$mb" ] || { echo "name a miniBox checkout: -m <dir> or MINIBOX_DIR" >&2; exit 2; }
mb="$(cd "$mb" && pwd)"
export MINIBOX_DIR="$mb"
mbuild="$mb/build/meson-cpp"
sr="$mbuild/guest-sysroot"
[ -f "$sr/lib/libstdc++.a" ] || { echo "no C++ guest toolchain at $sr" >&2; exit 1; }
[ -x "$root/build/tools/bin/glslangValidator" ] || { echo "run waterbox/build-native.sh first: it builds the shader tools" >&2; exit 1; }

"$here/apply-patches.sh"

xenia="$root/extern/xenia"
guest="$root/build/guest"
out="$root/build/wbx"
mkdir -p "$guest" "$out"

# The Vulkan bridge, both halves, from the registry miniBox carries: the
# guest's wrappers are compiled into the core, the host's dispatch into the
# runner.
vkgen="$guest/vk-gen"
[ -f "$mb/source/vk/gen-vk-bridge.py" ] || { echo "this miniBox has no Vulkan bridge (source/vk): it is on the vk-bridge branch" >&2; exit 1; }
mkdir -p "$vkgen.new"
python3 "$mb/source/vk/gen-vk-bridge.py" "$mb/extern/vulkan-headers/registry/vk.xml" "$mb/source/vk/vk-entry-points.txt" "$vkgen.new" > "$root/build/gen-vk.log" 2>&1 || {
	tail -5 "$root/build/gen-vk.log" >&2; echo "the Vulkan bridge did not generate: build/gen-vk.log" >&2; exit 1; }
mkdir -p "$vkgen"
for f in vk-bridge-ops.h vk-bridge-guest.c vk-bridge-host.inc; do
	cmp -s "$vkgen.new/$f" "$vkgen/$f" 2> /dev/null || cp "$vkgen.new/$f" "$vkgen/$f"
done
rm -rf "$vkgen.new"

cmake -S "$xenia" -B "$guest" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DCHIMERA_HEADLESS=ON -DCHIMERA_VSCHED=ON -DCHIMERA_FLATMEM=ON -DCHIMERA_NOFILEMAP=ON \
	-DCHIMERA_DRIVER_DIR="$here/guest" -DCHIMERA_VK_GEN="$vkgen" > "$root/build/configure-guest.log" 2>&1
commit="$(git -C "$xenia" rev-parse HEAD)"
short="$(printf %s "$commit" | cut -c1-9)"
{
	echo "// Written by waterbox/build-guest.sh, in place of xenia-build.py."
	echo "#ifndef GENERATED_VERSION_H_"
	echo "#define GENERATED_VERSION_H_"
	echo "#define XE_BUILD_BRANCH \"canary_experimental\""
	echo "#define XE_BUILD_COMMIT \"$commit\""
	echo "#define XE_BUILD_COMMIT_SHORT \"$short\""
	echo "#define XE_BUILD_DATE \"chimera\""
	echo "#endif"
} > "$guest/version.h.new"
cmp -s "$guest/version.h.new" "$guest/version.h" 2>/dev/null || mv "$guest/version.h.new" "$guest/version.h"
rm -f "$guest/version.h.new"
# shellcheck disable=SC2086
VULKAN_SDK="$root/build/tools" ninja -C "$guest" $jobs > "$root/build/build-guest.log" 2>&1 || {
	grep -E "^FAILED|error:" "$root/build/build-guest.log" | head -20 >&2
	echo "the guest build failed: build/build-guest.log" >&2
	exit 1
}

# A .wbx: the program's own archive whole (nothing refers to its exports),
# then every archive of the build in one group - they refer to each other
# freely.
link_wbx() { # <name> <archive of the program>
	libs="$(find "$guest" -name '*.a' ! -name 'libxenia-guest-*.a' | sort | tr '\n' ' ')"
	# shellcheck disable=SC2086
	g++ -specs "$sr/lib/musl-gcc.specs" -mcmodel=large -fno-pic -fno-pie \
		-static -no-pie -Wl,--eh-frame-hdr,-O2,--no-relax,-z,stack-size=8388608 -T "$mb/source/guest/linkscript.T" \
		-Wl,-u,pthread_once -Wl,-u,pthread_cond_wait -Wl,-u,pthread_cond_broadcast -Wl,-u,pthread_key_create \
		-Wl,-u,pthread_mutexattr_init -Wl,-u,pthread_mutexattr_settype -Wl,-u,pthread_mutexattr_destroy \
		-o "$out/$1.wbx" \
		"$mbuild/source/guest/cxxglue.c.o" "$mbuild/source/guest/emulibc.c.o" \
		-Wl,--whole-archive "$2" -Wl,--no-whole-archive \
		-Wl,--start-group $libs -Wl,--end-group \
		-L"$sr/lib" -lstdc++ -lgcc -lgcc_eh -lc > "$root/build/link-$1.log" 2>&1 || {
		grep -E "undefined reference|error" "$root/build/link-$1.log" | sed 's/.*undefined reference to/undefined:/' | sort | uniq -c | sort -rn | head -40 >&2
		echo "$1.wbx did not link: build/link-$1.log" >&2
		return 1
	}
	sh "$mb/source/guest/check-wbx.sh" "$out/$1.wbx"
	echo "built: build/wbx/$1.wbx"
}
link_wbx ppc-tests "$(find "$guest" -name libxenia-guest-ppc.a | head -1)"
link_wbx core "$(find "$guest" -name libxenia-guest-core.a | head -1)"

mbhost="$mb/build/meson-linux/source/host"
[ -f "$mbhost/libminiboxhost.so" ] || { echo "no miniBox host library at $mbhost" >&2; exit 1; }
gcc -O2 -g -Wall -I"$mb/source/host" -o "$out/run-ppc-wbx" "$here/run-ppc-wbx.c" \
	"$mbhost/libminiboxhost.so" -Wl,-rpath,"$mbhost"
echo "built: build/wbx/run-ppc-wbx"
# the core's runner carries the host half of the Vulkan bridge
gcc -O2 -g -Wall -I"$mb/source/host" -I"$mb/source/vk" -I"$vkgen" -I"$mb/extern/vulkan-headers/include" \
	-o "$out/run-wbx" "$here/run-wbx.c" "$mb/source/vk/vk-host.c" \
	"$mbhost/libminiboxhost.so" -Wl,-rpath,"$mbhost" -ldl
echo "built: build/wbx/run-wbx"
