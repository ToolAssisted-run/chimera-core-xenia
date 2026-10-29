// Which game a file is, without starting it (title_id.h). Read here with the
// C library and nothing of the emulator, because no emulator exists when a
// project's settings are suggested: the engine calls SuggestSettings instead
// of Init.
//
// A disc image holds the console's file system (XDVDFS) at one of a few
// places, the same ones Xenia tries (vfs/devices/disc_image_device.cc); the
// game is its default.xex, and the title id is in that program's execution
// information. An Arcade game is a container, whose header carries the title
// id at a fixed place.
#include "title_id.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace chimera_xenia {
namespace {

constexpr uint64_t kSector = 0x800;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[1] << 8 | p[0]); }

bool read_at(FILE* f, uint64_t at, void* out, size_t n) {
  return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0 && fread(out, 1, n, f) == n;
}

std::string hex8(uint32_t v) {
  char s[9];
  std::snprintf(s, sizeof s, "%08X", v);
  return s;
}

bool same_name(const char* a, size_t a_len, const char* b) {
  if (a_len != std::strlen(b)) return false;
  for (size_t i = 0; i < a_len; i++) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// The title id in a program's header: the execution information (optional
// header 0x00040006), whose fourth word it is.
bool title_of_xex(const std::vector<uint8_t>& x, uint32_t& title) {
  if (x.size() < 0x18 || std::memcmp(x.data(), "XEX2", 4) != 0) return false;
  const uint32_t count = be32(&x[0x14]);
  for (uint32_t i = 0; i < count && 0x18 + 8ull * (i + 1) <= x.size(); i++) {
    const uint8_t* h = &x[0x18 + 8 * i];
    if (be32(h) == 0x00040006u) {
      const uint32_t at = be32(h + 4);
      if (at + 16ull > x.size()) return false;
      title = be32(&x[at + 12]);
      return true;
    }
  }
  return false;
}

// default.xex in the root folder of the file system at `game`: the folder is
// a tree of entries, each with its left and right neighbours' places.
bool find_default_xex(FILE* f, uint64_t game, uint64_t size, uint64_t& at, uint32_t& length) {
  uint8_t volume[32];
  if (!read_at(f, game + 32 * kSector, volume, sizeof volume)) return false;
  const uint32_t root_sector = le32(volume + 20), root_size = le32(volume + 24);
  if (root_size < 14 || root_size > (32u << 20) || game + uint64_t(root_sector) * kSector + root_size > size) return false;
  std::vector<uint8_t> root(root_size);
  if (!read_at(f, game + uint64_t(root_sector) * kSector, root.data(), root.size())) return false;
  std::vector<uint32_t> todo{0};
  for (size_t visited = 0; !todo.empty() && visited < 65536; visited++) {
    const uint64_t e = uint64_t(todo.back()) * 4;
    todo.pop_back();
    if (e + 14 > root.size()) continue;
    const uint8_t* p = &root[e];
    const uint8_t name_length = p[13];
    if (e + 14 + name_length > root.size()) continue;
    if (same_name(reinterpret_cast<const char*>(p + 14), name_length, "default.xex") && !(p[12] & 0x10)) {
      at = game + uint64_t(le32(p + 4)) * kSector;
      length = le32(p + 8);
      return at + length <= size;
    }
    if (le16(p + 0)) todo.push_back(le16(p + 0));
    if (le16(p + 2)) todo.push_back(le16(p + 2));
  }
  return false;
}

}  // namespace

std::string title_id_of(const std::string& path, std::string& why) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    why = "suggest.reason.not_opened";
    return {};
  }
  std::string id;
  uint8_t head[0x364];
  fseeko(f, 0, SEEK_END);
  const uint64_t size = static_cast<uint64_t>(ftello(f));
  if (read_at(f, 0, head, sizeof head) &&
      (!std::memcmp(head, "CON ", 4) || !std::memcmp(head, "LIVE", 4) || !std::memcmp(head, "PIRS", 4))) {
    id = hex8(be32(head + 0x360));
  } else {
    static const uint64_t games[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000};
    uint64_t at = 0;
    uint32_t length = 0;
    bool found = false;
    for (uint64_t game : games) {
      char magic[20];
      if (read_at(f, game + 32 * kSector, magic, sizeof magic) && !std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20)) {
        found = find_default_xex(f, game, size, at, length);
        break;
      }
    }
    if (!found) {
      why = "suggest.reason.not_a_game";
    } else {
      // the headers are at the start of the program, and are small
      std::vector<uint8_t> xex(length < 0x10000 ? length : 0x10000);
      uint32_t title = 0;
      if (read_at(f, at, xex.data(), xex.size()) && title_of_xex(xex, title)) {
        id = hex8(title);
      } else {
        why = "suggest.reason.no_title";
      }
    }
  }
  std::fclose(f);
  return id;
}

}  // namespace chimera_xenia
