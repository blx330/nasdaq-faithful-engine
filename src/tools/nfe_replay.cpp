// Phase 1: reconstruct the full-day displayed book and report the checks.
//
//   nfe_replay <itch-file> [--out DIR] [--strict]
//
// Writes DIR/replay.json (counters) and DIR/crosses.csv (every Cross Trade).
// With --strict, exits non-zero on any unknown reference, overfill or crossed
// book during continuous trading.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "nfe/io.hpp"
#include "nfe/replay.hpp"

using namespace nfe;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: nfe_replay <itch-file> [--out DIR] [--strict]\n"); return 2; }
  std::string out = "out";
  bool strict = false;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!std::strcmp(argv[i], "--strict")) strict = true;
  }
  std::filesystem::create_directories(out);
  MappedFile f(argv[1]);
  auto r = std::make_unique<Replayer<>>();
  const auto t0 = std::chrono::steady_clock::now();
  const auto st = itch::parse_stream(f.data(), f.size(), *r);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const auto& c = r->c;
  FILE* j = std::fopen((out + "/replay.json").c_str(), "w");
  std::fprintf(j, "{\n  \"file\": \"%s\",\n  \"messages\": %llu,\n  \"unknown_type\": %llu,\n  \"malformed\": %llu,\n"
               "  \"truncated_tail_bytes\": %llu,\n",
               argv[1], (unsigned long long)st.messages, (unsigned long long)st.unknown,
               (unsigned long long)st.malformed, (unsigned long long)st.truncated_tail_bytes);
  std::fprintf(j, "  \"unknown_ref\": %llu,\n  \"duplicate_ref\": %llu,\n  \"overfill\": %llu,\n"
               "  \"crossed_continuous\": %llu,\n  \"crossed_other\": %llu,\n  \"broken_trades\": %llu,\n"
               "  \"c_printable\": %llu,\n  \"c_nonprintable\": %llu,\n  \"p_trades\": %llu,\n"
               "  \"first_anomaly_msg\": %llu,\n  \"orders_open_at_end\": %zu,\n  \"seconds\": %.3f,\n  \"by_type\": {",
               (unsigned long long)c.unknown_ref, (unsigned long long)c.duplicate_ref,
               (unsigned long long)c.overfill, (unsigned long long)c.crossed_continuous,
               (unsigned long long)c.crossed_other, (unsigned long long)c.broken_trades,
               (unsigned long long)c.c_printable, (unsigned long long)c.c_nonprintable,
               (unsigned long long)c.p_trades, (unsigned long long)c.first_anomaly_msg, r->book.order_count(), secs);
  bool first = true;
  for (int t = 0; t < 128; ++t)
    if (c.by_type[t]) { std::fprintf(j, "%s\"%c\": %llu", first ? "" : ", ", t, (unsigned long long)c.by_type[t]); first = false; }
  std::fprintf(j, "}\n}\n");
  std::fclose(j);

  FILE* x = std::fopen((out + "/crosses.csv").c_str(), "w");
  std::fprintf(x, "locate,symbol,cross_type,price,shares,ts_ns\n");
  for (auto& [loc, v] : r->crosses)
    for (auto& p : v)
      std::fprintf(x, "%u,%s,%c,%.4f,%llu,%llu\n", loc, r->names[loc].c_str(), p.type, p.price / 1e4,
                   (unsigned long long)p.shares, (unsigned long long)p.ts);
  std::fclose(x);

  std::printf("messages=%llu unknown_ref=%llu overfill=%llu crossed_continuous=%llu crossed_other=%llu  %.2fs (%.1f M msg/s)\n",
              (unsigned long long)st.messages, (unsigned long long)c.unknown_ref, (unsigned long long)c.overfill,
              (unsigned long long)c.crossed_continuous, (unsigned long long)c.crossed_other, secs,
              st.messages / secs / 1e6);
  if (strict && (c.unknown_ref || c.overfill || c.crossed_continuous || c.duplicate_ref || st.malformed)) return 1;
  return 0;
}
