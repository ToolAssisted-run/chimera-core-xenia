#!/bin/sh
# Assembles upstream's PowerPC instruction tests (extern/xenia/src/xenia/cpu/
# ppc/testing/*.s) into build/ppc-tests/bin, the way `xb gentests` does, with
# the assembler upstream uses: binutils 2.24 with its VMX128 patch
# (extern/xenia/third_party/binutils), built here out of the tree.
#
# These tests run guest code on the recompiler alone - no kernel, no game -
# which makes them the one thing about this core that can be proved anywhere.
#
#   waterbox/build-ppc-tests.sh
#
# Needs: gcc 13 (binutils 2.24 is from 2013 and newer compilers refuse it),
# make, curl, flex, bison.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
xenia="$root/extern/xenia"
tools="$root/build/tools"
bu="$tools/binutils"
version=2.24
sha256=4930b2886309112c00a279483eaef2f0f8e1b1b62010e0239c16b22af7c346d4
mkdir -p "$bu"

if [ ! -x "$bu/bin/powerpc-none-elf-as" ]; then
	tarball="$bu/binutils-$version.tar.gz"
	if [ ! -f "$tarball" ]; then
		for url in "https://mirrors.kernel.org/sourceware/binutils/releases/binutils-$version.tar.gz" \
			"https://ftp.gnu.org/gnu/binutils/binutils-$version.tar.gz"; do
			curl -fsSL "$url" -o "$tarball.part" && mv "$tarball.part" "$tarball" && break
		done
	fi
	[ -f "$tarball" ] || { echo "could not download binutils $version" >&2; exit 1; }
	echo "$sha256  $tarball" | sha256sum -c - > /dev/null || {
		echo "binutils-$version.tar.gz is not the file this script expects (sha256 $(sha256sum "$tarball" | cut -c1-64))" >&2
		exit 1
	}
	rm -rf "$bu/src"
	mkdir -p "$bu/src"
	tar xzf "$tarball" -C "$bu/src"
	(cd "$bu/src/binutils-$version" && patch -s -p0 < "$xenia/third_party/binutils/binutils-$version-vmx128.patch")
	(
		cd "$bu/src/binutils-$version"
		CC="${BINUTILS_CC:-gcc-13}" CFLAGS="-O1 -w -fcommon" MAKEINFO=true ./configure --disable-debug \
			--disable-dependency-tracking --disable-werror --disable-nls --target=powerpc-none-elf \
			--with-gnu-ld --with-gnu-as --prefix="$bu" > "$bu/configure.log" 2>&1
		make -j"${JOBS:-$(nproc)}" MAKEINFO=true all-gas all-ld all-binutils > "$bu/make.log" 2>&1
		make MAKEINFO=true install-gas install-ld install-binutils >> "$bu/make.log" 2>&1
	) || { tail -20 "$bu/make.log" >&2; echo "binutils did not build: $bu/make.log" >&2; exit 1; }
	rm -rf "$bu/src"
fi

as="$bu/bin/powerpc-none-elf-as"
ld="$bu/bin/powerpc-none-elf-ld"
nm="$bu/bin/powerpc-none-elf-nm"
out="$root/build/ppc-tests/bin"
mkdir -p "$out"
n=0
for src in "$xenia"/src/xenia/cpu/ppc/testing/instr_*.s "$xenia"/src/xenia/cpu/ppc/testing/seq_*.s; do
	[ -f "$src" ] || continue
	name="$(basename "$src" .s)"
	"$as" -a32 -be -mregnames -ma2 -maltivec -mvsx -mvmx128 -R -o "$out/$name.o" "$src"
	"$ld" -A powerpc:common32 -melf32ppc -EB -nostdlib --oformat=binary -Ttext=0x80000000 -e0x80000000 \
		-o "$out/$name.bin" "$out/$name.o" 2> /dev/null
	"$nm" --numeric-sort "$out/$name.o" > "$out/$name.map"
	n=$((n + 1))
done
echo "assembled: $n tests in build/ppc-tests/bin"
