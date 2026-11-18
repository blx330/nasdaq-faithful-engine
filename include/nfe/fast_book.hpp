// Final design, step 1: direct-indexed order lookup.
//
// Same interface and level structure as BaselineBook; only order lookup
// changes. ITCH reference numbers rise through the day, so orders are stored
// in fixed-size pages indexed by `ref >> kPageBits`. Lookup is two loads and
// no hashing. A page is freed once every order in it has left the book and it
// is behind the newest page, so memory follows the live order count rather
// than the highest reference number.
//
// Each further change (level structure, slot layout, prefetch) is a separate
// commit with a prediction and a before/after measurement: docs/perf-log.md.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "book.hpp"

namespace nfe {

class PagedOrders {
 public:
  static constexpr int kPageBits = 12;
  static constexpr uint64_t kPageSize = 1ULL << kPageBits;

  Order* find(uint64_t id) {
    const uint64_t p = id >> kPageBits;
    if (p >= pages_.size() || !pages_[p]) return nullptr;
    Order& o = pages_[p]->slots[id & (kPageSize - 1)];
    return o.level ? &o : nullptr;  // live orders always sit in a level
  }
  // Slot for a new order, or nullptr if the id is live already.
  Order* claim(uint64_t id) {
    const uint64_t p = id >> kPageBits;
    if (p >= pages_.size()) pages_.resize(p + 1);
    if (!pages_[p]) pages_[p] = std::make_unique<Page>();
    Order& o = pages_[p]->slots[id & (kPageSize - 1)];
    if (o.level) return nullptr;
    ++pages_[p]->live;
    ++size_;
    newest_ = std::max(newest_, p);
    return &o;
  }
  void release(Order* o) {
    const uint64_t p = o->id >> kPageBits;
    o->level = nullptr;
    --size_;
    if (--pages_[p]->live == 0 && p + 1 < newest_) pages_[p].reset();
  }
  size_t size() const { return size_; }

 private:
  struct Page {
    Order slots[kPageSize]{};
    uint32_t live = 0;
  };
  std::vector<std::unique_ptr<Page>> pages_;
  size_t size_ = 0;
  uint64_t newest_ = 0;
};

class FastBook {
 public:
  using Bids = std::map<uint32_t, Level, std::greater<uint32_t>>;
  using Asks = std::map<uint32_t, Level, std::less<uint32_t>>;
  struct Sym { Bids bids; Asks asks; };

  explicit FastBook(size_t n_locates = 65536) : syms_(n_locates) {}

  Order* find(uint64_t id) { return orders_.find(id); }
  size_t order_count() const { return orders_.size(); }

  Result add(uint16_t loc, uint64_t id, char side, uint32_t px, uint32_t qty, uint64_t ts) {
    Order* o = orders_.claim(id);
    if (!o) return Result::DuplicateRef;
    o->id = id; o->ts = ts; o->price = px; o->qty = qty; o->locate = loc; o->side = side;
    Level& lv = side == 'B' ? level_at(syms_[loc].bids, px) : level_at(syms_[loc].asks, px);
    lv.push_back(o);
    return Result::Ok;
  }
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
  template <class F>
  void walk(uint16_t loc, char side, F&& f) const {
    if (side == 'B') { for (auto& [p, lv] : syms_[loc].bids) if (!f(lv)) return; }
    else { for (auto& [p, lv] : syms_[loc].asks) if (!f(lv)) return; }
  }
  uint64_t level_qty(uint16_t loc, char side, uint32_t px) const {
    if (side == 'B') { auto it = syms_[loc].bids.find(px); return it == syms_[loc].bids.end() ? 0 : it->second.qty; }
    auto it = syms_[loc].asks.find(px); return it == syms_[loc].asks.end() ? 0 : it->second.qty;
  }

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
    const char side = o->side;
    const uint16_t loc = o->locate;
    lv->unlink(o);
    if (lv->count == 0) {
      if (side == 'B') syms_[loc].bids.erase(px);
      else syms_[loc].asks.erase(px);
    }
    orders_.release(o);
  }

  std::vector<Sym> syms_;
  PagedOrders orders_;
};

}  // namespace nfe
