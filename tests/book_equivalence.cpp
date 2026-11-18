// The final book must behave exactly like the baseline. Two checks:
//   1. random operations on both, full books compared every 50 operations;
//   2. optionally, an ITCH file replayed into both, counters and every
//      symbol's book compared at the end.
//
//   book_equivalence [operations] [itch-file]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "nfe/fast_book.hpp"
#include "nfe/io.hpp"
#include "nfe/matcher.hpp"
#include "nfe/replay.hpp"

using namespace nfe;

template <class B>
static std::string dump(const B& b, uint16_t loc) {
  std::string s;
  for (char side : {'B', 'S'})
    b.walk(loc, side, [&](const Level& lv) {
      s += side + std::to_string(lv.price) + "=" + std::to_string(lv.qty) + ":";
      for (const Order* o = lv.head; o; o = o->next) s += std::to_string(o->id) + "/" + std::to_string(o->qty) + ",";
      s += ";";
      return true;
    });
  return s;
}

int main(int argc, char** argv) {
  const uint64_t ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
  std::mt19937_64 rng(5);
  auto r = [&](uint64_t n) { return rng() % n; };
  auto a = std::make_unique<BaselineBook>(4);
  auto b = std::make_unique<FastBook>(4);
  uint64_t next = 1;
  std::vector<uint64_t> ids;
  for (uint64_t i = 0; i < ops; ++i) {
    const uint64_t k = r(100);
    if (k < 50 || ids.empty()) {
      const char side = r(2) ? 'B' : 'S';
      const uint32_t px = 1000 + uint32_t(r(9));
      const uint32_t q = 1 + uint32_t(r(500));
      std::vector<Fill> fa, fb;
      submit(*a, 1, next, side, px, q, i, fa);
      submit(*b, 1, next, side, px, q, i, fb);
      if (fa != fb) { std::printf("fills differ at op %llu\n", (unsigned long long)i); return 1; }
      ids.push_back(next);
      next += 1 + r(3);  // gaps, like real reference numbers
    } else {
      const uint64_t id = ids[r(ids.size())];
      const uint32_t q = 1 + uint32_t(r(300));
      Result ra, rb;
      if (k < 75) { ra = a->reduce(id, q); rb = b->reduce(id, q); }
      else if (k < 90) { ra = a->remove(id); rb = b->remove(id); }
      else {
        const uint32_t px = 1000 + uint32_t(r(9));
        Order* o = a->find(id);
        auto opp = o ? (o->side == 'B' ? a->best_ask(1) : a->best_bid(1)) : std::nullopt;
        if (o && opp && crosses(o->side, px, opp->price)) continue;  // ITCH replaces never cross
        ra = a->replace(id, next, px, q, i); rb = b->replace(id, next, px, q, i);
        ids.push_back(next); next += 1;
      }
      if (ra != rb) { std::printf("results differ at op %llu\n", (unsigned long long)i); return 1; }
    }
    if (i % 50 == 0 && (dump(*a, 1) != dump(*b, 1) || a->order_count() != b->order_count())) {
      std::printf("books differ at op %llu\n", (unsigned long long)i);
      return 1;
    }
  }
  std::printf("%llu random operations: books identical\n", (unsigned long long)ops);

  if (argc > 2) {
    MappedFile f(argv[2]);
    auto ra = std::make_unique<Replayer<BaselineBook>>();
    auto rb = std::make_unique<Replayer<FastBook>>();
    itch::parse_stream(f.data(), f.size(), *ra);
    itch::parse_stream(f.data(), f.size(), *rb);
    if (std::memcmp(&ra->c, &rb->c, sizeof ra->c) != 0) { std::printf("replay counters differ\n"); return 1; }
    for (int loc = 1; loc < 65536; ++loc)
      if (!ra->names[loc].empty() && dump(ra->book, uint16_t(loc)) != dump(rb->book, uint16_t(loc))) {
        std::printf("replayed books differ for %s\n", ra->names[loc].c_str());
        return 1;
      }
    std::printf("replay of %s: counters and books identical\n", argv[2]);
  }
  return 0;
}
