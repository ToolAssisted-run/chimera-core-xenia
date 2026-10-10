// The native reference of the core itself: the same exports the sandbox has
// (driver/wbx-entry.cc), called directly, a frame at a time, by the same
// harness the sandboxed runner uses (gate-harness.h). Built in the scheduler
// tree only: a frame is a stretch of the machine's own clock.
//
//   run-frames <program> <work directory, which must not exist> [options]
//
// The options are the harness's. Natively there are no states.
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include "../gate-harness.h"

int Init(void);
const char* GetLoadError(void);
void SetAxis(int32_t, int32_t);
void FrameAdvance(uint64_t);
int InputWasRead(void);
int16_t* GetAudio(void);
int GetAudioSampleCount(void);
uint64_t GetMachineTimeNs(void);
uint64_t GetSwitchCount(void);
uint64_t GetGpuCounter(void);
uint64_t GetRamDigest(void);
const uint8_t* ReadBus(int32_t, int64_t, int32_t);
uint32_t* GetVideoBgra(void);
int GetVideoWidth(void);
int GetVideoHeight(void);
}

static bool write_file(const char* name, const char* text) {
  FILE* f = std::fopen(name, "wb");
  if (!f) {
    return false;
  }
  std::fputs(text, f);
  return std::fclose(f) == 0;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: run-frames <program> <work directory> [options]\n");
    return 2;
  }
  struct harness_opts o;
  if (!harness_parse(argc, argv, 3, &o)) {
    return 2;
  }
  char program[PATH_MAX];
  if (!realpath(argv[1], program)) {
    std::fprintf(stderr, "run-frames: no such program: %s\n", argv[1]);
    return 2;
  }
  // What the emulator writes it writes under where it is started, and a run
  // must not find what another left: the directory is this run's own.
  if (mkdir(argv[2], 0777) != 0 || chdir(argv[2]) != 0) {
    std::fprintf(stderr, "run-frames: %s must be a directory that can be made and is not there yet\n", argv[2]);
    return 2;
  }
  char settings[256];
  harness_settings(&o, settings, sizeof settings);
  if (!write_file("rom.name", program) || !write_file("settings", settings)) {
    std::fprintf(stderr, "run-frames: cannot write into %s\n", argv[2]);
    return 2;
  }
  const struct harness_core core = {
      Init,          GetLoadError,        SetAxis,          FrameAdvance,   InputWasRead,       GetAudio,
      GetAudioSampleCount, GetMachineTimeNs, GetSwitchCount, GetGpuCounter, GetRamDigest, ReadBus,
      GetVideoBgra,  GetVideoWidth,       GetVideoHeight,
      nullptr,       nullptr,             nullptr,
  };
  const int result = harness_run(&core, &o);
  std::fflush(stdout);
  std::fflush(stderr);
  // The machine's threads are parked where the scheduler left them: leaving
  // is the process ending, not the emulator being taken apart.
  std::_Exit(result);
}
