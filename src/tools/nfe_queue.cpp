// Phase 4: order-level data for the queue position study.
//
//   nfe_queue <itch-file> --out DIR [--symbols A,B,C] [--day LABEL]
//
// Every displayed order added at the best bid or ask during continuous
// trading is tracked from arrival to exit. Writes:
//   DIR/orders.csv  one row per tracked order: arrival state and outcome
//   DIR/fills.csv   one row per execution of a tracked order
//   DIR/mids.csv    best bid and ask for tracked symbols, on every change
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "nfe/io.hpp"
#include "nfe/replay.hpp"

using namespace nfe;

struct Row {
  uint64_t ref, ts;
  uint16_t loc;
  char side;
  uint32_t price, qty;
  uint64_t ahead, level_after;
  uint32_t spread_ticks;
  double imbalance;
  bool improved;  // set a new best price (nothing ahead)
  uint64_t first_fill_ts = 0, full_fill_ts = 0, exit_ts = 0;
  uint32_t filled = 0, cancelled = 0;
  char exit = 'O';  // F filled, D deleted, U replaced, O open at end
};

struct Tracker : NullObserver {
  std::vector<uint8_t> want = std::vector<uint8_t>(65536, 0);
  bool all = true;
  std::vector<Row> rows;
  std::unordered_map<uint64_t, size_t> live;
  std::vector<std::pair<uint32_t, uint32_t>> last_quote = std::vector<std::pair<uint32_t, uint32_t>>(65536, {0, 0});
  FILE* fills = nullptr;
  FILE* mids = nullptr;
  uint64_t pending_ahead = 0;

  bool tracked(uint16_t loc) const { return all || want[loc]; }

  template <class R> void pre_add(R& r, const itch::AddOrder& m) {
    pending_ahead = r.book.level_qty(m.h.locate, m.side, m.price);
  }
  template <class R> void post_add(R& r, Order& o) {
    if (!tracked(o.locate) || !r.continuous(o.locate, o.ts)) return;
    auto bb = r.book.best_bid(o.locate);
    auto ba = r.book.best_ask(o.locate);
    if (!bb || !ba) return;
    const bool at_best = o.side == 'B' ? o.price == bb->price : o.price == ba->price;
    if (!at_best) return;
    Row row{};
    row.ref = o.id; row.ts = o.ts; row.loc = o.locate; row.side = o.side; row.price = o.price; row.qty = o.qty;
    row.ahead = pending_ahead;
    row.level_after = r.book.level_qty(o.locate, o.side, o.price);
    row.spread_ticks = (ba->price - bb->price) / 100;
    row.imbalance = double(int64_t(bb->qty) - int64_t(ba->qty)) / double(bb->qty + ba->qty);
    row.improved = pending_ahead == 0;
    live[o.id] = rows.size();
    rows.push_back(row);
  }
  template <class R> void pre_exec(R&, Order& o, uint32_t qty, uint32_t px, uint64_t ts, bool printable) {
    auto it = live.find(o.id);
    if (it == live.end()) return;
    Row& row = rows[it->second];
    if (!row.first_fill_ts) row.first_fill_ts = ts;
    row.filled += qty;
    std::fprintf(fills, "%llu,%u,%llu,%c,%u,%u,%d\n", (unsigned long long)o.id, o.locate, (unsigned long long)ts,
                 o.side, px, qty, int(printable));
    if (qty >= o.qty) { row.full_fill_ts = ts; row.exit_ts = ts; row.exit = 'F'; live.erase(it); }
  }
  template <class R> void pre_cancel(R&, Order& o, uint32_t qty, uint64_t) {
    auto it = live.find(o.id);
    if (it != live.end()) rows[it->second].cancelled += qty;
  }
  template <class R> void pre_delete(R&, Order& o, uint64_t ts) { leave(o.id, ts, 'D'); }
  template <class R> void pre_replace(R&, Order& o, const itch::OrderReplace& m) { leave(o.id, m.h.ts, 'U'); }
  void leave(uint64_t id, uint64_t ts, char why) {
    auto it = live.find(id);
    if (it == live.end()) return;
    rows[it->second].exit_ts = ts;
    rows[it->second].exit = why;
    live.erase(it);
  }
  template <class R> void touched(R& r, uint16_t loc, uint64_t ts) {
    if (!tracked(loc)) return;
    auto bb = r.book.best_bid(loc);
    auto ba = r.book.best_ask(loc);
    const std::pair<uint32_t, uint32_t> q{bb ? bb->price : 0, ba ? ba->price : 0};
    if (q == last_quote[loc]) return;
    last_quote[loc] = q;
    std::fprintf(mids, "%u,%llu,%u,%u,%d\n", loc, (unsigned long long)ts, q.first, q.second,
                 int(r.continuous(loc, ts)));
  }
};

// First pass: map the requested symbols to locates.
struct Directory {
  std::vector<uint8_t>* w;
  const std::set<std::string>* s;
  void on(const itch::StockDirectory& m) { if (s->count(std::string(m.stock.view()))) (*w)[m.h.locate] = 1; }
  template <class M> void on(const M&) {}
};

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: nfe_queue <itch-file> --out DIR [--symbols A,B] [--day LABEL]\n"); return 2; }
  std::string out = "out/queue", day = "day";
  std::set<std::string> syms;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!std::strcmp(argv[i], "--day") && i + 1 < argc) day = argv[++i];
    else if (!std::strcmp(argv[i], "--symbols") && i + 1 < argc) {
      std::stringstream ss(argv[++i]);
      for (std::string s; std::getline(ss, s, ',');) syms.insert(s);
    }
  }
  std::filesystem::create_directories(out);
  MappedFile f(argv[1]);
  Tracker t;
  t.all = syms.empty();
  t.fills = std::fopen((out + "/fills.csv").c_str(), "w");
  t.mids = std::fopen((out + "/mids.csv").c_str(), "w");
  std::fprintf(t.fills, "ref,locate,ts_ns,side,price,qty,printable\n");
  std::fprintf(t.mids, "locate,ts_ns,bid,ask,continuous\n");

  // Stock Directory messages come first in the file, so the first 64 MB are enough.
  if (!t.all) {
    Directory d{&t.want, &syms};
    itch::parse_stream(f.data(), std::min<size_t>(f.size(), 64 << 20), d);
  }
  auto r = std::make_unique<Replayer<BaselineBook, Tracker>>(&t);
  itch::parse_stream(f.data(), f.size(), *r);
  std::fclose(t.fills);
  std::fclose(t.mids);

  FILE* o = std::fopen((out + "/orders.csv").c_str(), "w");
  std::fprintf(o, "day,symbol,ref,locate,ts_ns,side,price,qty,ahead,level_after,spread_ticks,imbalance,improved,"
                  "first_fill_ts,full_fill_ts,exit_ts,filled,cancelled,exit\n");
  for (auto& w : t.rows)
    std::fprintf(o, "%s,%s,%llu,%u,%llu,%c,%u,%u,%llu,%llu,%u,%.6f,%d,%llu,%llu,%llu,%u,%u,%c\n", day.c_str(),
                 r->names[w.loc].c_str(), (unsigned long long)w.ref, w.loc, (unsigned long long)w.ts, w.side, w.price,
                 w.qty, (unsigned long long)w.ahead, (unsigned long long)w.level_after, w.spread_ticks, w.imbalance,
                 int(w.improved), (unsigned long long)w.first_fill_ts, (unsigned long long)w.full_fill_ts,
                 (unsigned long long)w.exit_ts, w.filled, w.cancelled, w.exit);
  std::fclose(o);
  std::printf("tracked %zu orders at the touch\n", t.rows.size());
  return 0;
}
