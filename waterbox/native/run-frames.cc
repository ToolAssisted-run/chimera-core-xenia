// The native reference of the core itself: the same exports the sandbox has
// (driver/wbx-entry.cc), called directly, a frame at a time, by the same
// harness the sandboxed runner uses (gate-harness.h). Built in the scheduler
// tree only: a frame is a stretch of the machine's own clock.
//
//   run-frames <program> <work directory, which must not exist> [options]
//   run-frames --check-views <work directory, which must not exist>
//   run-frames --check-turns <work directory, which must not exist>
//
// The options are the harness's. Natively there are no states. The other
// two forms need no program: each runs one of the core's own checks - of
// watched memory (CheckWatchedViews), or of a thread that reads the clock in
// a loop (CheckTurns) - and ends with 0 when its last line is "views ok" or
// "turns ok".
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
uint64_t GetInstructionCount(void);
const char* DebugInstructionsByThread(void);
uint64_t GetGpuCounter(void);
uint64_t GetRamDigest(void);
const uint8_t* ReadBus(int32_t, int64_t, int32_t);
void RemakeRenderer(void);
void StateSaving(void);
uint32_t* GetVideoBgra(void);
int GetVideoWidth(void);
int GetVideoHeight(void);
int InsertDisc(int32_t);
const char* CheckWatchedViews(void);
const char* CheckTurns(void);
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
  const bool check_turns = !std::strcmp(argv[1], "--check-turns");
  if (check_turns || !std::strcmp(argv[1], "--check-views")) {
    if (mkdir(argv[2], 0777) != 0 || chdir(argv[2]) != 0) {
      std::fprintf(stderr, "run-frames: %s must be a directory that can be made and is not there yet\n", argv[2]);
      return 2;
    }
    const char* report = check_turns ? CheckTurns() : CheckWatchedViews();
    std::fputs(report, stdout);
    std::fflush(stdout);
    const size_t n = std::strlen(report);
    std::_Exit(n >= 9 && !std::strcmp(report + n - 9, check_turns ? "turns ok\n" : "views ok\n") ? 0 : 1);
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
  char slots[4096];
  o.game = program;
  harness_slots(&o, [](const char* path) { return path; }, slots, sizeof slots);
  if (!write_file("rom.name", program) || !write_file("settings", settings) || !write_file("slots", slots)) {
    std::fprintf(stderr, "run-frames: cannot write into %s\n", argv[2]);
    return 2;
  }
  const struct harness_core core = {
      Init,          GetLoadError,        SetAxis,          FrameAdvance,   InputWasRead,       GetAudio,
      GetAudioSampleCount, GetMachineTimeNs, GetSwitchCount, GetInstructionCount, DebugInstructionsByThread, GetGpuCounter, GetRamDigest, ReadBus, RemakeRenderer, StateSaving,
      GetVideoBgra,  GetVideoWidth,       GetVideoHeight, InsertDisc,
      nullptr,       nullptr,             nullptr,
  };
  const int result = harness_run(&core, &o);
  std::fflush(stdout);
  std::fflush(stderr);
  // The machine's threads are parked where the scheduler left them: leaving
  // is the process ending, not the emulator being taken apart.
  std::_Exit(result);
}
