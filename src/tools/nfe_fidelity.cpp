// Phase 2: matching-mode fidelity against the feed.
//
//   nfe_fidelity <itch-file> [--out DIR]
//
// Writes DIR/fidelity.json (totals by class), DIR/per_symbol.csv,
// DIR/events.csv (one row per inferred aggressor) and DIR/unexplained.txt.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "nfe/fidelity.hpp"
#include "nfe/io.hpp"

using namespace nfe;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: nfe_fidelity <itch-file> [--out DIR]\n"); return 2; }
  std::string out = "out";
  for (int i = 2; i < argc; ++i)
    if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
  std::filesystem::create_directories(out);

  MappedFile f(argv[1]);
  auto fid = std::make_unique<Fidelity<>>();
  fid->events_csv = std::fopen((out + "/events.csv").c_str(), "w");
  fid->unexplained_txt = std::fopen((out + "/unexplained.txt").c_str(), "w");
  std::fprintf(fid->events_csv, "locate,ts_ns,side,qty,limit,n_msgs,has_p,has_c_printable,same_ts,class\n");
  const auto t0 = std::chrono::steady_clock::now();
  auto st = itch::parse_stream(f.data(), f.size(), *fid);
  fid->finish();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fclose(fid->events_csv);
  std::fclose(fid->unexplained_txt);

  FILE* ps = std::fopen((out + "/per_symbol.csv").c_str(), "w");
  std::fprintf(ps, "locate,symbol,messages");
  for (int k = 0; k < int(Cls::N); ++k) std::fprintf(ps, ",%s", cls_name(Cls(k)));
  std::fprintf(ps, ",one_tick_events,same_ts_events\n");
  for (int loc = 1; loc < 65536; ++loc) {
    auto& s = fid->per_sym[loc];
    uint64_t n = 0;
    for (auto v : s.cls) n += v;
    if (!n) continue;
    std::fprintf(ps, "%d,%s,%llu", loc, fid->r.names[loc].c_str(), (unsigned long long)n);
    for (auto v : s.cls) std::fprintf(ps, ",%llu", (unsigned long long)v);
    std::fprintf(ps, ",%llu,%llu\n", (unsigned long long)s.one_tick_events, (unsigned long long)s.same_ts_events);
  }
  std::fclose(ps);

  uint64_t events = 0;
  for (auto v : fid->total) events += v;
  FILE* j = std::fopen((out + "/fidelity.json").c_str(), "w");
  std::fprintf(j, "{\n  \"messages\": %llu,\n  \"events\": %llu,\n  \"unknown_ref\": %llu,\n  \"by_class\": {",
               (unsigned long long)st.messages, (unsigned long long)events, (unsigned long long)fid->r.c.unknown_ref);
  for (int k = 0; k < int(Cls::N); ++k)
    std::fprintf(j, "%s\"%s\": %llu", k ? ", " : "", cls_name(Cls(k)), (unsigned long long)fid->total[k]);
  std::fprintf(j, "},\n  \"first_unexplained_msg\": %llu,\n  \"first_anomaly_msg\": %llu,\n  \"seconds\": %.3f\n}\n",
               (unsigned long long)fid->first_unexplained_msg, (unsigned long long)fid->r.c.first_anomaly_msg, secs);
  std::fclose(j);

  const auto& t = fid->total;
  const double cont = double(events - t[int(Cls::Cross)]);
  const double no_hidden = double(t[int(Cls::Match)] + t[int(Cls::Grouping)] + t[int(Cls::Unexplained)] +
                                  t[int(Cls::NonDisplayPrice)]);
  std::printf("events=%llu continuous=%.0f exact=%.4f exact_no_hidden=%.4f unexplained=%llu  (%.2fs)\n",
              (unsigned long long)events, cont,
              cont ? (t[int(Cls::Match)] + t[int(Cls::MatchHidden)]) / cont : 0.0,
              no_hidden ? t[int(Cls::Match)] / no_hidden : 0.0,
              (unsigned long long)t[int(Cls::Unexplained)], secs);
  for (int k = 0; k < int(Cls::N); ++k) std::printf("  %-17s %llu\n", cls_name(Cls(k)), (unsigned long long)t[k]);
  return 0;
}
