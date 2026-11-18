// Matching mode: price-time priority against the displayed book.
//
// `predict` answers "if this limit order arrived now, which resting orders
// would it hit, in what order, for how many shares?" without changing the
// book. The fidelity pipeline calls it on the pre-event book and compares the
// answer with Nasdaq's executions. `submit` applies the same prediction and
// rests any remainder; the differential fuzzer drives it against the OCaml spec.
#pragma once
#include <cstdint>
#include <vector>

namespace nfe {

struct Fill {
  uint64_t resting_id;
  uint32_t price;
  uint32_t qty;
  bool operator==(const Fill&) const = default;
};

inline char opposite(char side) { return side == 'B' ? 'S' : 'B'; }

// True if an aggressor on `aggr_side` with limit `limit` trades at `px`.
inline bool crosses(char aggr_side, uint32_t limit, uint32_t px) {
  return aggr_side == 'B' ? px <= limit : px >= limit;
}

template <class Book>
void predict(const Book& book, uint16_t loc, char aggr_side, uint32_t limit, uint32_t qty,
             std::vector<Fill>& out) {
  uint32_t left = qty;
  book.walk(loc, opposite(aggr_side), [&](const auto& lv) {
    if (left == 0 || !crosses(aggr_side, limit, lv.price)) return false;
    for (const auto* o = lv.head; o && left > 0; o = o->next) {
      const uint32_t q = o->qty < left ? o->qty : left;
      out.push_back(Fill{o->id, lv.price, q});
      left -= q;
    }
    return left > 0;
  });
}

// Match an incoming limit order, then rest the remainder at its limit.
// Returns the shares rested.
template <class Book>
uint32_t submit(Book& book, uint16_t loc, uint64_t id, char side, uint32_t px, uint32_t qty,
                uint64_t ts, std::vector<Fill>& fills) {
  const size_t first = fills.size();
  predict(book, loc, side, px, qty, fills);
  uint32_t filled = 0;
  for (size_t i = first; i < fills.size(); ++i) {
    book.reduce(fills[i].resting_id, fills[i].qty);
    filled += fills[i].qty;
  }
  const uint32_t rest = qty - filled;
  if (rest > 0) book.add(loc, id, side, px, rest, ts);
  return rest;
}

}  // namespace nfe
