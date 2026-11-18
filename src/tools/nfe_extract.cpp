// Cut a symbol extract from a full-day ITCH file.
//
//   nfe_extract <in> <out> SYM1,SYM2,...
//
// Keeps market-wide messages (locate 0) and every message whose stock locate
// belongs to one of the symbols, byte for byte, so the extract is itself a
// valid ITCH file. Locates come from the Stock Directory (R) messages, which
// precede all order messages.
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "nfe/io.hpp"
#include "nfe/itch.hpp"

using namespace nfe;

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: nfe_extract <in> <out> SYM1,SYM2,...\n"); return 2; }
  std::set<std::string> want;
  std::stringstream ss(argv[3]);
  for (std::string s; std::getline(ss, s, ',');) want.insert(s);
  MappedFile f(argv[1]);
  FILE* out = std::fopen(argv[2], "wb");
  std::vector<uint8_t> keep(65536, 0);
  keep[0] = 1;
  const uint8_t* p = f.data();
  size_t off = 0, kept = 0, total = 0;
  while (off + 2 <= f.size()) {
    const size_t len = itch::be16(p + off);
    if (off + 2 + len > f.size() || len < 11) break;
    const uint8_t* m = p + off + 2;
    const uint16_t loc = itch::be16(m + 1);
    if (m[0] == 'R' && len >= 19) {
      itch::Symbol s = itch::symbol(m + 11);
      if (want.count(std::string(s.view()))) keep[loc] = 1;
    }
    if (keep[loc]) { std::fwrite(p + off, 1, 2 + len, out); ++kept; }
    ++total;
    off += 2 + len;
  }
  std::fclose(out);
  std::printf("kept %zu of %zu messages\n", kept, total);
  return 0;
}
