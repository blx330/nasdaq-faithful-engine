// Out-of-order (late) Adds: when they happen and which symbols have them.
//
//   nfe_lateadds <itch-file> [--out DIR]
//
// Replays with entry-order priority (the default of nfe_fidelity) and records
// every Add that lands ahead of an order already resting at its level.
// Writes DIR/late.csv (one row per late insert), DIR/symbols.csv (Stock
// Directory fields plus Add counts per symbol), DIR/crosses.csv and
// DIR/states.csv (every trading-state change), so that late-insert times can
// be measured from each symbol's own opening cross and halts can be told apart.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "nfe/io.hpp"
#include "nfe/itch.hpp"
#include "nfe/replay.hpp"

using namespace nfe;

struct LateObs {
  FILE* late = nullptr;
  std::vector<uint64_t> n_late = std::vector<uint64_t>(65536, 0);
  void rec(const Order& o) {
    if (!o.late) return;
    ++n_late[o.locate];
    std::fprintf(late, "%u,%llu,%llu,%c,%u\n", unsigned(o.locate), (unsigned long long)o.ts,
                 (unsigned long long)o.id, o.side, unsigned(o.price));
  }
  template <class R> void pre_add(R&, const itch::AddOrder&) {}
  template <class R> void post_add(R&, Order& o) { rec(o); }
  template <class R> void pre_exec(R&, Order&, uint32_t, uint32_t, uint64_t, bool) {}
  template <class R> void pre_cancel(R&, Order&, uint32_t, uint64_t) {}
  template <class R> void pre_delete(R&, Order&, uint64_t) {}
  template <class R> void pre_replace(R&, Order&, const itch::OrderReplace&) {}
  template <class R> void post_replace(R&, Order& o) { rec(o); }
  template <class R> void on_trade(R&, const itch::Trade&) {}
  template <class R> void on_cross(R&, const itch::CrossTrade&) {}
  template <class R> void touched(R&, uint16_t, uint64_t) {}
};

struct Meta { char cat = ' ', fsi = ' ', cls = ' ', etp = ' ', ipo = ' '; uint32_t lot = 0; };

struct Handler {
  LateObs obs;
  Replayer<BaselineBook, LateObs> r{&obs};
  std::vector<Meta> meta = std::vector<Meta>(65536);
  std::vector<uint64_t> adds = std::vector<uint64_t>(65536, 0);
  FILE* states = nullptr;
  void on(const itch::StockDirectory& m) {
    Meta& t = meta[m.h.locate];
    t.cat = m.market_category; t.fsi = m.fsi; t.cls = m.issue_class; t.etp = m.etp_flag; t.ipo = m.ipo_flag; t.lot = m.round_lot;
    r.on(m);
  }
  void on(const itch::StockTradingAction& m) {
    std::fprintf(states, "%u,%llu,%c\n", unsigned(m.h.locate), (unsigned long long)m.h.ts, m.state);
    r.on(m);
  }
  void on(const itch::AddOrder& m) { ++adds[m.h.locate]; r.on(m); }
  template <class M> void on(const M& m) { r.on(m); }
};

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: nfe_lateadds <itch-file> [--out DIR]\n"); return 2; }
  std::string out = "out";
  for (int i = 2; i < argc; ++i)
    if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
  std::filesystem::create_directories(out);

  MappedFile f(argv[1]);
  auto h = std::make_unique<Handler>();
  h->r.book.priority_by_ref = true;
  h->obs.late = std::fopen((out + "/late.csv").c_str(), "w");
  h->states = std::fopen((out + "/states.csv").c_str(), "w");
  std::fprintf(h->obs.late, "locate,ts_ns,ref,side,price\n");
  std::fprintf(h->states, "locate,ts_ns,state\n");
  auto st = itch::parse_stream(f.data(), f.size(), *h);
  std::fclose(h->obs.late);
  std::fclose(h->states);

  FILE* s = std::fopen((out + "/symbols.csv").c_str(), "w");
  std::fprintf(s, "locate,symbol,market_category,fsi,issue_class,etp,ipo_flag,round_lot,adds,late\n");
  for (int loc = 1; loc < 65536; ++loc) {
    if (h->r.names[loc].empty()) continue;
    const Meta& t = h->meta[loc];
    std::fprintf(s, "%d,%s,%c,%c,%c,%c,%c,%u,%llu,%llu\n", loc, h->r.names[loc].c_str(), t.cat, t.fsi, t.cls, t.etp, t.ipo,
                 t.lot, (unsigned long long)h->adds[loc], (unsigned long long)h->obs.n_late[loc]);
  }
  std::fclose(s);
  FILE* c = std::fopen((out + "/crosses.csv").c_str(), "w");
  std::fprintf(c, "locate,symbol,type,ts_ns,price,shares\n");
  for (auto& [loc, v] : h->r.crosses)
    for (auto& p : v)
      std::fprintf(c, "%u,%s,%c,%llu,%u,%llu\n", unsigned(loc), h->r.names[loc].c_str(), p.type, (unsigned long long)p.ts,
                   unsigned(p.price), (unsigned long long)p.shares);
  std::fclose(c);
  std::printf("messages=%llu late_inserts=%llu unknown_ref=%llu\n", (unsigned long long)st.messages,
              (unsigned long long)h->r.book.late_inserts, (unsigned long long)h->r.c.unknown_ref);
  return 0;
}
