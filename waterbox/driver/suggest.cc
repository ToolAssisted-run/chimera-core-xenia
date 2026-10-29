// What the core suggests for a game before a project exists (the engine's
// ce_suggest_settings; SuggestSettings in wbx-entry.cc): the game is looked up
// by its title id in Xenia Manager's list of optimized settings, which the
// core carries (optimized_settings.inc, made by
// waterbox/import-optimized-settings.py), and the answer is
//
//   {"title_id": "5454086B", "values": {setting: value, ...}, "note": "..."}
//
// "values" is empty for a game the list does not have, and the note says so.
// Nothing is applied here: the new-project wizard puts the values on its
// settings page, where every one can still be changed (chimera#289).
#include "suggest.h"

#include <cstring>

#include "title_id.h"

/* A text a person reads is a key of languages/en.json, each of its
 * arguments after a TEXT_ARG, and a note of several texts puts a TEXT_LINE
 * before each (chimera docs/translations.md). Each is a string of its own so
 * the byte after it is never read as more of its hex. */
#define TEXT_ARG "\x1f"
#define TEXT_LINE "\x1d"

namespace chimera_xenia {
namespace {

struct OptimizedSettings {
  const char* title_id;
  const char* title;
  const char* values;  // a JSON object of the core's settings
  const char* note;
};

const OptimizedSettings kList[] = {
#include "optimized_settings.inc"
};

std::string json_quoted(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      // a text's separators (0x1F, 0x1D) too: they are what the engine reads
      char code[8];
      std::snprintf(code, sizeof code, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += code;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

}  // namespace

std::string suggest_settings(const std::string& game) {
  std::string why;
  const std::string id = game.empty() ? std::string() : title_id_of(game, why);
  const OptimizedSettings* found = nullptr;
  for (const OptimizedSettings& e : kList) {
    if (!id.empty() && id == e.title_id) {
      found = &e;
      break;
    }
  }
  std::string note;
  if (found) {
    note = std::string(found->note) + TEXT_LINE "suggest.credit";
  } else if (id.empty()) {
    note = std::string("suggest.none" TEXT_ARG) + (game.empty() ? std::string("suggest.no_game") : why);
  } else {
    note = std::string("suggest.not_listed" TEXT_ARG) + id;
  }
  return "{\"title_id\":" + json_quoted(id) + ",\"values\":" + (found ? found->values : "{}") +
         ",\"note\":" + json_quoted(note) + "}";
}

}  // namespace chimera_xenia
