// Baseline limit order book: std::map of price levels per side and
// std::unordered_map of orders. Each level holds an intrusive FIFO of its
// orders, so time priority is the list order.
//
// This is the "baseline" design from the spec. It is deliberately plain; the
// performance phase replaces pieces of it one change at a time.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nfe {

struct Level;

struct Order {
  uint64_t id = 0;
  uint64_t ts = 0;       // time the order took its current queue position
  uint32_t price = 0;
  uint32_t qty = 0;      // remaining displayed shares
  uint16_t locate = 0;
  char side = 'B';       // 'B' or 'S'
  Level* level = nullptr;
  Order* prev = nullptr;
  Order* next = nullptr;
};

struct Level {
  uint32_t price = 0;
  uint64_t qty = 0;      // total displayed shares at this price
  uint32_t count = 0;    // number of orders
  Order* head = nullptr; // oldest order: first to fill
  Order* tail = nullptr;

  void push_back(Order* o) {
    o->prev = tail; o->next = nullptr; o->level = this;
    if (tail) tail->next = o; else head = o;
    tail = o; qty += o->qty; ++count;
  }
  void unlink(Order* o) {
    if (o->prev) o->prev->next = o->next; else head = o->next;
    if (o->next) o->next->prev = o->prev; else tail = o->prev;
    qty -= o->qty; --count;
    o->prev = o->next = nullptr; o->level = nullptr;
  }
};

struct Quote { uint32_t price; uint64_t qty; };

enum class Result { Ok, UnknownRef, DuplicateRef, Overfill };

class BaselineBook {
 public:
  using Bids = std::map<uint32_t, Level, std::greater<uint32_t>>;
  using Asks = std::map<uint32_t, Level, std::less<uint32_t>>;
  struct Sym { Bids bids; Asks asks; };

  explicit BaselineBook(size_t n_locates = 65536) : syms_(n_locates) {}

  Order* find(uint64_t id) {
    auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
  }
  size_t order_count() const { return orders_.size(); }

  Result add(uint16_t loc, uint64_t id, char side, uint32_t px, uint32_t qty, uint64_t ts) {
    auto [it, fresh] = orders_.try_emplace(id);
    if (!fresh) return Result::DuplicateRef;
    Order& o = it->second;
    o.id = id; o.ts = ts; o.price = px; o.qty = qty; o.locate = loc; o.side = side;
    Level& lv = side == 'B' ? level_at(syms_[loc].bids, px) : level_at(syms_[loc].asks, px);
    lv.push_back(&o);
    return Result::Ok;
  }

  // Remove `qty` shares from an order in place. Priority is kept; the order
  // is deleted when it reaches zero. Used for executions and partial cancels.
  Result reduce(uint64_t id, uint32_t qty) {
    Order* o = find(id);
    if (!o) return Result::UnknownRef;
    if (qty > o->qty) return Result::Overfill;
    o->qty -= qty;
    o->level->qty -= qty;
    if (o->qty == 0) erase(o);
    return Result::Ok;
  }

  Result remove(uint64_t id) {
    Order* o = find(id);
    if (!o) return Result::UnknownRef;
    erase(o);
    return Result::Ok;
  }

  // ITCH Replace: the new order goes to the back of the queue at its price,
  // even if the price is unchanged.
  Result replace(uint64_t old_id, uint64_t new_id, uint32_t px, uint32_t qty, uint64_t ts) {
    Order* o = find(old_id);
    if (!o) return Result::UnknownRef;
    if (find(new_id)) return Result::DuplicateRef;
    const uint16_t loc = o->locate;
    const char side = o->side;
    erase(o);
    return add(loc, new_id, side, px, qty, ts);
  }

  std::optional<Quote> best_bid(uint16_t loc) const { return best(syms_[loc].bids); }
  std::optional<Quote> best_ask(uint16_t loc) const { return best(syms_[loc].asks); }

  // Visit levels on one side from the best price outward. `f(const Level&)`
  // returns false to stop.
  template <class F>
  void walk(uint16_t loc, char side, F&& f) const {
    if (side == 'B') { for (auto& [p, lv] : syms_[loc].bids) if (!f(lv)) return; }
    else { for (auto& [p, lv] : syms_[loc].asks) if (!f(lv)) return; }
  }

  // Displayed shares at a price on a side (0 if no level).
  uint64_t level_qty(uint16_t loc, char side, uint32_t px) const {
    if (side == 'B') { auto it = syms_[loc].bids.find(px); return it == syms_[loc].bids.end() ? 0 : it->second.qty; }
    auto it = syms_[loc].asks.find(px); return it == syms_[loc].asks.end() ? 0 : it->second.qty;
  }

  const Sym& sym(uint16_t loc) const { return syms_[loc]; }

 private:
  template <class M>
  static Level& level_at(M& m, uint32_t px) {
    auto [it, fresh] = m.try_emplace(px);
    if (fresh) it->second.price = px;
    return it->second;
  }
  template <class M>
  static std::optional<Quote> best(const M& m) {
    if (m.empty()) return std::nullopt;
    return Quote{m.begin()->first, m.begin()->second.qty};
  }
  void erase(Order* o) {
    Level* lv = o->level;
    const uint32_t px = lv->price;
    const uint64_t id = o->id;  // copy: erasing by a reference into the node is unsafe
    lv->unlink(o);
    if (lv->count == 0) {
      if (o->side == 'B') syms_[o->locate].bids.erase(px);
      else syms_[o->locate].asks.erase(px);
    }
    orders_.erase(id);
  }

  std::vector<Sym> syms_;
  std::unordered_map<uint64_t, Order> orders_;
};

}  // namespace nfe
