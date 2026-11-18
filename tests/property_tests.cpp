// Property tests on the C++ book: random operations, invariants checked after
// every one. These are independent of the OCaml spec, so a bug that the spec
// and the engine happen to share can still be caught here.
//
//   property_tests [operations] [seed]
//
// Prints "ops_to_detection=N" and exits 1 at the first violation.
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

#include "nfe/book.hpp"
#include "nfe/matcher.hpp"

using namespace nfe;

struct Model {  // what we expect each live order to hold
  char side;
  uint32_t price, qty;
  uint64_t seq;  // time priority
};

static bool check(BaselineBook& b, std::map<uint64_t, Model>& live, const char*& why) {
  // Every level: qty and count equal the sum of its orders; FIFO by seq; links consistent.
  uint64_t seen = 0;
  for (char side : {'B', 'S'}) {
    bool ok = true;
    uint32_t prev_px = 0;
    bool first_lv = true;
    b.walk(1, side, [&](const Level& lv) {
      if (!first_lv && (side == 'B' ? lv.price >= prev_px : lv.price <= prev_px)) { why = "levels out of order"; ok = false; }
      first_lv = false; prev_px = lv.price;
      uint64_t q = 0; uint32_t n = 0; uint64_t last_seq = 0; const Order* prev = nullptr;
      for (const Order* o = lv.head; o; prev = o, o = o->next) {
        auto it = live.find(o->id);
        if (it == live.end()) { why = "order in book but not live"; ok = false; return false; }
        if (o->qty != it->second.qty || o->price != lv.price || o->side != side) { why = "order fields wrong"; ok = false; }
        if (it->second.seq < last_seq) { why = "time priority violated"; ok = false; }
        if (o->prev != prev || o->level != &lv) { why = "links broken"; ok = false; }
        if (o->qty == 0) { why = "zero-size order resting"; ok = false; }
        last_seq = it->second.seq; q += o->qty; ++n;
      }
      if (lv.tail != prev) { why = "tail wrong"; ok = false; }
      if (q != lv.qty || n != lv.count || n == 0) { why = "level totals wrong"; ok = false; }
      seen += n;
      return ok;
    });
    if (!ok) return false;
  }
  if (seen != live.size() || b.order_count() != live.size()) { why = "order count mismatch"; return false; }
  auto bb = b.best_bid(1), ba = b.best_ask(1);
  if (bb && ba && bb->price >= ba->price) { why = "book crossed"; return false; }
  return true;
}

int main(int argc, char** argv) {
  const uint64_t ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
  const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  std::mt19937_64 rng(seed);
  auto r = [&](uint64_t n) { return rng() % n; };

  uint64_t done = 0;
  while (done < ops) {
    BaselineBook b(2);
    std::map<uint64_t, Model> live;
    uint64_t next_id = 1, seq = 0;
    const uint32_t base = 1000 + uint32_t(r(1000));
    for (int k = 0; k < 300 && done < ops; ++k, ++done) {
      const char* why = nullptr;
      const uint64_t roll = r(100);
      std::vector<uint64_t> keys;
      for (auto& [id, m] : live) keys.push_back(id);
      if (roll < 55 || keys.empty()) {
        const char side = r(2) ? 'B' : 'S';
        const uint32_t px = base + uint32_t(r(7)) - 3;
        const uint32_t qty = 1 + uint32_t(r(5)) * 100;
        // Expected fills from the model: opposite orders that cross, by price then seq.
        std::vector<std::pair<std::pair<uint32_t, uint64_t>, uint64_t>> cand;
        for (auto& [id, m] : live)
          if (m.side != side && crosses(side, px, m.price))
            cand.push_back({{side == 'B' ? m.price : ~m.price, m.seq}, id});
        std::sort(cand.begin(), cand.end());
        std::vector<Fill> want;
        uint32_t left = qty;
        for (auto& c : cand) {
          if (!left) break;
          auto& m = live[c.second];
          uint32_t q = std::min(m.qty, left);
          want.push_back(Fill{c.second, m.price, q});
          left -= q;
        }
        std::vector<Fill> got;
        uint32_t rested = submit(b, 1, next_id, side, px, qty, ++seq, got);
        if (got != want) why = "fills differ from the model";
        uint64_t filled = 0;
        for (auto& f : got) {
          if (!crosses(side, px, f.price)) why = "fill outside limit";
          filled += f.qty;
        }
        if (filled + rested != qty) why = "aggressor shares not conserved";
        for (auto& f : want) { auto& m = live[f.resting_id]; m.qty -= f.qty; if (!m.qty) live.erase(f.resting_id); }
        if (left) live[next_id] = Model{side, px, left, seq};
        ++next_id;
      } else if (roll < 75) {
        const uint64_t id = keys[r(keys.size())];
        auto& m = live[id];
        const uint32_t q = 1 + uint32_t(r(m.qty));
        if (q < m.qty) { b.reduce(id, q); m.qty -= q; }   // keeps priority: seq unchanged
        else { b.remove(id); live.erase(id); }
      } else if (roll < 87) {
        const uint64_t id = keys[r(keys.size())];
        b.remove(id); live.erase(id);
      } else {
        // Replace without crossing (as ITCH replaces do): goes to the back.
        const uint64_t id = keys[r(keys.size())];
        Model m = live[id];
        uint32_t px = m.price;
        auto opp = m.side == 'B' ? b.best_ask(1) : b.best_bid(1);
        const uint32_t cand_px = m.price + uint32_t(r(3)) - 1;
        if (!opp || !crosses(m.side, cand_px, opp->price)) px = cand_px;
        const uint32_t q = 1 + uint32_t(r(4)) * 100;
        if (b.replace(id, next_id, px, q, ++seq) != Result::Ok) why = "replace failed";
        live.erase(id);
        live[next_id] = Model{m.side, px, q, seq};
        ++next_id;
      }
      if (!why) check(b, live, why);
      if (why) {
        std::printf("property violated: %s\nops_to_detection=%llu\n", why, (unsigned long long)(done + 1));
        return 1;
      }
    }
  }
  std::printf("%llu operations, all properties held\n", (unsigned long long)ops);
  return 0;
}
