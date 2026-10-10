#!/bin/sh
# Writes patches/ from the patched extern/xenia tree: the reverse of
# apply-patches.sh. A patch is the tree's difference in a list of paths; what
# no list names goes to the patch marked "*". Edit the tree, run this, and
# apply-patches.sh must then say "already applied". A file is in ONE patch,
# whole: where two patches' subjects meet in a file (x64_emitter.cc has the
# scheduler's budget and the recompiler's move), it is in the earlier one.
set -eu
# a reader that stops reading (| head) must not stop this half way: it has
# deleted the patches by then and written only some of them back
trap '' PIPE
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
xenia="$root/extern/xenia"

# name | paths (space separated), in the order the patches apply
series='
0001-chimera-a-build-with-no-desktop|CMakeLists.txt third_party/CMakeLists.txt cmake/XeniaHelpers.cmake src/xenia/CMakeLists.txt src/xenia/ui/CMakeLists.txt src/xenia/base/CMakeLists.txt src/xenia/ui/imgui_drawer.cc src/xenia/ui/chimera_headless.cc src/xenia/ui/vulkan/vulkan_api.h src/xenia/ui/vulkan/vulkan_presenter.cc src/xenia/ui/vulkan/vulkan_provider.cc src/xenia/gpu/null/null_graphics_system.cc
0002-chimera-an-emulator-with-no-window|src/xenia/emulator.cc
0003-chimera-one-thread-at-a-time-on-one-clock|*
0004-chimera-what-gcc-will-not-take|src/xenia/base/cvar.h src/xenia/base/memory.cc src/xenia/gpu/command_processor.h
0005-chimera-the-instruction-tests-in-one-process|src/xenia/cpu/ppc/testing/ppc_testing_main.cc src/xenia/cpu/ppc/testing/CMakeLists.txt
0006-chimera-the-memory-in-one-block|src/xenia/base/chimera_flatmem.h src/xenia/memory.h src/xenia/memory.cc src/xenia/cpu/ppc/ppc_context.h src/xenia/cpu/thread_state.cc src/xenia/cpu/backend/x64/x64_seq_memory.cc src/xenia/cpu/mmio_handler.h src/xenia/cpu/mmio_handler.cc
0007-chimera-the-recompiler-from-the-memory-base|src/xenia/cpu/backend/code_cache_base.h src/xenia/cpu/backend/x64/x64_emitter.h src/xenia/cpu/backend/x64/x64_backend.cc
0008-chimera-a-disc-image-that-is-read|src/xenia/vfs/devices/disc_image_device.h src/xenia/vfs/devices/disc_image_device.cc src/xenia/vfs/devices/disc_image_file.cc src/xenia/vfs/devices/disc_image_entry.cc
0009-chimera-the-vulkan-loader-is-the-bridge|src/xenia/ui/vulkan/vulkan_instance.cc
'

# new files are part of the difference
git -C "$xenia" add -N . > /dev/null 2>&1
named=""
for line in $(printf '%s\n' "$series" | grep '|' | grep -v '|\*$' | cut -d'|' -f2 | tr ' ' '\n'); do
	named="$named $line"
done
rm -f "$root"/patches/*.patch
printf '%s\n' "$series" | grep '|' | while IFS='|' read -r name paths; do
	if [ "$paths" = "*" ]; then
		excludes=""
		for p in $named; do excludes="$excludes :!$p"; done
		# shellcheck disable=SC2086
		git -C "$xenia" diff -- . $excludes > "$root/patches/$name.patch"
	else
		# shellcheck disable=SC2086
		git -C "$xenia" diff -- $paths > "$root/patches/$name.patch"
	fi
	echo "$name.patch: $(grep -c '^diff --git' "$root/patches/$name.patch") files" 2> /dev/null || true
done
git -C "$xenia" reset -q
