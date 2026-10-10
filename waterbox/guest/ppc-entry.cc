// The instruction tests as a guest: one exported call that runs upstream's
// runner on the files the host mounted, and answers with how it ended. What
// each test did is on the guest's standard error, as it is natively.
#include <string>
#include <vector>

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"

extern "C" __attribute__((visibility("default"), used)) int ChimeraPpcTests(void) {
  // A guest's files have names and no directories; the recompiler may use
  // AVX2 and FMA and nothing else, as in every build of this core.
  static char arg0[] = "ppc-tests";
  static char arg1[] = "--test_path=";
  static char arg2[] = "--test_bin_path=";
  static char arg3[] = "--test_skip_file=skip.txt";
  static char arg4[] = "--x64_extension_mask=3";
  static char* argv[] = {arg0, arg1, arg2, arg3, arg4, nullptr};
  const xe::ConsoleAppEntryInfo info = xe::GetConsoleAppEntryInfo();
  int argc = 5;
  char** args = argv;
  cvar::ParseLaunchArguments(argc, args, info.positional_usage, info.positional_options);
  return info.entry_point({std::string(arg0)});
}
