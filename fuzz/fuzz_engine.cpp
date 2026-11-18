// libFuzzer target: bytes decoded into book commands, with the book's
// structural invariants checked after every command.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "nfe/book.hpp"
#include "nfe/matcher.hpp"

using namespace nfe;

static void invariants(const BaselineBook& b) {
  for (char side : {'B', 'S'})
    b.walk(1, side, [&](const Level& lv) {
      uint64_t q = 0; uint32_t n = 0;
      for (const Order* o = lv.head; o; o = o->next) { q += o->qty; ++n; if (!o->qty) __builtin_trap(); }
      if (q != lv.qty || n != lv.count || n == 0) __builtin_trap();
      return true;
    });
  auto bb = b.best_bid(1), ba = b.best_ask(1);
  if (bb && ba && bb->price >= ba->price) __builtin_trap();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
  auto b = std::make_unique<BaselineBook>(2);
  std::vector<Fill> fills;
  uint64_t next_id = 1, ts = 0;
  for (size_t i = 0; i + 4 <= n; i += 4) {
    const uint8_t op = d[i] % 4, a = d[i + 1], c = d[i + 2], q = d[i + 3];
    const uint64_t target = 1 + a % (next_id);
    switch (op) {
      case 0: {  // submit
        const char side = c & 1 ? 'B' : 'S';
        const uint32_t px = 100 + (c >> 1) % 8;
        if (q) submit(*b, 1, next_id, side, px, q, ++ts, fills);
        ++next_id;
        break;
      }
      case 1: {  // partial or full cancel
        if (Order* o = b->find(target)) {
          if (q && q < o->qty) b->reduce(target, q); else b->remove(target);
        }
        break;
      }
      case 2: b->remove(target); break;
      case 3: {  // replace without crossing
        if (Order* o = b->find(target)) {
          const char side = o->side;
          const uint32_t px = 100 + c % 8;
          auto opp = side == 'B' ? b->best_ask(1) : b->best_bid(1);
          if (q && !(opp && crosses(side, px, opp->price))) b->replace(target, next_id, px, q, ++ts);
        }
        ++next_id;
        break;
      }
    }
    invariants(*b);
    fills.clear();
  }
  return 0;
}
