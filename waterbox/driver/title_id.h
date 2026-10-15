// Which game a file is, without starting it: the title id the console knows
// it by, eight hex digits ("5454086B" is Max Payne 3). The per-game settings
// the core suggests are looked up by it (SuggestSettings, chimera#289).
#ifndef CHIMERA_XENIA_TITLE_ID_H_
#define CHIMERA_XENIA_TITLE_ID_H_

#include <string>

namespace chimera_xenia {

// The title id of a disc image (.iso) or of an Xbox Live Arcade game (a
// container: "CON ", "LIVE" or "PIRS"), in upper-case hex; empty with the
// reason in `why` when the file is neither or is damaged.
std::string title_id_of(const std::string& path, std::string& why);

}  // namespace chimera_xenia

#endif  // CHIMERA_XENIA_TITLE_ID_H_
