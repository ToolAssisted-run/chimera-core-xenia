// What the core suggests for a game before a project exists (suggest.cc).
#ifndef CHIMERA_XENIA_SUGGEST_H_
#define CHIMERA_XENIA_SUGGEST_H_

#include <string>

namespace chimera_xenia {

// The answer SuggestSettings gives for the game file at `game`, as JSON.
std::string suggest_settings(const std::string& game);

}  // namespace chimera_xenia

#endif  // CHIMERA_XENIA_SUGGEST_H_
