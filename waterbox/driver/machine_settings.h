// What makes this an Xbox 360 that is the same on every host: the emulator's
// settings the core does not leave to anybody. One place, for the native
// reference (native/run-native.cc) and the core's driver alike.
#ifndef CHIMERA_XENIA_MACHINE_SETTINGS_H_
#define CHIMERA_XENIA_MACHINE_SETTINGS_H_

#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/platform_amd64.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/kernel/kernel_flags.h"

// declared where they are defined, in sources with no header for them
DECLARE_bool(emit_inline_mmio_checks);
DECLARE_bool(disable_instruction_infocache);
DECLARE_bool(store_shaders);
DECLARE_int32(vulkan_pipeline_creation_threads);

namespace chimera_xenia {

// False, with the reason, on a host that cannot be this machine.
inline bool apply_machine_settings(std::string& error) {
  // The kernel's own switch for "nobody is there to answer a dialog".
  cvars::headless = true;

  // What the recompiler may use of the host's processor is one value on every
  // machine: AVX2 and FMA, and nothing else it knows of (user-decided,
  // 2026-10-10). It emits different code with different features and the
  // results differ - a fused multiply-add rounds once, two steps round twice -
  // so a host without both cannot run the same machine, and is refused.
  cvars::x64_extension_mask = xe::amd64::kX64EmitAVX2 | xe::amd64::kX64EmitFMA;

  // The console's device registers are found by looking at the address of a
  // 32-bit load or store, and not by letting it fault and reading the
  // processor's state in a signal handler: a guest gets no such state.
  // Upstream has both; this is its other one.
  cvars::emit_inline_mmio_checks = true;

  // Upstream keeps a file per game of which instructions were called and
  // which touched a register, and reads it back the next time: a run would
  // depend on the runs before it. It is not kept.
  cvars::disable_instruction_infocache = true;

  // Nor the shaders and pipelines a game used, which upstream writes down
  // beside its cache and compiles ahead of time on the next run - with a
  // thread of its own to write them, so that a machine with somewhere to
  // write was not the machine without.
  cvars::store_shaders = false;

  // Pipelines are made by the thread that needs them. Upstream has threads
  // for it, which in a sandbox are no faster (one thread runs at a time) and
  // which a renderer made again on another device would have to end and
  // start again - and a thread started later is a thread in another place
  // in the order, which is another machine.
  cvars::vulkan_pipeline_creation_threads = 0;

  const uint64_t have = xe::amd64::GetFeatureFlags();
  const uint64_t need = xe::amd64::kX64EmitAVX2 | xe::amd64::kX64EmitFMA;
  if ((have & need) != need) {
    error = "this processor has no AVX2 or no FMA, and the core needs both";
    return false;
  }
  return true;
}

}  // namespace chimera_xenia

#endif  // CHIMERA_XENIA_MACHINE_SETTINGS_H_
