// The machine behind the core's exports (xenia_driver.h): Xenia set up with no
// window, a pad driver that answers with what the frontend set, a sound
// driver that keeps what the program plays, and the frame.
//
// The keystroke part of the pad driver (which virtual key a button is, the
// order changes are reported in, the repeat) follows Xenia's SDL driver,
// src/xenia/hid/sdl/sdl_input_driver.cc (BSD-3-Clause, Copyright 2020 Ben
// Vanik and the Xenia authors), with the host's pad taken out of it.
#include "xenia_driver.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

#include "machine_settings.h"
#include "memfs.h"
#include "third_party/zlib-ng/zlib-ng.h"

#ifdef CHIMERA_GUEST
#include "vk-bridge.h"
#endif

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_system.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/chimera_vsched.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/null/null_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/vfs/devices/host_path_device.h"

DECLARE_path(log_file);
DECLARE_bool(log_to_stdout);
// Upstream defines it in its desktop app, which this core does not build.
DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");

namespace chimera_xenia {
namespace {

// the status codes are macros that cast to these
using xe::X_RESULT;
using xe::X_STATUS;

struct Pad {
  bool connected = false;
  xe::hid::X_INPUT_STATE state = {};
  // what the program has been told of, as keystrokes
  uint64_t told = 0;
  int repeat = 0;  // 0 idle, 1 waiting for the delay, 2 repeating
  uint8_t repeat_button = 0;
  uint32_t repeat_time = 0;
};

// A frame of sound is at most a few of the console's 256-sample frames; a
// second's worth is room to spare for a program that plays in bursts.
constexpr size_t AUDIO_CAP_PAIRS = 48000;

struct Machine {
  std::unique_ptr<xe::Emulator> emulator;
  // the files the project's read-only packages became in the storage, and
  // their headers: the project's, not save data, and never exported
  std::vector<std::string> brought;
  Pad pads[PORTS];
  bool input_read = false;
  uint64_t frames = 0;
  // what the program has played since the last frame ended, mixed; and the
  // last frame's, for the frontend
  std::vector<int32_t> mix;
  std::vector<int16_t> audio;
  int audio_pairs = 0;
  // whether the last frame ended with the renderer's thread parked where
  // the renderer can be made again
  bool renderer_quiet = false;
  // the device the renderer's objects were made on; zero natively
  uint64_t device = 0;
  // make the renderer again: because it was asked, or because the device is
  // another one (and then the old one answers nothing)
  bool remake = false;
  bool remake_device_gone = false;
  // the last picture read back
  bool vulkan = false;
  bool rendering = true;
  std::vector<uint32_t> video;
  int video_width = 0, video_height = 0;
};
Machine* g_machine = nullptr;

// The machine's calendar, for its filesystem: it is switched on at
// 2010-01-01 00:00:00 UTC, as the scheduler has it.
int64_t machine_seconds() { return 1262304000 + xe::chimera::VirtualNanoseconds() / 1000000000; }

// Which device the renderer's calls land on: the bridge's context in the
// sandbox, where every load makes another; natively there is only ever one.
uint64_t device_now() {
#ifdef CHIMERA_GUEST
  return chimera_vk_context_id();
#else
  return 0;
#endif
}

// ---- the pads -------------------------------------------------------------

constexpr int16_t THUMB_THRESHOLD = 0x4E00;
constexpr uint8_t TRIGGER_THRESHOLD = 0x1F;
constexpr uint32_t REPEAT_DELAY_MS = 400;
constexpr uint32_t REPEAT_RATE_MS = 100;

uint64_t analog_keys(const xe::hid::X_INPUT_GAMEPAD& pad) {
  uint64_t f = 0;
  f |= static_cast<uint64_t>(pad.left_trigger > TRIGGER_THRESHOLD) << 16;
  f |= static_cast<uint64_t>(pad.right_trigger > TRIGGER_THRESHOLD) << 17;
  int16_t x = pad.thumb_lx, y = pad.thumb_ly;
  for (size_t i = 0; i <= 8; i += 8) {
    uint64_t u = y > THUMB_THRESHOLD;
    uint64_t d = y < ~THUMB_THRESHOLD;
    uint64_t r = x > THUMB_THRESHOLD;
    uint64_t l = x < ~THUMB_THRESHOLD;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);
    x = pad.thumb_rx;
    y = pad.thumb_ry;
  }
  return f;
}

class PadDriver final : public xe::hid::InputDriver {
 public:
  PadDriver() : InputDriver(nullptr, 0) {}

  xe::X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  xe::X_RESULT GetCapabilities(uint32_t user, uint32_t flags, xe::hid::X_INPUT_CAPABILITIES* out) override {
    if (user >= PORTS || !out) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    if (!g_machine->pads[user].connected) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    // a wired pad with every button; the guide button is the console's own
    std::memset(out, 0, sizeof(*out));
    out->type = 0x01;      // a gamepad
    out->sub_type = 0x01;  // of the ordinary kind
    out->flags = 0;
    out->gamepad.buttons = 0xF3FF;
    out->gamepad.left_trigger = 0xFF;
    out->gamepad.right_trigger = 0xFF;
    out->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
    out->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
    out->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
    out->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
    out->vibration.left_motor_speed = 0xFFFFu;
    out->vibration.right_motor_speed = 0xFFFFu;
    return X_ERROR_SUCCESS;
  }

  xe::X_RESULT GetState(uint32_t user, xe::hid::X_INPUT_STATE* out) override {
    if (user >= PORTS) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    Pad& pad = g_machine->pads[user];
    if (!pad.connected) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    g_machine->input_read = true;
    if (out) {
      std::memcpy(out, &pad.state, sizeof(*out));
    }
    return X_ERROR_SUCCESS;
  }

  xe::X_RESULT SetState(uint32_t user, xe::hid::X_INPUT_VIBRATION* vibration) override {
    if (user >= PORTS) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    return g_machine->pads[user].connected ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  xe::X_RESULT GetKeystroke(uint32_t users, uint32_t flags, xe::hid::X_INPUT_KEYSTROKE* out) override {
    const bool any = users == xe::XUserIndexAny;
    if ((users >= PORTS && !any) || !out) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    // The order of this list is the order changes are reported in when
    // several buttons change at once.
    static constexpr std::array<xe::ui::VirtualKey, 34> keys = {
        xe::ui::VirtualKey::kXInputPadDpadUp,
        xe::ui::VirtualKey::kXInputPadDpadDown,
        xe::ui::VirtualKey::kXInputPadDpadLeft,
        xe::ui::VirtualKey::kXInputPadDpadRight,
        xe::ui::VirtualKey::kXInputPadStart,
        xe::ui::VirtualKey::kXInputPadBack,
        xe::ui::VirtualKey::kXInputPadLThumbPress,
        xe::ui::VirtualKey::kXInputPadRThumbPress,
        xe::ui::VirtualKey::kXInputPadLShoulder,
        xe::ui::VirtualKey::kXInputPadRShoulder,
        xe::ui::VirtualKey::kXInputPadGuide,
        xe::ui::VirtualKey::kNone,
        xe::ui::VirtualKey::kXInputPadA,
        xe::ui::VirtualKey::kXInputPadB,
        xe::ui::VirtualKey::kXInputPadX,
        xe::ui::VirtualKey::kXInputPadY,
        // made from the triggers and the sticks
        xe::ui::VirtualKey::kXInputPadLTrigger,
        xe::ui::VirtualKey::kXInputPadRTrigger,
        xe::ui::VirtualKey::kXInputPadLThumbUp,
        xe::ui::VirtualKey::kXInputPadLThumbDown,
        xe::ui::VirtualKey::kXInputPadLThumbRight,
        xe::ui::VirtualKey::kXInputPadLThumbLeft,
        xe::ui::VirtualKey::kXInputPadLThumbUpLeft,
        xe::ui::VirtualKey::kXInputPadLThumbUpRight,
        xe::ui::VirtualKey::kXInputPadLThumbDownRight,
        xe::ui::VirtualKey::kXInputPadLThumbDownLeft,
        xe::ui::VirtualKey::kXInputPadRThumbUp,
        xe::ui::VirtualKey::kXInputPadRThumbDown,
        xe::ui::VirtualKey::kXInputPadRThumbRight,
        xe::ui::VirtualKey::kXInputPadRThumbLeft,
        xe::ui::VirtualKey::kXInputPadRThumbUpLeft,
        xe::ui::VirtualKey::kXInputPadRThumbUpRight,
        xe::ui::VirtualKey::kXInputPadRThumbDownRight,
        xe::ui::VirtualKey::kXInputPadRThumbDownLeft,
    };
    for (uint32_t user = any ? 0 : users; user < (any ? uint32_t(PORTS) : users + 1); user++) {
      Pad& pad = g_machine->pads[user];
      if (!pad.connected) {
        if (any) {
          continue;
        }
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
      g_machine->input_read = true;
      const uint64_t now_keys = uint16_t(pad.state.gamepad.buttons) | analog_keys(pad.state.gamepad);
      // the machine's clock, so a held button repeats on the machine's time
      const uint32_t now = xe::Clock::QueryGuestUptimeMillis();
      if (pad.repeat == 1 && pad.repeat_time + REPEAT_DELAY_MS < now) {
        pad.repeat = 2;
      }
      if (pad.repeat == 2 && pad.repeat_time + REPEAT_RATE_MS < now) {
        pad.repeat_time = now;
        out->virtual_key = uint16_t(keys.at(pad.repeat_button));
        out->unicode = 0;
        out->user_index = static_cast<uint8_t>(user);
        out->hid_code = 0;
        out->flags = xe::hid::X_INPUT_KEYSTROKE_KEYDOWN | xe::hid::X_INPUT_KEYSTROKE_REPEAT;
        return X_ERROR_SUCCESS;
      }
      const uint64_t changed = now_keys ^ pad.told;
      if (!changed) {
        continue;
      }
      // Releases first, then presses: a stick going from up-left to left
      // reports up-left released before left pressed, as the console does.
      for (int pass = 0; pass < 2; pass++) {
        for (uint8_t i = 0; i < uint8_t(keys.size()); i++) {
          const uint64_t bit = uint64_t(1) << i;
          if (!(changed & bit) || keys[i] == xe::ui::VirtualKey::kNone) {
            continue;
          }
          const bool pressed = (now_keys & bit) != 0;
          out->virtual_key = uint16_t(keys[i]);
          out->unicode = 0;
          out->user_index = static_cast<uint8_t>(user);
          out->hid_code = 0;
          if (pass == 0 && !pressed) {
            out->flags = xe::hid::X_INPUT_KEYSTROKE_KEYUP;
            pad.told &= ~bit;
            pad.repeat = 0;
            return X_ERROR_SUCCESS;
          }
          if (pass == 1 && pressed) {
            out->flags = xe::hid::X_INPUT_KEYSTROKE_KEYDOWN;
            pad.told |= bit;
            pad.repeat = 1;
            pad.repeat_button = i;
            pad.repeat_time = now;
            return X_ERROR_SUCCESS;
          }
        }
      }
      // a change in a bit that is no key (bit 11) is forgotten, or it would
      // be looked at for ever
      pad.told = (pad.told & ~changed) | (now_keys & changed);
    }
    return X_ERROR_EMPTY;
  }

  xe::hid::InputType GetInputType() const override { return xe::hid::InputType::Controller; }
};

// ---- the sound ------------------------------------------------------------

int32_t to_s16(float v) {
  // not a number is silence; the rest is clamped before it is an integer
  if (!(v == v)) {
    return 0;
  }
  const float scaled = v * 32767.0f;
  if (scaled >= 32767.0f) {
    return 32767;
  }
  if (scaled <= -32768.0f) {
    return -32768;
  }
  return static_cast<int32_t>(scaled);
}

// A client's driver: what the program hands the console's sound output, 256
// samples of six channels at 48 kHz, big-endian floats, a channel after
// another. Each client writes at its own place in the frame's mix.
class MixDriver final : public xe::apu::AudioDriver {
 public:
  explicit MixDriver(xe::threading::Semaphore* semaphore) : semaphore_(semaphore) {}
  bool Initialize() override { return true; }
  void Shutdown() override {}
  void SubmitFrame(float* samples) override {
    Machine& m = *g_machine;
    constexpr size_t n = kChannelSamplesDefault;
    if (at_ + n <= AUDIO_CAP_PAIRS) {
      if (m.mix.size() < (at_ + n) * 2) {
        m.mix.resize((at_ + n) * 2, 0);
      }
      for (size_t i = 0; i < n; i++) {
        // front left, front right, centre, low frequency, back left, back
        // right: the centre goes to both sides, the low frequency nowhere
        const float fl = xe::byte_swap(samples[0 * n + i]);
        const float fr = xe::byte_swap(samples[1 * n + i]);
        const float fc = xe::byte_swap(samples[2 * n + i]);
        const float bl = xe::byte_swap(samples[4 * n + i]);
        const float br = xe::byte_swap(samples[5 * n + i]);
        m.mix[(at_ + i) * 2] += to_s16((fl + bl + fc * 0.5f) * (1.0f / 2.5f));
        m.mix[(at_ + i) * 2 + 1] += to_s16((fr + br + fc * 0.5f) * (1.0f / 2.5f));
      }
      at_ += n;
    }
    // there is always room for the next one: nothing is waiting to be played
    if (semaphore_) {
      semaphore_->Release(1, nullptr);
    }
  }
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float volume) override {}

  // the frame took what was mixed
  void taken(size_t pairs) { at_ = at_ > pairs ? at_ - pairs : 0; }

 private:
  xe::threading::Semaphore* semaphore_;
  size_t at_ = 0;
};

// A driver that is not a client's (the media player's): heard by nobody.
class SilentDriver final : public xe::apu::AudioDriver {
 public:
  explicit SilentDriver(xe::threading::Semaphore* semaphore) : semaphore_(semaphore) {}
  bool Initialize() override { return true; }
  void Shutdown() override {}
  void SubmitFrame(float* samples) override {
    if (semaphore_) {
      semaphore_->Release(1, nullptr);
    }
  }
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float volume) override {}

 private:
  xe::threading::Semaphore* semaphore_;
};

std::vector<MixDriver*> g_mix_drivers;

class SoundSystem final : public xe::apu::AudioSystem {
 public:
  explicit SoundSystem(xe::cpu::Processor* processor) : AudioSystem(processor) {}
  std::string name() const override { return "Chimera"; }
  xe::X_STATUS CreateDriver(size_t index, xe::threading::Semaphore* semaphore,
                            xe::apu::AudioDriver** out_driver) override {
    auto* driver = new MixDriver(semaphore);
    g_mix_drivers.push_back(driver);
    *out_driver = driver;
    return X_STATUS_SUCCESS;
  }
  xe::apu::AudioDriver* CreateDriver(xe::threading::Semaphore* semaphore, uint32_t frequency, uint32_t channels,
                                     bool need_format_conversion) override {
    return new SilentDriver(semaphore);
  }
  void DestroyDriver(xe::apu::AudioDriver* driver) override {
    for (size_t i = 0; i < g_mix_drivers.size(); i++) {
      if (g_mix_drivers[i] == driver) {
        g_mix_drivers.erase(g_mix_drivers.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    delete driver;
  }
};

int16_t clamp16(int32_t v) { return static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// One of Xenia's settings by its name, from its value as Xenia's
// configuration file writes it - the way Xenia itself reads a game's own
// configuration file. False with the reason for a name Xenia does not have or
// a value of the wrong kind.
bool apply_xenia_setting(const std::string& name, const std::string& value, std::string& error) {
  const auto found = cvar::ConfigVars ? cvar::ConfigVars->find(name) : std::map<std::string, cvar::IConfigVar*>::iterator();
  if (!cvar::ConfigVars || found == cvar::ConfigVars->end()) {
    error = "Xenia has no setting called " + name;
    return false;
  }
  toml::table line;
  try {
    line = toml::parse(name + " = " + value);
  } catch (const toml::parse_error&) {
    error = "the value of Xenia's setting " + name + " cannot be read: " + value;
    return false;
  }
  // a value of the wrong kind is passed over, and only noted in a list
  const size_t mismatches = cvar::config_type_mismatch_warnings ? cvar::config_type_mismatch_warnings->size() : 0;
  found->second->LoadConfigValue(line.get(name));
  if (cvar::config_type_mismatch_warnings && cvar::config_type_mismatch_warnings->size() != mismatches) {
    error = "Xenia's setting " + name + " does not take " + value;
    return false;
  }
  return true;
}

}  // namespace

namespace {
// The console switched on, with nothing in it yet.
bool switch_on(const Options& options, std::string& error) {
  if (g_machine) {
    error = "the machine is already on";
    return false;
  }
  if (options.renderer != "null" && options.renderer != "vulkan") {
    error = "no such renderer: " + options.renderer;
    return false;
  }
  if (!apply_machine_settings(error)) {
    return false;
  }
  // A logger that was never started does not drop a line: it waits for room
  // that never comes. Its file is where the machine was started, when a file
  // can be made there (in the sandbox none can, and nothing is written);
  // standard output is the runners' and is left alone unless asked for.
  cvars::log_file = "chimera.log";
  cvars::log_to_stdout = options.log;
  // What the console's memory holds must not depend on the card that draws.
  // A program asks how many pixels of something were visible (an occlusion
  // query) and the answer is written into its memory. Xenia's default asks
  // the real card and writes the answer "when the GPU catches up": another
  // number on another card, and another moment in another run - a run that
  // only saved states was no longer the run that did not. So the answer is
  // the made-up one, the same everywhere, with no renderer too. Nothing else
  // the card draws is read back either: these two are Xenia's defaults, said
  // here so that a change of default upstream cannot change the machine.
  xe::gpu::SetZPDMode("fake");
  xe::gpu::SetReadbackResolveMode("none");
  // Then the project's own choices of Xenia's settings, each read as its
  // line in Xenia's configuration file would be (chimera#289).
  for (const auto& [name, value] : options.xenia_settings) {
    if (!apply_xenia_setting(name, value, error)) {
      return false;
    }
  }
  xe::chimera::g_trace_schedule = options.trace_schedule;
  xe::chimera::SetCosts(options.instruction_ps, options.yield_ns);
  xe::InitializeLogging("chimera");

  g_machine = new Machine();
  Machine& m = *g_machine;
  m.audio.resize(AUDIO_CAP_PAIRS * 2);
  for (int i = 0; i < PORTS; i++) {
    m.pads[i].connected = i < options.pads;
  }

  // The emulator's own roots - its storage, its content (profiles, saved
  // games), its cache - are in the machine's own filesystem, which is memory:
  // the same in a sandbox and out of one, kept by every savestate, and empty
  // when the machine is switched on.
  chimera::memfs::set_clock(machine_seconds);
  const std::filesystem::path work = std::string(chimera::memfs::ROOT);
  for (const char* directory : {"storage", "content", "cache"}) {
    chimera::memfs::mkdirs((work / directory).string());
  }
  m.emulator = std::make_unique<xe::Emulator>("", work / "storage", work / "content", work / "cache");
  const bool vulkan = options.renderer == "vulkan";
  m.vulkan = vulkan;
  xe::X_STATUS status = m.emulator->Setup(
      nullptr, nullptr, true,
      [](xe::cpu::Processor* processor) -> std::unique_ptr<xe::apu::AudioSystem> {
        return std::make_unique<SoundSystem>(processor);
      },
      [vulkan]() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
        if (vulkan) {
          return std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
        }
        return std::make_unique<xe::gpu::null::NullGraphicsSystem>();
      },
      [](xe::ui::Window* window) {
        std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
        drivers.emplace_back(std::make_unique<PadDriver>());
        return drivers;
      });
  if (XFAILED(status)) {
    char text[64];
    std::snprintf(text, sizeof(text), "the emulator did not set up (%08X)", status);
    error = text;
    return false;
  }
  // The console's scratch partition, for a game that keeps something there
  // (an instant replay, a first load): upstream's desktop app mounts it when
  // told to, and this core does not use that app, so it is here, in the
  // machine's own filesystem (mount_scratch, chimera#289).
  if (cvars::mount_scratch) {
    const std::filesystem::path scratch = work / "storage" / "scratch";
    chimera::memfs::mkdirs(scratch.string());
    auto device = std::make_unique<xe::vfs::HostPathDevice>("\\SCRATCH", scratch, false);
    if (!device->Initialize() || !m.emulator->file_system()->RegisterDevice(std::move(device))) {
      error = "the console's scratch partition could not be mounted";
      return false;
    }
    m.emulator->file_system()->RegisterSymbolicLink("scratch:", "\\SCRATCH");
  }
  // One profile, always the same one, always signed in on the first pad: a
  // game that will not save - or will not start - without somebody signed in
  // has somebody. (Upstream's own fixed identity for a default profile.)
  if (auto* profiles = m.emulator->kernel_state()->xam_state()->profile_manager()) {
    if (!profiles->CreateProfile("Player", true, true)) {
      error = "the console's profile could not be made";
      return false;
    }
  }

  m.device = device_now();
  return true;
}
}  // namespace

namespace {
// Whether a program is a container - the one file an Xbox Live Arcade game
// is - and not a disc image: by how the file begins, as the emulator tells.
bool is_container(const std::string& program) {
  char magic[4] = {};
  if (FILE* f = std::fopen(program.c_str(), "rb")) {
    const size_t n = std::fread(magic, 1, sizeof magic, f);
    std::fclose(f);
    if (n != sizeof magic) {
      return false;
    }
  }
  return !std::memcmp(magic, "LIVE", 4) || !std::memcmp(magic, "PIRS", 4) || !std::memcmp(magic, "CON ", 4);
}
}  // namespace

namespace {
std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Whether the console now lists the package where a game asks for it: the
// same listing XamContentCreateEnumerator makes, for the package's title and
// kind, on the hard drive, for its owner (a saved game is the signed-in
// player's, other content everybody's).
bool listed(Machine& m, uint64_t xuid, uint32_t title, const xe::Emulator::ContentInstallEntry& entry,
            const std::string& name, std::string& error) {
  if (entry.content_type_ == xe::XContentType::kSavedGame) {
    if (const auto profile = m.emulator->kernel_state()->xam_state()->profile_manager()->GetProfile(uint8_t(0))) {
      xuid = profile->xuid();
    }
  }
  const auto found = m.emulator->kernel_state()->content_manager()->ListContent(
      static_cast<uint32_t>(xe::kernel::xam::DummyDeviceId::HDD), xuid, title, entry.content_type_,
      static_cast<xe::kernel::xam::XContentFlag>(0));
  for (const auto& data : found) {
    if (data.file_name() == entry.filename_.string()) {
      XELOGI("chimera: {} is in the console's storage: title {:08X}, kind {:08X}", name, title,
             static_cast<uint32_t>(entry.content_type_));
      return true;
    }
  }
  char what[64];
  std::snprintf(what, sizeof what, "title %08X, kind %08X", title, static_cast<uint32_t>(entry.content_type_));
  error = name + " was put in the console's storage, but the console does not list it there (" + what + ")";
  return false;
}

// One package of the Content slot into the console's storage, where Xenia's
// Install Content puts it (chimera issue 271): under content/<profile or
// 0>/<title>/<kind>/, with its header beside. A package the console treats
// as read-only - add-on content, a title update - is kept as the one file it
// is, and here that file is the project's own, served in place: nothing is
// copied, and a savestate does not carry it. One a player can write (a saved
// game) is unpacked into the storage, as Xenia does, and is then the
// machine's to change.
bool install_package(Machine& m, const std::string& host, std::string& error) {
  const std::string name = base_name(host);
  xe::Emulator::ContentInstallEntry entry{std::filesystem::path(host)};
  if (XFAILED(m.emulator->ProcessContentPackageHeader(host, entry))) {
    error = name + " is not an Xbox 360 package (" + entry.installation_error_message_ +
            "): the Content slot takes add-on content, title updates and saved games, each one file as the console keeps it";
    return false;
  }
  const auto type = entry.content_type_;
  if (type != xe::XContentType::kMarketplaceContent && type != xe::XContentType::kInstaller &&
      type != xe::XContentType::kSavedGame) {
    char kind[16];
    std::snprintf(kind, sizeof kind, "%08X", static_cast<uint32_t>(type));
    error = name + " is a package of kind " + kind +
            ", which the Content slot does not take: it takes add-on content (00000002), title updates (000B0000) "
            "and saved games (00000001). A game itself goes in the Game slot";
    return false;
  }
  auto* contents = m.emulator->kernel_state()->content_manager();
  auto package = contents->OpenPackage(host);
  if (!package || !package->GetContainerHeader()) {
    error = name + " could not be opened as a package";
    return false;
  }
  if (package->GetContainerMetadata()->volume_type.get() != xe::kernel::xam::XContentVolumeType::kStfs) {
    error = name + " is a package of the kind that is a file beside a folder of data (SVOD), which is not supported";
    return false;
  }
  const uint64_t owner = package->GetContainerMetadata()->profile_id;
  const uint32_t title = package->GetContainerMetadata()->execution_info.title_id;
  const std::string folder = entry.data_installation_path_.string();
  const std::string file = (entry.data_installation_path_ / entry.filename_).string();
  if (!chimera::memfs::mkdirs(folder) ||
      !chimera::memfs::mkdirs(entry.header_installation_path_.parent_path().string()) ||
      XFAILED(xe::kernel::xam::ContentManager::ExtractContentHeader(entry.header_installation_path_,
                                                                    *package->GetContainerHeader()))) {
    error = name + " could not be put in the console's storage";
    return false;
  }
  if (package->IsReadOnly()) {
    if (!chimera::memfs::graft(file, host)) {
      error = name + " could not be put in the console's storage";
      return false;
    }
    m.brought.push_back(file);
    m.brought.push_back(entry.header_installation_path_.string());
    return listed(m, owner, title, entry, name, error);
  }
  package.reset();
  auto* mounted = contents->OpenAndMountPackage(host, "");
  uint64_t progress = 0;
  const xe::X_STATUS status =
      mounted ? contents->InstallContentPackage(mounted, file, entry.header_installation_path_, progress) : X_STATUS_UNSUCCESSFUL;
  if (mounted) {
    contents->CloseContentByDeviceName(mounted->GetDevicePath());
  }
  if (XFAILED(status)) {
    error = name + " could not be unpacked into the console's storage";
    return false;
  }
  return listed(m, owner, title, entry, name, error);
}

// The Save data slot: a zip as Emulator > Export Save Data wrote it, every
// entry content/..., written back into the console's storage before the game
// starts. Stored and deflated entries, as zip tools write them.
uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

bool load_savedata(const std::string& host, std::string& error) {
  const std::string name = base_name(host);
  std::vector<uint8_t> zip;
  if (FILE* f = std::fopen(host.c_str(), "rb")) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
      zip.insert(zip.end(), buf, buf + n);
    }
    std::fclose(f);
  }
  // the end of the central directory: the last 22 bytes and a comment
  size_t end = std::string::npos;
  for (size_t i = zip.size() >= 22 ? zip.size() - 22 : 0; zip.size() >= 22; i--) {
    if (le32(&zip[i]) == 0x06054b50u) {
      end = i;
      break;
    }
    if (i == 0 || zip.size() - i > 22 + 65535) {
      break;
    }
  }
  if (end == std::string::npos) {
    error = name + " is not a zip: the Save data slot takes the zip Emulator > Export Save Data writes";
    return false;
  }
  const uint16_t count = le16(&zip[end + 10]);
  size_t at = le32(&zip[end + 16]);
  for (uint16_t k = 0; k < count; k++) {
    if (at + 46 > zip.size() || le32(&zip[at]) != 0x02014b50u) {
      error = name + " is a damaged zip";
      return false;
    }
    const uint16_t method = le16(&zip[at + 10]);
    const uint32_t packed = le32(&zip[at + 20]), size = le32(&zip[at + 24]);
    const uint16_t name_length = le16(&zip[at + 28]), extra = le16(&zip[at + 30]), comment = le16(&zip[at + 32]);
    const uint32_t local = le32(&zip[at + 42]);
    const std::string entry(reinterpret_cast<const char*>(&zip[at + 46]), name_length);
    at += 46 + name_length + extra + comment;
    if (!entry.empty() && entry.back() == '/') {
      continue;
    }
    if (entry.rfind("content/", 0) != 0 || entry.find("..") != std::string::npos) {
      error = name + " holds " + entry +
              ", which is not the console's storage: the zip Emulator > Export Save Data writes has every entry under content/";
      return false;
    }
    if (local + 30 > zip.size() || le32(&zip[local]) != 0x04034b50u) {
      error = name + " is a damaged zip";
      return false;
    }
    const size_t data = local + 30 + le16(&zip[local + 26]) + le16(&zip[local + 28]);
    if (data + packed > zip.size()) {
      error = name + " is a damaged zip";
      return false;
    }
    std::vector<uint8_t> bytes(size);
    if (method == 0 && packed == size) {
      std::memcpy(bytes.data(), &zip[data], size);
    } else if (method == 8) {
      zng_stream z{};
      if (zng_inflateInit2(&z, -15) != Z_OK) {
        error = name + ": " + entry + " could not be unpacked";
        return false;
      }
      z.next_in = &zip[data];
      z.avail_in = packed;
      z.next_out = bytes.data();
      z.avail_out = size;
      const int r = zng_inflate(&z, Z_FINISH);
      zng_inflateEnd(&z);
      if (r != Z_STREAM_END || z.total_out != size) {
        error = name + ": " + entry + " could not be unpacked";
        return false;
      }
    } else {
      error = name + ": " + entry + " is packed in a way this core does not read (stored and deflated are read)";
      return false;
    }
    const std::string path = std::string(chimera::memfs::ROOT) + "/" + entry;
    if (!chimera::memfs::mkdirs(path.substr(0, path.rfind('/'))) || !chimera::memfs::put(path, std::move(bytes))) {
      error = name + ": " + entry + " could not be written into the console's storage";
      return false;
    }
  }
  return true;
}
}  // namespace

bool boot(const std::string& program, const Options& options, std::string& error) {
  if (!switch_on(options, error)) {
    return false;
  }
  Machine& m = *g_machine;
  // What the project brings besides the game, before the game starts: the
  // saved storage first, then the packages on top of it.
  if (!options.savedata.empty() && !load_savedata(options.savedata, error)) {
    return false;
  }
  for (const std::string& package : options.content) {
    if (!install_package(m, package, error)) {
      return false;
    }
  }
  m.emulator->chimera_discs.push_back(program);
  for (const std::string& disc : options.discs) {
    m.emulator->chimera_discs.push_back(disc);
  }
  // The first licence bit is what an Arcade game reads as "bought". A disc
  // game that asks is answered with none, as before there was this setting.
  cvars::license_mask = is_container(program) && options.arcade_bought ? 1 : 0;
  const xe::X_STATUS status = m.emulator->LaunchPath(program);
  if (XFAILED(status)) {
    char text[64];
    std::snprintf(text, sizeof(text), "the program did not launch (%08X)", status);
    error = program + ": " + text;
    return false;
  }
  return true;
}

bool insert_disc(uint32_t n) { return g_machine && XSUCCEEDED(g_machine->emulator->ChimeraInsertDisc(n)); }

void set_pad(int port, uint16_t buttons, const int32_t* axes) {
  if (!g_machine || port < 0 || port >= PORTS) {
    return;
  }
  Pad& pad = g_machine->pads[port];
  auto clamp = [](int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; };
  xe::hid::X_INPUT_GAMEPAD next = {};
  next.buttons = buttons;
  next.left_trigger = static_cast<uint8_t>(clamp(axes[0], 0, 255));
  next.right_trigger = static_cast<uint8_t>(clamp(axes[1], 0, 255));
  next.thumb_lx = static_cast<int16_t>(clamp(axes[2], -32768, 32767));
  next.thumb_ly = static_cast<int16_t>(clamp(axes[3], -32768, 32767));
  next.thumb_rx = static_cast<int16_t>(clamp(axes[4], -32768, 32767));
  next.thumb_ry = static_cast<int16_t>(clamp(axes[5], -32768, 32767));
  // the packet number moves when the state does, as the console's does
  if (std::memcmp(&next, &pad.state.gamepad, sizeof(next)) != 0) {
    pad.state.gamepad = next;
    pad.state.packet_number = uint32_t(pad.state.packet_number) + 1;
  }
}

namespace {

// The renderer made again, by the frame's own thread, while the renderer's
// thread is parked with nothing half done.
void remake_now() {
  Machine& m = *g_machine;
  const bool gone = m.remake_device_gone;
  m.remake = false;
  m.remake_device_gone = false;
  m.emulator->graphics_system()->command_processor()->ChimeraRemakeContext(!gone);
  m.device = device_now();
}

}  // namespace

void state_saving() {
  if (!g_machine || !g_machine->vulkan) {
    return;
  }
  auto* cp = g_machine->emulator->graphics_system()->command_processor();
  // only at a moment the renderer could be made again from; a frame nearly
  // always ends at one
  cp->ChimeraStateSaving(g_machine->renderer_quiet && cp->ChimeraQuiet() && !g_machine->remake);
}

void remake_renderer() {
  if (g_machine && g_machine->vulkan) {
    g_machine->remake = true;
  }
}

void frame() {
  Machine& m = *g_machine;
  m.input_read = false;
  auto* gs = m.emulator->graphics_system();
  if (m.vulkan) {
    // A savestate was loaded: the device is another one, and what the
    // renderer holds names nothing on it.
    if (device_now() != m.device) {
      m.remake = true;
      m.remake_device_gone = true;
    }
    // Made again before anybody runs, when the renderer's thread is where
    // that can be done; when a frame next ends with it there, if it is not.
    auto* cp = gs->command_processor();
    if (m.remake && (cp->ChimeraQuiet() || !cp->ChimeraContextUp())) {
      remake_now();
    }
  }
  const uint64_t f = m.frames + 1;
  // Two microseconds past the vertical blank: the thread that raises it has
  // been woken, and has raised it, by then.
  const int64_t end = xe::chimera::FrameBoundaryNs(static_cast<int64_t>(f)) + 2000;
  xe::chimera::SleepUntil(end);
  m.frames = f;
  if (m.vulkan) {
    // A frame ends with the renderer's thread parked where nothing of a draw
    // is half done - between packets, or inside one that waits for the
    // program - because a state saved now must be one the renderer can be
    // made again from. It nearly always is there already. When it is not,
    // the frame waits for it, for as long as a frame at most.
    auto* cp = gs->command_processor();
    m.renderer_quiet = xe::chimera::WaitFor([cp] { return cp->ChimeraQuiet(); },
                                            xe::chimera::FrameBoundaryNs(static_cast<int64_t>(f) + 1));
    if (m.remake && m.renderer_quiet) {
      remake_now();
    }
  }

  const size_t pairs = m.mix.size() / 2;
  for (size_t i = 0; i < pairs * 2; i++) {
    m.audio[i] = clamp16(m.mix[i]);
  }
  m.audio_pairs = static_cast<int>(pairs);
  m.mix.clear();
  for (MixDriver* driver : g_mix_drivers) {
    driver->taken(pairs);
  }

  // The picture: what the program last showed, copied out of the presenter's
  // image by the device. Asked from this thread while every other is parked.
  if (m.vulkan && m.rendering) {
    xe::ui::Presenter* presenter = gs ? gs->presenter() : nullptr;
    xe::ui::RawImage image;
    if (presenter && presenter->CaptureGuestOutput(image) && image.width && image.height) {
      m.video.resize(size_t(image.width) * image.height);
      m.video_width = static_cast<int>(image.width);
      m.video_height = static_cast<int>(image.height);
      for (uint32_t y = 0; y < image.height; y++) {
        const uint8_t* row = image.data.data() + size_t(y) * image.stride;
        uint32_t* out = m.video.data() + size_t(y) * image.width;
        for (uint32_t x = 0; x < image.width; x++) {
          out[x] = 0xFF000000u | uint32_t(row[x * 4]) << 16 | uint32_t(row[x * 4 + 1]) << 8 | row[x * 4 + 2];
        }
      }
    }
  }
}

const uint32_t* video(int& width, int& height) {
  width = g_machine ? g_machine->video_width : 0;
  height = g_machine ? g_machine->video_height : 0;
  return g_machine && !g_machine->video.empty() ? g_machine->video.data() : nullptr;
}

void set_rendering(bool on) {
  if (g_machine) {
    g_machine->rendering = on;
  }
}

bool input_was_read() { return g_machine && g_machine->input_read; }

const int16_t* audio(int& pairs) {
  pairs = g_machine ? g_machine->audio_pairs : 0;
  return g_machine ? g_machine->audio.data() : nullptr;
}

uint64_t frames() { return g_machine ? g_machine->frames : 0; }
uint64_t time_ns() { return static_cast<uint64_t>(xe::chimera::VirtualNanoseconds()); }
uint64_t switches() { return xe::chimera::Switches(); }
uint64_t instructions() { return xe::chimera::Instructions(); }
std::string instructions_by_thread() { return xe::chimera::InstructionsByThread(); }
uint64_t gpu_counter() {
  if (!g_machine) {
    return 0;
  }
  auto* gs = g_machine->emulator->graphics_system();
  return gs && gs->command_processor() ? gs->command_processor()->counter() : 0;
}

uint64_t ram_digest() {
  if (!g_machine) {
    return 0;
  }
  const uint8_t* p = g_machine->emulator->memory()->physical_membase();
  uint64_t h = 0xcbf29ce484222325ull;
  xe::cpu::MMIOHandler::BeginObserving();
  for (size_t i = 0; i < 0x20000000; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001b3ull;
  }
  xe::cpu::MMIOHandler::EndObserving();
  return h;
}

namespace {
struct SavedFile {
  std::string name;
  std::vector<uint8_t> data;
};
std::vector<SavedFile> g_saved;
const std::string g_no_name;
const std::vector<uint8_t> g_no_bytes;
}  // namespace

size_t savedata_snapshot() {
  g_saved.clear();
  const std::string content = std::string(chimera::memfs::ROOT) + "/content";
  for (const std::string& relative : chimera::memfs::list(content)) {
    const std::string path = content + "/" + relative;
    if (g_machine && std::find(g_machine->brought.begin(), g_machine->brought.end(), path) != g_machine->brought.end()) {
      continue;
    }
    SavedFile file;
    file.name = "content/" + relative;
    if (chimera::memfs::get(content + "/" + relative, file.data)) {
      g_saved.push_back(std::move(file));
    }
  }
  return g_saved.size();
}
const std::string& savedata_name(size_t index) { return index < g_saved.size() ? g_saved[index].name : g_no_name; }
const std::vector<uint8_t>& savedata_bytes(size_t index) {
  return index < g_saved.size() ? g_saved[index].data : g_no_bytes;
}

bool watched_access(void* host_address, bool is_write) {
  auto* handler = xe::cpu::MMIOHandler::global_handler();
  return handler && handler->HandleWatchedAccess(host_address, is_write);
}

namespace {
// changed where a fault is handled, which no compiler sees coming
volatile int g_view_notices = 0;
std::pair<uint32_t, uint32_t> view_notice(void*, uint32_t physical_address, uint32_t length, bool) {
  g_view_notices = g_view_notices + 1;
  return std::make_pair(physical_address, length);
}
}  // namespace

// The console's 512 MiB are seen at three places in its address space
// (0xA0000000, 0xC0000000, and 0xE0000000 one page on), and a program may
// write a page through any of them. A renderer watches pages it has copies
// of; whichever way the write comes, it must be told, once, and the write
// must land.
bool check_watched_views(std::string& report) {
  Options options;
  std::string error;
  if (!switch_on(options, error)) {
    report = error;
    return false;
  }
  xe::Memory* memory = g_machine->emulator->memory();
  void* handle = memory->RegisterPhysicalMemoryInvalidationCallback(view_notice, nullptr);
  static const uint32_t bases[] = {0xA0000000u, 0xC0000000u, 0xE0000000u};
  bool good = true;
  char line[160];
  for (const uint32_t base : bases) {
    auto* heap = memory->LookupHeap(base);
    uint32_t address = 0;
    if (!heap->Alloc(heap->page_size(), heap->page_size(), xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
                     xe::kMemoryProtectRead | xe::kMemoryProtectWrite, false, &address)) {
      std::snprintf(line, sizeof line, "no page could be had at %08X\n", base);
      report += line;
      good = false;
      continue;
    }
    const uint32_t physical = memory->GetPhysicalAddress(address);
    // the same page by each of its three addresses; the page's own first
    const uint32_t ways[] = {address, 0xA0000000u + physical, 0xC0000000u + physical, 0xE0000000u + physical - 0x1000u};
    uint8_t value = 0x40;
    for (size_t i = 0; i < sizeof ways / sizeof *ways; i++) {
      const uint32_t way = ways[i];
      if (i > 0 && way == address) {
        continue;
      }
      value++;
      g_view_notices = 0;
      memory->EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, false);
      // each write where it is written: a compiler may otherwise make one
      // store of the two, after the count between them is read
      *memory->TranslateVirtual<volatile uint8_t*>(way + 8) = value;
      const int first = g_view_notices;
      *memory->TranslateVirtual<volatile uint8_t*>(way + 9) = value;
      const uint8_t read = *memory->TranslateVirtual<volatile uint8_t*>(address + 8);
      const int second = g_view_notices;
      const bool fine = first == 1 && second == 1 && read == value;
      std::snprintf(line, sizeof line, "page %08X (physical %08X) written at %08X: %s (told %d then %d times, reads %02X)\n",
                    address, physical, way, fine ? "ok" : "WRONG", first, second, read);
      report += line;
      good = good && fine;
    }
    heap->Release(address);
  }
  memory->UnregisterPhysicalMemoryInvalidationCallback(handle);
  // Memory a game asks for aligned (MmAllocatePhysicalMemoryEx, which takes
  // a range, and an allocation anywhere in a heap): the console aligns the
  // physical address, and the 0xE0000000 view starts a page into physical
  // memory. Each heap must give it (chimera issue 272: every aligned
  // allocation in the 0xE0000000 view was refused, and Marvel vs. Capcom 3
  // wrote to the 0 it was handed).
  for (const uint32_t base : bases) {
    auto* heap = static_cast<xe::PhysicalHeap*>(memory->LookupHeap(base));
    for (const uint32_t alignment : {0x2000u, 0x10000u}) {
      uint32_t ranged = 0, anywhere = 0;
      const uint32_t flags = xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit;
      const uint32_t protect = xe::kMemoryProtectRead | xe::kMemoryProtectWrite;
      const bool got_ranged = heap->AllocRange(base, base + 0x1FFFFFFFu, 0x20000, alignment, flags, protect, true, &ranged);
      const bool got_anywhere = heap->Alloc(0x20000, alignment, flags, protect, true, &anywhere);
      const bool fine = got_ranged && got_anywhere && memory->GetPhysicalAddress(ranged) % alignment == 0 &&
                        memory->GetPhysicalAddress(anywhere) % alignment == 0;
      std::snprintf(line, sizeof line, "aligned to %05X at %08X: %s (in a range %08X, anywhere %08X)\n", alignment, base,
                    fine ? "ok" : "WRONG", ranged, anywhere);
      report += line;
      good = good && fine;
      if (got_ranged) heap->Release(ranged);
      if (got_anywhere) heap->Release(anywhere);
    }
  }
  return good;
}

// With no game: a thread that waits by reading the clock in a loop, as a
// program does that waits for a moment to come, and another that sleeps five
// milliseconds of the machine's time. The reader does here what a block of
// recompiled code does - pays for its instructions out of its slice, and
// asks for the next thread when the slice is spent. It must come to the end
// of its turn, or the sleeper never runs and the machine's time runs away
// under the reader (chimera issue 258: no frame ended).
bool check_turns(std::string& report) {
  static volatile bool woke;
  woke = false;
  auto sleeper = xe::threading::Thread::Create({}, [] {
    xe::threading::Sleep(std::chrono::milliseconds(5));
    woke = true;
  });
  const int64_t start = xe::chimera::VirtualNanoseconds();
  const uint64_t switches = xe::chimera::Switches();
  uint64_t reads = 0;
  // ten seconds of the machine's time at most, at fifty instructions a read
  while (!woke && reads < 100000000) {
    xe::chimera::g_instruction_budget -= 50;
    if (xe::chimera::g_instruction_budget < 0) {
      xe::chimera::BudgetSpent(nullptr);
    }
    xe::chimera::VirtualNanoseconds();
    reads++;
  }
  const int64_t took = xe::chimera::VirtualNanoseconds() - start;
  // woken at the first end of a turn after its five milliseconds: within a
  // slice or two of them
  const bool good = woke && took >= 5000000 && took < 6000000;
  char line[200];
  std::snprintf(line, sizeof line, "the clock was read %llu times in %lld ns of the machine's time, the baton changed hands %llu times, the sleeper %s\n",
                static_cast<unsigned long long>(reads), static_cast<long long>(took),
                static_cast<unsigned long long>(xe::chimera::Switches() - switches), woke ? "woke" : "never ran");
  report += line;
  return good;
}

bool bus_ready() { return g_machine != nullptr; }

bool bus_read(uint32_t address, uint8_t* out, size_t length) {
  if (!g_machine) {
    return false;
  }
  xe::Memory* memory = g_machine->emulator->memory();
  xe::cpu::MMIOHandler::BeginObserving();
  for (size_t i = 0; i < length; i++) {
    out[i] = *memory->TranslateVirtual<uint8_t*>(address + static_cast<uint32_t>(i));
  }
  xe::cpu::MMIOHandler::EndObserving();
  return true;
}

bool bus_write(uint32_t address, uint8_t value) {
  if (!g_machine) {
    return false;
  }
  xe::Memory* memory = g_machine->emulator->memory();
  uint32_t protect = 0;
  auto* heap = memory->LookupHeap(address);
  if (!heap || !heap->QueryProtect(address, &protect) || !(protect & xe::kMemoryProtectWrite)) {
    return false;
  }
  *memory->TranslateVirtual<uint8_t*>(address) = value;
  return true;
}

}  // namespace chimera_xenia
