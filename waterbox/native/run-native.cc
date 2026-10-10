// The native reference: Xenia with no window, no host audio and no host pad,
// run on a program for a while and asked what it did. This is the build the
// sandboxed one will be compared with; today it is the host's clock and the
// host's threads, so two runs of it are not yet the same run.
//
//   run-native [--renderer=null|vulkan] [--seconds=N] [--work=<dir>] <program>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "../driver/machine_settings.h"

#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/base/chimera_vsched.h"
#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/null/null_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/memory.h"

DEFINE_transient_path(target, "", "The program to run.", "Chimera");
DEFINE_transient_string(renderer, "null", "null or vulkan.", "Chimera");
DEFINE_transient_string(seconds, "10", "How long to run, on the host's clock.", "Chimera");
DEFINE_transient_path(work, "work-native", "Where the emulator keeps what it writes.", "Chimera");
DEFINE_transient_bool(ram_blocks, false, "Print a hash for each MiB of RAM.", "Chimera");
DEFINE_transient_string(ram_hex, "", "Print RAM as hex: <physical address>:<bytes>, both hex.", "Chimera");

namespace {

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ p[i]) * 0x100000001b3ull;
  }
  return h;
}

int RunNative(const std::vector<std::string>& args) {
  if (cvars::target.empty()) {
    std::fprintf(stderr, "run-native: no program given\n");
    return 2;
  }
  // Upstream's console entry point on this platform leaves logging off, and
  // a logger that was never started does not drop a line: it waits for room
  // that never comes. The first thing to log is the Vulkan loader.
  xe::InitializeLogging("run-native");

  // the settings that make it one machine on every host
  {
    std::string why;
    if (!chimera_xenia::apply_machine_settings(why)) {
      std::fprintf(stderr, "run-native: %s\n", why.c_str());
      return 1;
    }
  }

#ifdef CHIMERA_GUEST
  // A guest has the files it was given and no directories to make: the
  // emulator's roots are names nothing is under.
  const std::filesystem::path work = cvars::work;
#else
  const std::filesystem::path work = std::filesystem::absolute(cvars::work);
  std::filesystem::create_directories(work / "content");
  std::filesystem::create_directories(work / "cache");
#endif

  auto emulator = std::make_unique<xe::Emulator>("", work, work / "content", work / "cache");
  const bool vulkan = cvars::renderer == "vulkan";
  xe::X_STATUS status = emulator->Setup(
      nullptr, nullptr, true,
      [](xe::cpu::Processor* processor) { return xe::apu::nop::NopAudioSystem::Create(processor); },
      [vulkan]() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
        if (vulkan) {
          return std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
        }
        return std::make_unique<xe::gpu::null::NullGraphicsSystem>();
      },
      [](xe::ui::Window* window) {
        std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
        drivers.emplace_back(xe::hid::nop::Create(window, 0));
        return drivers;
      });
  if (XFAILED(status)) {
    std::fprintf(stderr, "run-native: the emulator did not set up (%08X)\n", status);
    return 1;
  }

  bool launched = false;
  emulator->on_launch.AddListener([&launched](uint32_t title_id, const std::string_view name) {
    launched = true;
    std::fprintf(stderr, "run-native: launched title %08X \"%.*s\"\n", title_id, static_cast<int>(name.size()),
                 name.data());
  });

#ifdef CHIMERA_GUEST
  status = emulator->LaunchPath(cvars::target);
#else
  status = emulator->LaunchPath(std::filesystem::absolute(cvars::target));
#endif
  if (XFAILED(status)) {
    std::fprintf(stderr, "run-native: the program did not launch (%08X)\n", status);
    return 1;
  }

  auto* gs = emulator->graphics_system();
  uint32_t last = 0;
  const uint32_t seconds = static_cast<uint32_t>(std::strtoul(cvars::seconds.c_str(), nullptr, 10));
  for (uint32_t second = 1; second <= seconds; second++) {
#ifdef CHIMERA_VSCHED
    // A second of the machine's time: this thread is one of the schedule's,
    // and sleeping on the host's clock would hold everybody up.
    xe::chimera::SleepUntil(static_cast<int64_t>(second) * 1000000000ll);
#else
    std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
    const uint32_t swaps = gs && gs->command_processor() ? gs->command_processor()->counter() : 0;
    std::fprintf(stderr, "run-native: second %u, %u frames (%u this second)\n", second, swaps, swaps - last);
    last = swaps;
  }

  const uint32_t swaps = gs && gs->command_processor() ? gs->command_processor()->counter() : 0;
  std::printf("launched=%d\n", launched ? 1 : 0);
#ifdef CHIMERA_VSCHED
  std::printf("virtualNs=%lld\n", static_cast<long long>(xe::chimera::VirtualNanoseconds()));
  std::printf("switches=%llu\n", static_cast<unsigned long long>(xe::chimera::Switches()));
#endif
  std::printf("frames=%u\n", swaps);
  // The console's 512 MiB, as the machine sees them. Read while the guest
  // runs, so this is a sign of life and not yet a digest to compare.
  std::printf("ramHash=%016" PRIx64 "\n", Fnv(emulator->memory()->physical_membase(), 0x20000000));
  // For finding where two runs' memory differs: a hash a MiB, then the bytes.
  if (cvars::ram_blocks) {
    for (uint32_t mib = 0; mib < 512; mib++) {
      std::printf("ramBlock[%03x]=%016" PRIx64 "\n", mib,
                  Fnv(emulator->memory()->physical_membase() + (size_t(mib) << 20), 1 << 20));
    }
  }
  if (!cvars::ram_hex.empty()) {
    unsigned long address = 0, length = 0;
    if (std::sscanf(cvars::ram_hex.c_str(), "%lx:%lx", &address, &length) == 2 && address < 0x20000000 &&
        length <= 0x20000000 - address) {
      const uint8_t* p = emulator->memory()->physical_membase() + address;
      for (unsigned long i = 0; i < length; i += 32) {
        std::printf("ram[%08lx]=", address + i);
        for (unsigned long k = i; k < i + 32 && k < length; k++) {
          std::printf("%02x", p[k]);
        }
        std::printf("\n");
      }
    }
  }
  std::fflush(stdout);
  std::fflush(stderr);
  xe::ShutdownLogging();
#ifdef CHIMERA_GUEST
  // In the sandbox the call returns; the machine's other threads stay where
  // the scheduler left them. The emulator is not taken apart.
  (void)emulator.release();
  return 0;
#else
  // The guest's threads are the host's and are still running: leaving is the
  // process ending, not the emulator being taken apart.
  std::_Exit(0);
#endif
}

}  // namespace

XE_DEFINE_CONSOLE_APP("run-native", RunNative, "[program]", "target");
