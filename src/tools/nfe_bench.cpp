// Phase 5: throughput and latency on a full-day replay.
//
//   nfe_bench <itch-file> [--runs N] [--book baseline|fast|both] [--json PATH]
//
// For each book design, measures over N runs (after one discarded warm-up):
//   parse_only   - decode every message, no book
//   replay       - decode and apply (the full phase 1 replay)
//   book_only    - apply pre-decoded order operations: no parsing at all
// and per-message latency percentiles of the book-only path, with the timer's
// own overhead measured and reported. Pin the process and fix the CPU
// frequency for numbers worth comparing (scripts/bench.sh does both).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nfe/fast_book.hpp"
#include "nfe/io.hpp"
#include "nfe/replay.hpp"

using namespace nfe;
using Clock = std::chrono::steady_clock;

// Cheapest available tick counter, serialized so the timed region cannot
// leak out. x86: the TSC. ARM (Apple silicon): the generic timer, which runs
// at 24 MHz on Apple chips, so per-operation latency there is quantized to
// about 42 ns; throughput numbers are unaffected. Benchmark on x86 Linux.
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
static inline uint64_t ticks() { _mm_lfence(); const uint64_t t = __rdtsc(); _mm_lfence(); return t; }
#elif defined(__aarch64__)
static inline uint64_t ticks() { uint64_t v; asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v)::"memory"); return v; }
#else
static inline uint64_t ticks() { return uint64_t(Clock::now().time_since_epoch().count()); }
#endif

struct Op {
  char type;
  char side;
  uint16_t loc;
  uint32_t qty, px;
  uint64_t ref, ref2, ts;
};

struct Decoder {
  std::vector<Op> ops;
  void on(const itch::AddOrder& m) { ops.push_back({'A', m.side, m.h.locate, m.shares, m.price, m.ref, 0, m.h.ts}); }
  void on(const itch::OrderExecuted& m) { ops.push_back({'E', 0, m.h.locate, m.shares, 0, m.ref, 0, m.h.ts}); }
  void on(const itch::OrderExecutedWithPrice& m) { ops.push_back({'E', 0, m.h.locate, m.shares, 0, m.ref, 0, m.h.ts}); }
  void on(const itch::OrderCancel& m) { ops.push_back({'E', 0, m.h.locate, m.shares, 0, m.ref, 0, m.h.ts}); }
  void on(const itch::OrderDelete& m) { ops.push_back({'D', 0, m.h.locate, 0, 0, m.ref, 0, m.h.ts}); }
  void on(const itch::OrderReplace& m) { ops.push_back({'U', 0, m.h.locate, m.shares, m.price, m.old_ref, m.new_ref, m.h.ts}); }
  template <class M> void on(const M&) {}
};

struct Counter {
  uint64_t n = 0, acc = 0;
  template <class M> void on(const M& m) { ++n; acc += m.h.ts; }
};

template <class Book>
inline void apply(Book& b, const Op& o) {
  switch (o.type) {
    case 'A': b.add(o.loc, o.ref, o.side, o.px, o.qty, o.ts); break;
    case 'E': b.reduce(o.ref, o.qty); break;
    case 'D': b.remove(o.ref); break;
    case 'U': b.replace(o.ref, o.ref2, o.px, o.qty, o.ts); break;
  }
}

struct Stat { double mean, sd; };
static Stat stat(const std::vector<double>& v) {
  double m = 0, s = 0;
  for (double x : v) m += x;
  m /= v.size();
  for (double x : v) s += (x - m) * (x - m);
  return {m, v.size() > 1 ? std::sqrt(s / (v.size() - 1)) : 0.0};
}

static double ticks_per_ns() {
  const auto t0 = Clock::now();
  const uint64_t c0 = ticks();
  while (Clock::now() - t0 < std::chrono::milliseconds(200)) {}
  const uint64_t c1 = ticks();
  return double(c1 - c0) / std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
}

template <class Book>
static std::string bench(const char* name, const MappedFile& f, const std::vector<Op>& ops, uint64_t msgs, int runs,
                         double tpn) {
  std::vector<double> replay, book_only;
  for (int r = 0; r <= runs; ++r) {
    auto rp = std::make_unique<Replayer<Book>>();
    auto t0 = Clock::now();
    itch::parse_stream(f.data(), f.size(), *rp);
    const double s1 = std::chrono::duration<double>(Clock::now() - t0).count();
    auto b = std::make_unique<Book>();
    t0 = Clock::now();
    for (const Op& o : ops) apply(*b, o);
    const double s2 = std::chrono::duration<double>(Clock::now() - t0).count();
    if (r > 0) { replay.push_back(msgs / s1 / 1e6); book_only.push_back(ops.size() / s2 / 1e6); }
  }
  // Latency: time each book operation with rdtsc. The timer's own cost is
  // measured on empty regions and reported, not subtracted.
  std::vector<uint32_t> lat(ops.size());
  {
    auto b = std::make_unique<Book>();
    for (size_t i = 0; i < ops.size(); ++i) {
      const uint64_t c0 = ticks();
      apply(*b, ops[i]);
      lat[i] = uint32_t(std::min<uint64_t>(ticks() - c0, UINT32_MAX));
    }
  }
  std::vector<uint32_t> empty(1 << 20);
  for (auto& e : empty) { const uint64_t c0 = ticks(); e = uint32_t(ticks() - c0); }
  auto pct = [&](std::vector<uint32_t>& v, double p) {
    size_t k = size_t(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k] / tpn;
  };
  const double p50 = pct(lat, 0.5), p99 = pct(lat, 0.99), p999 = pct(lat, 0.999);
  const double t50 = pct(empty, 0.5);
  auto a = stat(replay), c = stat(book_only);
  std::printf("%-9s replay %7.2f ± %.2f M msg/s   book-only %7.2f ± %.2f M op/s   latency p50 %.0f p99 %.0f p99.9 %.0f ns (timer %.0f ns)\n",
              name, a.mean, a.sd, c.mean, c.sd, p50, p99, p999, t50);
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "\"%s\": {\"replay_mmsg_s\": [%.3f, %.3f], \"book_only_mops_s\": [%.3f, %.3f], "
                "\"latency_ns\": {\"p50\": %.1f, \"p99\": %.1f, \"p99_9\": %.1f, \"timer_overhead_p50\": %.1f}}",
                name, a.mean, a.sd, c.mean, c.sd, p50, p99, p999, t50);
  return buf;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: nfe_bench <itch-file> [--runs N] [--book baseline|fast|both] [--json PATH]\n"); return 2; }
  int runs = 10;
  std::string which = "both", json;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--runs") && i + 1 < argc) runs = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--book") && i + 1 < argc) which = argv[++i];
    else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
  }
  MappedFile f(argv[1]);
  volatile uint8_t sink = 0;
  for (size_t i = 0; i < f.size(); i += 4096) sink += f.data()[i];  // fault the file in

  std::vector<double> parse;
  uint64_t msgs = 0;
  for (int r = 0; r <= runs; ++r) {
    Counter c;
    const auto t0 = Clock::now();
    itch::parse_stream(f.data(), f.size(), c);
    const double s = std::chrono::duration<double>(Clock::now() - t0).count();
    msgs = c.n;
    if (r > 0) parse.push_back(msgs / s / 1e6);
  }
  Decoder d;
  itch::parse_stream(f.data(), f.size(), d);
  const double tpn = ticks_per_ns();
  auto p = stat(parse);
  std::printf("messages %llu, book ops %zu, runs %d\nparse-only %.2f ± %.2f M msg/s\n", (unsigned long long)msgs,
              d.ops.size(), runs, p.mean, p.sd);
  std::vector<std::string> parts;
  if (which == "baseline" || which == "both") parts.push_back(bench<BaselineBook>("baseline", f, d.ops, msgs, runs, tpn));
  if (which == "fast" || which == "both") parts.push_back(bench<FastBook>("fast", f, d.ops, msgs, runs, tpn));
  if (!json.empty()) {
    FILE* j = std::fopen(json.c_str(), "w");
    std::fprintf(j, "{\"messages\": %llu, \"book_ops\": %zu, \"runs\": %d, \"parse_only_mmsg_s\": [%.3f, %.3f]",
                 (unsigned long long)msgs, d.ops.size(), runs, p.mean, p.sd);
    for (auto& s : parts) std::fprintf(j, ", %s", s.c_str());
    std::fprintf(j, "}\n");
    std::fclose(j);
  }
  return 0;
}
