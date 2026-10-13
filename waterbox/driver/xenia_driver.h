// The machine as the core's exports see it: an Xbox 360 that is switched on
// with a program in it, given its pads' state, and run a frame at a time. The
// same file natively (the scheduler tree, build/vsched) and in the sandbox.
#ifndef CHIMERA_XENIA_DRIVER_H_
#define CHIMERA_XENIA_DRIVER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
  // An Xbox Live Arcade game asks the console whether it was bought, and is
  // its own free trial when the answer is no. True: bought. Only a game that
  // is a container is told so; a disc game is answered as it always was.
  bool arcade_bought = true;
  // The emulator's log on standard output, for a person.
  bool log = false;
  // Every handing over of the baton on standard error, for finding where two
  // runs part.
  bool trace_schedule = false;
  // What an instruction costs (picoseconds) and what a wait or a yield costs
  // (nanoseconds); zero is the machine's own. For measuring: they are what
  // the machine's speed is made of, and a machine with others is another.
  int64_t instruction_ps = 0;
  int64_t yield_ns = 0;
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

// The renderer made again on a new device before the next frame, as it is
// after a savestate is loaded - asked for by hand, so that the gate can see
// that it changes nothing. Nothing with the renderer that draws nothing.
void remake_renderer();

// A savestate is about to be made of the machine as it stands: what only
// the graphics device holds is fetched into the machine, so that the state
// has it. Nothing with the renderer that draws nothing.
void state_saving();

// Whether the program asked for a pad's state during the last frame.
bool input_was_read();

// The last frame's sound: 48 kHz, stereo, interleaved.
const int16_t* audio(int& pairs);

uint64_t frames();
uint64_t time_ns();
uint64_t switches();
uint64_t instructions();
std::string instructions_by_thread();
// Vertical blanks and presented pictures together, as the emulator counts them.
uint64_t gpu_counter();

// A digest of the console's 512 MiB, for comparing two runs. Read as an
// observer: a page the program has closed is read all the same, and nobody
// in the machine is told.
uint64_t ram_digest();

// What the console keeps - its profile, and what games saved - as it stands
// now: every file of the machine's content storage, in name order. The count;
// then each by index until the next snapshot.
size_t savedata_snapshot();
const std::string& savedata_name(size_t index);
const std::vector<uint8_t>& savedata_bytes(size_t index);

// A fault the sandbox hands back: a page somebody in the emulator watches
// (the renderer's caches watch the console's memory) was touched. True when
// the page was let go and the access can be tried again.
bool watched_access(void* host_address, bool is_write);

// A check of the core itself, with no program: the console is switched on,
// one page in each of the three places its memory is seen is watched as a
// renderer watches it, and written through each of the three. True when
// every write landed and was told of once; what happened is in report, a
// line a write. The machine is of no use afterwards.
bool check_watched_views(std::string& report);
// With no game: a thread that reads the clock in a loop comes to the end of
// its turn (issue 258).
bool check_turns(std::string& report);

// The console's 32-bit address space, as an observer reads it (a closed
// page reads as what it holds) and as a tool writes it (a page that cannot be
// written is left alone; one that the renderer watches tells it so).
bool bus_ready();
bool bus_read(uint32_t address, uint8_t* out, size_t length);
bool bus_write(uint32_t address, uint8_t value);

}  // namespace chimera_xenia

#endif  // CHIMERA_XENIA_DRIVER_H_
