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

#include "suggest.h"
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
// how many controllers are plugged in (the "pads" setting), from the first port
int g_pads = 1;
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

// A value in a flat JSON object as it is written there, a string without its
// quotes and its escapes: "strict", true, 80. "" when the key is not there.
std::string json_raw(const std::string& json, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  const auto at = json.find(k);
  if (at == std::string::npos) {
    return "";
  }
  size_t i = json.find(':', at + k.size());
  if (i == std::string::npos) {
    return "";
  }
  i = json.find_first_not_of(" \t\r\n", i + 1);
  if (i == std::string::npos) {
    return "";
  }
  std::string out;
  if (json[i] == '"') {
    for (size_t j = i + 1; j < json.size() && json[j] != '"'; j++) {
      if (json[j] == '\\' && j + 1 < json.size()) {
        j++;
      }
      out += json[j];
    }
    return out;
  }
  const size_t end = json.find_first_of(",} \t\r\n", i);
  return json.substr(i, end == std::string::npos ? std::string::npos : end - i);
}

// Xenia's settings a project may set (waterbox.config declares them under
// these names): 's' a word, 'b' true or false, 'i' a whole number.
struct XeniaSetting {
  const char* name;
  char kind;
};
const XeniaSetting kXeniaSettings[] = {
    {"clear_memory_page_state", 'b'},
    {"occlusion_query_fake_lower_threshold", 'i'},
    {"occlusion_query_fake_upper_threshold", 'i'},
    {"depth_bias_shader_offset", 'b'},
    {"gamma_render_target_as_unorm16", 'b'},
    {"gpu_allow_invalid_upload_range", 'b'},
    {"force_depth_clamp", 'b'},
    {"render_target_path_vulkan", 's'},
    {"break_on_unimplemented_instructions", 'b'},
    {"protect_zero", 'b'},
    {"ignore_offset_for_ranged_allocations", 'b'},
    {"scribble_heap", 'b'},
    {"scribble_heap_value", 'i'},
    {"xma_decoder", 's'},
    {"use_dedicated_xma_thread", 'b'},
    {"cl", 's'},
    {"launch_module", 's'},
    {"mount_scratch", 'b'},
    // these make the console hold what the graphics card draws; applied
    // after the core's own choices of them (xenia_driver.cc)
    {"readback_resolve", 's'},
    {"occlusion_query", 's'},
    {"occlusion_query_full_counters", 'b'},
    {"readback_memexport", 'b'},
};

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

// Every name in a slot of the project's slot map, in order.
std::vector<std::string> slot_all(const std::string& slots, const char* id) {
  std::vector<std::string> out;
  const std::string k = std::string("\"") + id + "\"";
  const auto at = slots.find(k);
  if (at == std::string::npos) {
    return out;
  }
  size_t i = slots.find('[', at + k.size());
  if (i == std::string::npos) {
    return out;
  }
  // string after string: a name may hold brackets and commas of its own
  for (i++; i < slots.size();) {
    i = slots.find_first_not_of(" \t\r\n,", i);
    if (i == std::string::npos || slots[i] != '"') {
      break;
    }
    std::string name;
    size_t j = i + 1;
    for (; j < slots.size() && slots[j] != '"'; j++) {
      if (slots[j] == '\\' && j + 1 < slots.size()) {
        j++;
      }
      name += slots[j];
    }
    out.push_back(name);
    i = j + 1;
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

// For the core's own tests, with no game: see check_watched_views. The text
// ends with "views ok" when every write was right.
CORE_EXPORT const char* CheckWatchedViews(void) {
  static std::string report;
  report.clear();
  const bool good = chimera_xenia::check_watched_views(report);
  report += good ? "views ok\n" : "views WRONG\n";
  return report.c_str();
}

// For the core's own tests, with no game: see check_turns. The text ends
// with "turns ok" when the thread that read the clock gave way.
CORE_EXPORT const char* CheckTurns(void) {
  static std::string report;
  report.clear();
  const bool good = chimera_xenia::check_turns(report);
  report += good ? "turns ok\n" : "turns WRONG\n";
  return report.c_str();
}

#ifdef CHIMERA_GUEST
// The Vulkan bridge: the host's dispatcher, handed over before Init. Without
// it there is no device in the sandbox and only the renderer that draws
// nothing can be had.
CORE_EXPORT void SetVulkanBridge(uint64_t addr) {
  g_bridge = chimera_vk_install(reinterpret_cast<chimera_vk_bridge_fn>(static_cast<uintptr_t>(addr)));
}
#endif

// For the core's own tests: put disc n in, as the game does when it asks.
CORE_EXPORT int InsertDisc(int32_t n) { return chimera_xenia::insert_disc(static_cast<uint32_t>(n)) ? 1 : 0; }

namespace {
// The game: the first file of the "game" slot, or the file opened on its own.
std::string game_file(const std::string& slots) {
  std::string name = mounted(slot_first(slots, "game"));
  if (name.empty()) {
    name = read_file("rom.name");
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
      name.pop_back();
    }
    name = mounted(name);
  }
  return name;
}
}  // namespace

// What the core suggests for these files before any machine exists (the
// engine's ce_suggest_settings): called INSTEAD of Init, on the same mounts.
CORE_EXPORT const char* SuggestSettings(void) {
  static std::string answer;
  answer = chimera_xenia::suggest_settings(game_file(read_file("slots")));
  return answer.c_str();
}

CORE_EXPORT int Init(void) {
  const std::string slots = read_file("slots");
  const std::string name = game_file(slots);
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
  g_pads = options.pads;
  options.log = json_string(settings, "log") == "1";
  options.trace_schedule = json_string(settings, "trace_schedule") == "1";
  options.instruction_ps = std::atoll(json_string(settings, "instruction_ps").c_str());
  // CPU Speed: millions of instructions a second for the whole console
  const std::string cpu_speed = json_string(settings, "cpu_speed");
  if (!cpu_speed.empty()) {
    const long long mips = std::atoll(cpu_speed.c_str());
    if (mips != 500 && mips != 1000 && mips != 2000 && mips != 4000) {
      g_error = "no such CPU speed: " + cpu_speed;
      return 0;
    }
    options.instruction_ps = 1000000 / mips;
  }
  options.yield_ns = std::atoll(json_string(settings, "yield_ns").c_str());
  // Arcade Games: "full" (and no word at all) is a game that was bought
  const std::string arcade = json_string(settings, "arcade_license");
  if (!arcade.empty() && arcade != "full" && arcade != "trial") {
    g_error = "no such Arcade Games setting: " + arcade;
    return 0;
  }
  options.arcade_bought = arcade != "trial";
  // Xenia's own settings, under Xenia's names (chimera#289): each becomes the
  // line Xenia's configuration file would have.
  for (const XeniaSetting& x : kXeniaSettings) {
    const std::string value = json_raw(settings, x.name);
    if (value.empty()) {
      continue;
    }
    std::string line;
    if (x.kind == 's') {
      line = "\"";
      for (const char c : value) {
        line += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
      }
      line += "\"";
    } else if (x.kind == 'b') {
      if (value != "true" && value != "false") {
        g_error = std::string("the setting ") + x.name + " is true or false, not " + value;
        return 0;
      }
      line = value;
    } else {
      char* end = nullptr;
      std::strtoll(value.c_str(), &end, 10);
      if (end == value.c_str() || *end) {
        g_error = std::string("the setting ") + x.name + " is a whole number, not " + value;
        return 0;
      }
      line = value;
    }
    options.xenia_settings.emplace_back(x.name, line);
  }
  for (const std::string& package : slot_all(slots, "content")) {
    options.content.push_back(mounted(package));
  }
  options.savedata = mounted(slot_first(slots, "savedata"));
  // the game slot's later files are the game's other discs, in order
  const std::vector<std::string> discs = slot_all(slots, "game");
  for (size_t i = 1; i < discs.size(); i++) {
    options.discs.push_back(mounted(discs[i]));
  }
  if (options.renderer == "vulkan" && !g_bridge) {
    g_error = "the Vulkan renderer needs a device, and none was handed over. Either this machine has no Vulkan, or "
              "this Chimera is older than this core: its Vulkan bridge has fewer commands than the core was built "
              "with, and a newer Chimera is needed";
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

// Which of the declared controls this console has. The package declares four
// controllers; one that is not plugged in gets no column in the input log.
// Fifteen buttons and AXES_PER_PORT axes a controller, in declared order.
CORE_EXPORT int IsButtonActive(int32_t index) { return index >= 0 && index / 15 < g_pads ? 1 : 0; }
CORE_EXPORT int IsAxisActive(int32_t index) {
  return index >= 0 && index / chimera_xenia::AXES_PER_PORT < g_pads ? 1 : 0;
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
CORE_EXPORT uint64_t GetInstructionCount(void) { return chimera_xenia::instructions(); }
// The same by thread, as text ("<id> <count> <name>" a line): where the
// machine's time goes, for a person.
CORE_EXPORT const char* DebugInstructionsByThread(void) {
  static std::string text;
  text = chimera_xenia::instructions_by_thread();
  return text.c_str();
}
CORE_EXPORT uint64_t GetGpuCounter(void) { return chimera_xenia::gpu_counter(); }
// A digest of the console's 512 MiB, for the gate.
CORE_EXPORT uint64_t GetRamDigest(void) { return chimera_xenia::ram_digest(); }
