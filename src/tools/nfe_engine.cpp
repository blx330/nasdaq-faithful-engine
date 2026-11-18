// The C++ engine behind the shared text protocol, for differential testing
// against the OCaml spec (spec/bin/spec_engine.ml). Same commands, same output:
//
//   A <id> <B|S> <price> <qty>      submit a limit order (match, then rest)
//   X <id> <qty>                    cancel shares; cancelling all deletes
//   D <id>                          delete
//   U <old> <new> <price> <qty>     replace: delete old, submit new
//   #                               dump the book, print "#", start fresh
//
// Output: "T <aggressor> <resting> <price> <qty>" per fill, "R <id>" per
// reject, and "L <side> <price> <id>:<qty> ..." per level, best first.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nfe/book.hpp"
#include "nfe/matcher.hpp"

using namespace nfe;
using Book = BaselineBook;

static std::string out;

static void emit_fills(uint64_t aggr, const std::vector<Fill>& fills) {
  char buf[96];
  for (auto& f : fills) {
    std::snprintf(buf, sizeof buf, "T %llu %llu %u %u\n", (unsigned long long)aggr,
                  (unsigned long long)f.resting_id, f.price, f.qty);
    out += buf;
  }
}
static void reject(uint64_t id) { out += "R " + std::to_string(id) + "\n"; }

static void dump(const Book& b) {
  for (char side : {'B', 'S'})
    b.walk(1, side, [&](const Level& lv) {
      out += std::string("L ") + side + " " + std::to_string(lv.price);
      for (const Order* o = lv.head; o; o = o->next)
        out += " " + std::to_string(o->id) + ":" + std::to_string(o->qty);
      out += "\n";
      return true;
    });
}

static void do_submit(Book& b, uint64_t id, char side, long long px, long long qty, uint64_t& ts) {
  if (qty <= 0 || px <= 0 || b.find(id) || (side != 'B' && side != 'S')) { reject(id); return; }
  std::vector<Fill> fills;
  submit(b, 1, id, side, uint32_t(px), uint32_t(qty), ++ts, fills);
  emit_fills(id, fills);
}

int main() {
  auto book = std::make_unique<Book>(2);
  uint64_t ts = 0;
  char line[256];
  while (std::fgets(line, sizeof line, stdin)) {
    char cmd = 0, side = 0;
    unsigned long long a = 0, b = 0;
    long long p = 0, q = 0;
    if (line[0] == '#') {
      dump(*book);
      out += "#\n";
      book = std::make_unique<Book>(2);
    } else if (std::sscanf(line, "A %llu %c %lld %lld", &a, &side, &p, &q) == 4) {
      do_submit(*book, a, side, p, q, ts);
    } else if (std::sscanf(line, "X %llu %lld", &a, &q) == 2) {
      Order* o = book->find(a);
      if (!o || q <= 0) reject(a);
      else if (uint64_t(q) >= o->qty) book->remove(a);
      else book->reduce(a, uint32_t(q));
    } else if (std::sscanf(line, "D %llu", &a) == 1) {
      if (book->remove(a) != Result::Ok) reject(a);
    } else if (std::sscanf(line, "U %llu %llu %lld %lld", &a, &b, &p, &q) == 4) {
      Order* o = book->find(a);
      if (!o || q <= 0 || p <= 0 || (b != a && book->find(b))) { reject(a); continue; }
      cmd = o->side;
      book->remove(a);
      do_submit(*book, b, cmd, p, q, ts);
    }
    if (out.size() > (1 << 20)) { std::fwrite(out.data(), 1, out.size(), stdout); out.clear(); }
  }
  dump(*book);
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}
