// The machine as the core's exports see it: an Xbox 360 that is switched on
// with a program in it, given its pads' state, and run a frame at a time. The
// same file natively (the scheduler tree, build/vsched) and in the sandbox.
#ifndef CHIMERA_XENIA_DRIVER_H_
#define CHIMERA_XENIA_DRIVER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace chimera_xenia {

constexpr int PORTS = 4;
// A pad's axes, in this order: left trigger, right trigger (0..255), then the
// left stick's x and y and the right stick's (-32768..32767, up and right
// positive, as the console has them).
constexpr int AXES_PER_PORT = 6;

struct Options {
  // "null" draws nothing; "vulkan" is Xenia's Vulkan renderer.
  std::string renderer = "null";
  // How many pads are plugged in, from the first port.
  int pads = 1;
  // The emulator's log on standard output, for a person.
  bool log = false;
  // Every handing over of the baton on standard error, for finding where two
  // runs part.
  bool trace_schedule = false;
};

// Switch the machine on with a program in it. False with the reason in error.
bool boot(const std::string& program, const Options& options, std::string& error);

// A pad for the frames that follow: the console's own button bits
// (X_INPUT_GAMEPAD_*) and AXES_PER_PORT axes.
void set_pad(int port, uint16_t buttons, const int32_t* axes);

// One frame: to just past the next vertical blank.
void frame();

// The picture as it stood when the last frame ended, 32 bits a pixel with
// blue lowest; null before the program has shown one, and always with the
// renderer that draws nothing.
const uint32_t* video(int& width, int& height);

// Off: the picture is not read back at the end of a frame (the frontend's
// fast-forward). The machine draws all the same.
void set_rendering(bool on);

// Whether the program asked for a pad's state during the last frame.
bool input_was_read();

// The last frame's sound: 48 kHz, stereo, interleaved.
const int16_t* audio(int& pairs);

uint64_t frames();
uint64_t time_ns();
uint64_t switches();
// Vertical blanks and presented pictures together, as the emulator counts them.
uint64_t gpu_counter();

// A digest of the console's 512 MiB, for comparing two runs. Read as an
// observer: a page the program has closed is read all the same, and nobody
// in the machine is told.
uint64_t ram_digest();

// A fault the sandbox hands back: a page somebody in the emulator watches
// (the renderer's caches watch the console's memory) was touched. True when
// the page was let go and the access can be tried again.
bool watched_access(void* host_address, bool is_write);

// The console's 32-bit address space, as an observer reads it (a closed
// page reads as what it holds) and as a tool writes it (a page that cannot be
// written is left alone; one that the renderer watches tells it so).
bool bus_ready();
bool bus_read(uint32_t address, uint8_t* out, size_t length);
bool bus_write(uint32_t address, uint8_t value);

}  // namespace chimera_xenia

#endif  // CHIMERA_XENIA_DRIVER_H_
