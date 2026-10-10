// The core's exports: what the frontend (and both runners of the gate) call.
// The same file in both flavours: natively they are called directly, in the
// sandbox through miniBox. A project names its files in "slots" ({"game":
// ["x.iso"]}, each mounted under that name); a file opened on its own arrives
// as what "rom.name" names. The settings are the JSON object in "settings".
// Mounted files in the sandbox, files in the working directory natively.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "xenia_driver.h"

#ifdef CHIMERA_GUEST
#include "vk-bridge.h"
#endif

#define CORE_EXPORT extern "C" __attribute__((visibility("default"), used))
#ifdef CHIMERA_GUEST
// not in a savestate: scratch that is written and read within one call
#define CORE_INVISIBLE __attribute__((section(".ldata.invis")))
#else
#define CORE_INVISIBLE
#endif

namespace {

// What the console sends a television, and what the frontend is shown
// before the program has shown anything (and for good with the renderer that
// draws nothing): 1280x720 of black.
constexpr int SCREEN_W = 1280, SCREEN_H = 720;

std::string g_error;
#ifdef CHIMERA_GUEST
bool g_bridge = false;
#else
// natively the device is the host's own
bool g_bridge = true;
#endif
std::vector<uint32_t> g_blank(SCREEN_W * SCREEN_H, 0xFF000000u);
// Each pad's axes for the next frame, AXES_PER_PORT a port.
int32_t g_axes[chimera_xenia::PORTS * chimera_xenia::AXES_PER_PORT];

std::string read_file(const char* name) {
  std::string out;
  if (FILE* f = std::fopen(name, "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
      out.append(buf, n);
    }
    std::fclose(f);
  }
  return out;
}

// The string value of a key in a flat JSON object, or "".
std::string json_string(const std::string& json, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  const auto at = json.find(k);
  if (at == std::string::npos) {
    return "";
  }
  size_t i = json.find(':', at + k.size());
  if (i == std::string::npos) {
    return "";
  }
  i = json.find('"', i);
  if (i == std::string::npos) {
    return "";
  }
  const size_t end = json.find('"', i + 1);
  return end == std::string::npos ? "" : json.substr(i + 1, end - i - 1);
}

// The first name in a slot of the project's slot map, or "".
std::string slot_first(const std::string& slots, const char* id) {
  const std::string k = std::string("\"") + id + "\"";
  const auto at = slots.find(k);
  if (at == std::string::npos) {
    return "";
  }
  size_t i = slots.find('[', at + k.size());
  if (i == std::string::npos) {
    return "";
  }
  i = slots.find_first_not_of(" \t\r\n", i + 1);
  if (i == std::string::npos || slots[i] != '"') {
    return "";
  }
  std::string out;
  for (size_t j = i + 1; j < slots.size() && slots[j] != '"'; j++) {
    if (slots[j] == '\\' && j + 1 < slots.size()) {
      j++;
    }
    out += slots[j];
  }
  return out;
}

// A mounted file's name as it opens: a project mounts a slot's file under
// the name the map gives, a file opened on its own sits at "/<name>" too.
std::string mounted(const std::string& name) {
  if (name.empty() || name[0] == '/') {
    return name;
  }
  if (FILE* f = std::fopen(name.c_str(), "rb")) {
    std::fclose(f);
    return name;
  }
  if (FILE* f = std::fopen(("/" + name).c_str(), "rb")) {
    std::fclose(f);
    return "/" + name;
  }
  return name;
}

}  // namespace

CORE_EXPORT const char* GetLoadError(void) { return g_error.c_str(); }

#ifdef CHIMERA_GUEST
// miniBox calls this on the faulting thread when the guest touches a page
// the guest itself protected: the renderer's caches watch the console's
// memory that way. Nonzero: the page was let go, try the access again.
CORE_EXPORT int GuestFaultHandler(uint64_t addr, uint64_t is_write) {
  return chimera_xenia::watched_access(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), is_write != 0) ? 1 : 0;
}
#endif

#ifdef CHIMERA_GUEST
// The Vulkan bridge: the host's dispatcher, handed over before Init. Without
// it there is no device in the sandbox and only the renderer that draws
// nothing can be had.
CORE_EXPORT void SetVulkanBridge(uint64_t addr) {
  g_bridge = chimera_vk_install(reinterpret_cast<chimera_vk_bridge_fn>(static_cast<uintptr_t>(addr)));
}
#endif

CORE_EXPORT int Init(void) {
  const std::string slots = read_file("slots");
  std::string name = mounted(slot_first(slots, "game"));
  if (name.empty()) {
    name = read_file("rom.name");
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
      name.pop_back();
    }
    name = mounted(name);
  }
  if (name.empty()) {
    g_error = "no game: neither a \"game\" slot nor rom.name";
    return 0;
  }
  const std::string settings = read_file("settings");
  chimera_xenia::Options options;
  // "vulkan-hw": named so that a frontend knows to hand the graphics device
  // over. The gate's runners say "vulkan".
  const std::string renderer = json_string(settings, "renderer");
  if (renderer == "vulkan-hw" || renderer == "vulkan") {
    options.renderer = "vulkan";
  } else if (renderer == "null" || renderer.empty()) {
    options.renderer = "null";
  } else {
    g_error = "no such renderer: " + renderer;
    return 0;
  }
  const std::string pads = json_string(settings, "pads");
  if (!pads.empty()) {
    options.pads = std::atoi(pads.c_str());
    if (options.pads < 0 || options.pads > chimera_xenia::PORTS) {
      g_error = "no such number of pads: " + pads;
      return 0;
    }
  }
  options.log = json_string(settings, "log") == "1";
  options.trace_schedule = json_string(settings, "trace_schedule") == "1";
  if (options.renderer == "vulkan" && !g_bridge) {
    g_error = "the Vulkan renderer needs a device, and none was handed over (this machine may have no Vulkan, or "
              "this frontend no Vulkan bridge)";
    return 0;
  }
  return chimera_xenia::boot(name, options, g_error) ? 1 : 0;
}

// An axis of the next frame (the frontend sets every one before each frame):
// AXES_PER_PORT for the first port, then the second's, and so on.
CORE_EXPORT void SetAxis(int32_t index, int32_t value) {
  if (index >= 0 && index < static_cast<int32_t>(sizeof g_axes / sizeof *g_axes)) {
    g_axes[index] = value;
  }
}

// The buttons: fifteen bits a port, the first port lowest, in the order the
// package declares them (waterbox.config) - the d-pad's four, Start, Back,
// the two stick presses, the two shoulders, Guide, then A, B, X, Y. That is
// the console's own word with its one unused bit (11) closed up.
CORE_EXPORT void FrameAdvance(uint64_t buttons) {
  for (int port = 0; port < chimera_xenia::PORTS; port++) {
    const uint32_t declared = static_cast<uint32_t>(buttons >> (15 * port)) & 0x7FFF;
    const uint16_t word = static_cast<uint16_t>((declared & 0x07FF) | ((declared >> 11) << 12));
    chimera_xenia::set_pad(port, word, g_axes + port * chimera_xenia::AXES_PER_PORT);
  }
  chimera_xenia::frame();
}

CORE_EXPORT uint32_t* GetVideoBgra(void) {
  int w, h;
  const uint32_t* picture = chimera_xenia::video(w, h);
  return picture ? const_cast<uint32_t*>(picture) : g_blank.data();
}
CORE_EXPORT int GetVideoWidth(void) {
  int w, h;
  return chimera_xenia::video(w, h) ? w : SCREEN_W;
}
CORE_EXPORT int GetVideoHeight(void) {
  int w, h;
  return chimera_xenia::video(w, h) ? h : SCREEN_H;
}

// Turbo (the frontend's fast-forward and seeks): off, the picture is not
// read back. The machine draws all the same.
CORE_EXPORT void SetRenderingEnabled(int on) { chimera_xenia::set_rendering(on != 0); }

CORE_EXPORT int16_t* GetAudio(void) {
  int pairs;
  return const_cast<int16_t*>(chimera_xenia::audio(pairs));
}
CORE_EXPORT int GetAudioSampleCount(void) {
  int pairs;
  chimera_xenia::audio(pairs);
  return pairs;
}

// The frontend says a savestate is about to be made (its engine calls this
// before every one): what only the graphics device holds is brought into
// the machine, where the state will find it.
CORE_EXPORT void StateSaving(void) { chimera_xenia::state_saving(); }

// For the gate: the renderer made again on a new device before the next
// frame, as after a load.
CORE_EXPORT void RemakeRenderer(void) { chimera_xenia::remake_renderer(); }

CORE_EXPORT int InputWasRead(void) { return chimera_xenia::input_was_read() ? 1 : 0; }

// Save data out (the frontend's Export Save Data): a snapshot of what the
// console keeps - its profile and what games saved - then each file by index.
CORE_EXPORT int32_t GetSaveDataFileCount(void) { return static_cast<int32_t>(chimera_xenia::savedata_snapshot()); }
CORE_EXPORT const char* GetSaveDataFileName(int32_t i) {
  return chimera_xenia::savedata_name(static_cast<size_t>(i)).c_str();
}
CORE_EXPORT int64_t GetSaveDataFileSize(int32_t i) {
  return static_cast<int64_t>(chimera_xenia::savedata_bytes(static_cast<size_t>(i)).size());
}
CORE_EXPORT const uint8_t* GetSaveDataFileBuffer(int32_t i) {
  return chimera_xenia::savedata_bytes(static_cast<size_t>(i)).data();
}

// No memory domain. The console's 512 MiB are one block that never moves,
// but a block a tool may not read by itself: the program closes pages of it
// (a read then faults, for a tool as for the program) and the renderer
// watches others. The RAM tools get the bus, which is read from inside.
CORE_EXPORT int GetMemoryDomainCount(void) { return 0; }
CORE_EXPORT const char* GetMemoryDomainName(int32_t) { return ""; }
CORE_EXPORT uint8_t* GetMemoryDomainPtr(int32_t) { return nullptr; }
CORE_EXPORT int64_t GetMemoryDomainSize(int32_t) { return 0; }
CORE_EXPORT int GetMemoryDomainWritable(int32_t) { return 0; }

// The console's 32-bit address space, as a program sees it.
CORE_EXPORT int32_t GetBusCount(void) { return chimera_xenia::bus_ready() ? 1 : 0; }
CORE_EXPORT const char* GetBusName(int32_t) { return "Memory"; }
CORE_EXPORT int64_t GetBusSize(int32_t) { return int64_t(1) << 32; }
CORE_EXPORT int32_t GetBusWritable(int32_t) { return 1; }
CORE_EXPORT int32_t PeekBus(int32_t bus, int32_t addr) {
  uint8_t value = 0;
  if (bus == 0) {
    chimera_xenia::bus_read(static_cast<uint32_t>(addr), &value, 1);
  }
  return value;
}
CORE_EXPORT void PokeBus(int32_t bus, int32_t addr, int32_t value) {
  if (bus == 0) {
    chimera_xenia::bus_write(static_cast<uint32_t>(addr), static_cast<uint8_t>(value));
  }
}
namespace {
CORE_INVISIBLE uint8_t g_bus_run[65536];
}
CORE_EXPORT const uint8_t* ReadBus(int32_t bus, int64_t addr, int32_t len) {
  if (len < 0) {
    len = 0;
  }
  if (len > static_cast<int32_t>(sizeof g_bus_run)) {
    len = static_cast<int32_t>(sizeof g_bus_run);
  }
  std::memset(g_bus_run, 0, static_cast<size_t>(len));
  if (bus == 0 && addr >= 0 && addr + len <= (int64_t(1) << 32)) {
    chimera_xenia::bus_read(static_cast<uint32_t>(addr), g_bus_run, static_cast<size_t>(len));
  }
  return g_bus_run;
}

CORE_EXPORT uint64_t GetFrameCount(void) { return chimera_xenia::frames(); }
CORE_EXPORT uint64_t GetMachineTimeNs(void) { return chimera_xenia::time_ns(); }
CORE_EXPORT uint64_t GetSwitchCount(void) { return chimera_xenia::switches(); }
CORE_EXPORT uint64_t GetGpuCounter(void) { return chimera_xenia::gpu_counter(); }
// A digest of the console's 512 MiB, for the gate.
CORE_EXPORT uint64_t GetRamDigest(void) { return chimera_xenia::ram_digest(); }
