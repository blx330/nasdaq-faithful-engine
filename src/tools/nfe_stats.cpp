// Message counts by type and the busiest symbols, for picking extracts.
//
//   nfe_stats <itch-file> [top-N]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nfe/io.hpp"
#include "nfe/itch.hpp"

using namespace nfe;

struct Counter {
  uint64_t by_type[128]{};
  std::vector<uint64_t> per_loc = std::vector<uint64_t>(65536, 0);
  std::vector<std::string> names = std::vector<std::string>(65536);
  void on(const itch::StockDirectory& m) { tick(m.h); names[m.h.locate] = std::string(m.stock.view()); }
  template <class M> void on(const M& m) { tick(m.h); }
  void tick(const itch::Header& h) { ++by_type[uint8_t(h.type) & 127]; ++per_loc[h.locate]; }
};

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: nfe_stats <itch-file> [top-N]\n"); return 2; }
  const int top = argc > 2 ? std::atoi(argv[2]) : 20;
  MappedFile f(argv[1]);
  Counter c;
  auto st = itch::parse_stream(f.data(), f.size(), c);
  std::printf("messages %llu  unknown %llu  malformed %llu  tail %llu\n", (unsigned long long)st.messages,
              (unsigned long long)st.unknown, (unsigned long long)st.malformed,
              (unsigned long long)st.truncated_tail_bytes);
  for (int t = 0; t < 128; ++t)
    if (c.by_type[t]) std::printf("  %c %12llu\n", t, (unsigned long long)c.by_type[t]);
  std::vector<int> idx;
  for (int i = 1; i < 65536; ++i) if (c.per_loc[i]) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return c.per_loc[a] > c.per_loc[b]; });
  std::printf("busiest symbols:\n");
  for (int i = 0; i < top && i < int(idx.size()); ++i)
    std::printf("  %-8s %12llu\n", c.names[idx[i]].c_str(), (unsigned long long)c.per_loc[idx[i]]);
  return 0;
}
