// The machine behind the core's exports (xenia_driver.h): Xenia set up with no
// window, a pad driver that answers with what the frontend set, a sound
// driver that keeps what the program plays, and the frame.
//
// The keystroke part of the pad driver (which virtual key a button is, the
// order changes are reported in, the repeat) follows Xenia's SDL driver,
// src/xenia/hid/sdl/sdl_input_driver.cc (BSD-3-Clause, Copyright 2020 Ben
// Vanik and the Xenia authors), with the host's pad taken out of it.
#include "xenia_driver.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

#include "machine_settings.h"

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
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/virtual_key.h"

DECLARE_path(log_file);
DECLARE_bool(log_to_stdout);

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
  Pad pads[PORTS];
  bool input_read = false;
  uint64_t frames = 0;
  // what the program has played since the last frame ended, mixed; and the
  // last frame's, for the frontend
  std::vector<int32_t> mix;
  std::vector<int16_t> audio;
  int audio_pairs = 0;
  // the last picture read back
  bool vulkan = false;
  bool rendering = true;
  std::vector<uint32_t> video;
  int video_width = 0, video_height = 0;
};
Machine* g_machine = nullptr;

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

}  // namespace

bool boot(const std::string& program, const Options& options, std::string& error) {
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
  xe::chimera::g_trace_schedule = options.trace_schedule;
  xe::InitializeLogging("chimera");

  g_machine = new Machine();
  Machine& m = *g_machine;
  m.audio.resize(AUDIO_CAP_PAIRS * 2);
  for (int i = 0; i < PORTS; i++) {
    m.pads[i].connected = i < options.pads;
  }

  // The emulator's own roots - its storage, its content, its cache - are
  // names nothing is under: the program is what it was given and nothing is
  // kept from one run to the next.
  const std::filesystem::path work = "work";
  m.emulator = std::make_unique<xe::Emulator>("", work, work / "content", work / "cache");
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
  status = m.emulator->LaunchPath(program);
  if (XFAILED(status)) {
    char text[64];
    std::snprintf(text, sizeof(text), "the program did not launch (%08X)", status);
    error = program + ": " + text;
    return false;
  }
  return true;
}

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

void frame() {
  Machine& m = *g_machine;
  m.input_read = false;
  const uint64_t f = m.frames + 1;
  // Two microseconds past the vertical blank: the thread that raises it has
  // been woken, and has raised it, by then.
  xe::chimera::SleepUntil(xe::chimera::FrameBoundaryNs(static_cast<int64_t>(f)) + 2000);
  m.frames = f;

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
    auto* gs = m.emulator->graphics_system();
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

bool watched_access(void* host_address, bool is_write) {
  auto* handler = xe::cpu::MMIOHandler::global_handler();
  return handler && handler->HandleWatchedAccess(host_address, is_write);
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
