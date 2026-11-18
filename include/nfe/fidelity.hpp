// Phase 2: does the engine, in matching mode, reproduce Nasdaq's fills?
//
// ITCH shows resting orders and the executions against them, never the
// incoming order. This handler recovers the incoming order ("aggressor")
// from each group of executions, rewinds to the book just before them (by
// buffering: the group is not applied until it is complete), asks the
// matcher what that order would have filled, and classifies any difference.
//
// Grouping rule (a heuristic, tested on synthetic data with ground truth):
// consecutive E, C and P messages for one symbol with the same timestamp and
// the same resting side form one event; an Add on the opposite side at the
// same timestamp that follows them is the aggressor's posted remainder.
#pragma once
#include <algorithm>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <string>
#include <vector>

#include "book.hpp"
#include "matcher.hpp"
#include "replay.hpp"

namespace nfe {

enum class Cls : uint8_t {
  Match,            // predicted fills identical to the feed
  MatchHidden,      // displayed fills identical; P messages also in the event
  NonDisplayed,     // differs, and P messages (hidden liquidity) are in the event
  NonDisplayPrice,  // differs only by execution price; printable C in the event
  Cross,            // outside continuous trading, or non-printable C: a cross
  Grouping,         // matches once repeated fills of one order are merged: >1 aggressor
  Repriced,         // matches once the posted remainder is read as repriced, not as the limit
  HiddenOnly,       // only P messages: nothing displayed to predict
  Unexplained,
  N
};
inline const char* cls_name(Cls c) {
  static const char* n[] = {"match", "match_hidden", "non_displayed", "nondisplay_price", "cross",
                            "grouping", "remainder_repriced", "hidden_only", "unexplained"};
  return n[int(c)];
}

struct ExecMsg {
  char type;  // 'E', 'C', 'P'
  uint64_t ref;
  uint32_t qty;
  uint32_t price;  // C and P: execution price; E: filled in from the book
  bool printable;
  itch::OrderExecuted e;
  itch::OrderExecutedWithPrice c;
  itch::Trade p;
};

struct SymStats {
  uint64_t cls[int(Cls::N)]{};
  uint64_t one_tick_events = 0;  // pre-event spread was exactly one tick
  uint64_t same_ts_events = 0;   // another event in this symbol shares the timestamp
};

template <class Book = BaselineBook>
class Fidelity {
 public:
  Replayer<Book> r;
  std::vector<SymStats> per_sym = std::vector<SymStats>(65536);
  uint64_t total[int(Cls::N)]{};
  FILE* events_csv = nullptr;       // one row per event
  FILE* unexplained_txt = nullptr;  // details for every unexplained event
  uint64_t unexplained_logged = 0;
  uint64_t first_unexplained_msg = 0;
  uint64_t late_events = 0;         // events whose feed fills include an order placed by priority_by_ref

  // ---- ITCH handlers ----
  void on(const itch::OrderExecuted& m) {
    if (!joins(m.h, side_of(m.ref))) close();
    open_if_needed(m.h, side_of(m.ref));
    ExecMsg x{}; x.type = 'E'; x.ref = m.ref; x.qty = m.shares; x.printable = true; x.e = m;
    buf_.push_back(x);
  }
  void on(const itch::OrderExecutedWithPrice& m) {
    if (!joins(m.h, side_of(m.ref))) close();
    open_if_needed(m.h, side_of(m.ref));
    ExecMsg x{}; x.type = 'C'; x.ref = m.ref; x.qty = m.shares; x.price = m.price;
    x.printable = m.printable == 'Y'; x.c = m;
    buf_.push_back(x);
  }
  void on(const itch::Trade& m) {
    // P's side field is not reliable for the resting side, so it never splits an event.
    if (!joins(m.h, 0)) close();
    open_if_needed(m.h, 0);
    ExecMsg x{}; x.type = 'P'; x.ref = 0; x.qty = m.shares; x.price = m.price; x.printable = true; x.p = m;
    buf_.push_back(x);
  }
  void on(const itch::AddOrder& m) {
    if (open_ && m.h.locate == loc_ && m.h.ts == ts_ && is_remainder(m)) {
      remainder_ = m;
      has_remainder_ = true;
      close();
      return;
    }
    close();
    r.on(m);
  }
  template <class M> void on(const M& m) { close(); r.on(m); }

  void finish() { close(); }

 private:
  char side_of(uint64_t ref) {
    const Order* o = r.book.find(ref);
    return o ? o->side : 0;
  }
  // The Add is the aggressor's posted remainder if it is on the aggressor's
  // side. When only P messages were seen the resting side is unknown, but a
  // passive order can never rest at or through a price that just traded
  // against hidden liquidity, so an Add there must be the aggressor's.
  bool is_remainder(const itch::AddOrder& m) {
    if (rest_side_) return m.side == opposite(rest_side_);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (auto& x : buf_) { lo = std::min(lo, x.price); hi = std::max(hi, x.price); }
    if ((m.side == 'B' && m.price >= hi) || (m.side == 'S' && m.price <= lo)) {
      hidden_aggr_ = m.side;
      return true;
    }
    return false;
  }
  bool joins(const itch::Header& h, char side) const {
    if (!open_ || h.locate != loc_ || h.ts != ts_) return false;
    return side == 0 || rest_side_ == 0 || side == rest_side_;
  }
  void open_if_needed(const itch::Header& h, char side) {
    if (!open_) {
      open_ = true; loc_ = h.locate; ts_ = h.ts; rest_side_ = side;
      buf_.clear(); has_remainder_ = false; hidden_aggr_ = 0;
    } else if (rest_side_ == 0 && side != 0) {
      rest_side_ = side;
    }
  }

  void close() {
    if (!open_) return;
    open_ = false;
    classify();
    for (auto& x : buf_) {
      if (x.type == 'E') r.on(x.e);
      else if (x.type == 'C') r.on(x.c);
      else r.on(x.p);
    }
    if (has_remainder_) r.on(remainder_);
    buf_.clear();
    has_remainder_ = false;
  }

  void classify() {
    SymStats& ss = per_sym[loc_];
    const bool same_ts = loc_ == prev_loc_ && ts_ == prev_ts_;
    if (same_ts) ++ss.same_ts_events;
    prev_loc_ = loc_; prev_ts_ = ts_;

    // Feed side: displayed fills in order, plus flags.
    feed_.clear();
    bool has_p = false, has_c_print = false, has_c_nonprint = false, unknown = false, late = false;
    uint64_t total_qty = 0;
    uint32_t worst = 0;
    const char aggr = rest_side_ ? opposite(rest_side_) : hidden_aggr_;
    for (auto& x : buf_) {
      uint32_t px = x.price;
      if (x.type == 'P') { has_p = true; }
      else {
        const Order* o = r.book.find(x.ref);
        if (!o) { unknown = true; continue; }
        if (o->late) late = true;
        if (x.type == 'E') px = o->price;
        else if (x.printable) has_c_print = true;
        else has_c_nonprint = true;
        feed_.push_back(Fill{x.ref, o->price, x.qty});
      }
      total_qty += x.qty;
      if (aggr == 'B') worst = std::max(worst, px);
      else if (aggr == 'S') worst = worst == 0 ? px : std::min(worst, px);
    }

    Cls cls;
    uint32_t limit = 0;
    if (rest_side_ == 0) {
      cls = r.continuous(loc_, ts_) ? Cls::HiddenOnly : Cls::Cross;
      if (has_remainder_) { total_qty += remainder_.shares; limit = remainder_.price; }
      else limit = worst;
    } else if (!r.continuous(loc_, ts_) || has_c_nonprint) {
      cls = Cls::Cross;
    } else {
      if (has_remainder_) total_qty += remainder_.shares;
      limit = has_remainder_ ? remainder_.price : worst;
      // A remainder posted less aggressively than the aggressor's own fills
      // cannot be its limit: the order traded at `worst`, so its limit was at
      // least that. Seen on Nasdaq when the remainder is repriced to avoid
      // locking another market (Price to Comply). Predict with the fills' limit.
      bool repriced = false;
      if (has_remainder_ && !feed_.empty() && (aggr == 'B' ? limit < worst : limit > worst)) {
        limit = worst;
        repriced = true;
      }
      auto bb = r.book.best_bid(loc_);
      auto ba = r.book.best_ask(loc_);
      if (bb && ba && ba->price - bb->price == 100) ++ss.one_tick_events;
      pred_.clear();
      predict(r.book, loc_, aggr, limit, uint32_t(total_qty), pred_);
      const bool exact = !unknown && pred_ == feed_;
      if (exact && repriced && !has_p && !has_c_print) cls = Cls::Repriced;
      else if (exact && !has_p && !has_c_print) cls = Cls::Match;
      else if (exact && has_c_print) cls = Cls::NonDisplayPrice;
      else if (exact) cls = Cls::MatchHidden;
      else if (has_p) cls = Cls::NonDisplayed;
      else if (has_c_print) cls = Cls::NonDisplayPrice;
      else if (merged_matches(aggr, limit)) cls = Cls::Grouping;
      else cls = Cls::Unexplained;
    }
    ++ss.cls[int(cls)];
    ++total[int(cls)];
    if (late) ++late_events;
    if (events_csv)
      std::fprintf(events_csv, "%u,%llu,%c,%llu,%u,%zu,%d,%d,%d,%d,%s\n", loc_, (unsigned long long)ts_,
                   aggr ? aggr : '?', (unsigned long long)total_qty, limit, buf_.size(), has_p, has_c_print,
                   int(same_ts), int(late), cls_name(cls));
    if (cls == Cls::Unexplained && !first_unexplained_msg) first_unexplained_msg = r.c.n_msgs + 1;
    if (cls == Cls::Unexplained && unexplained_txt && unexplained_logged < 10000) log_unexplained(aggr, limit, total_qty);
  }

  // Two same-side aggressors at one timestamp walk the book in the same
  // order as one bigger aggressor, except that an order split between them
  // prints twice. Merge adjacent fills of the same order and compare again.
  bool merged_matches(char aggr, uint32_t limit) {
    std::vector<Fill> merged;
    for (auto& f : feed_) {
      if (!merged.empty() && merged.back().resting_id == f.resting_id) merged.back().qty += f.qty;
      else merged.push_back(f);
    }
    if (merged.size() == feed_.size()) return false;
    return merged == pred_;
  }

  void log_unexplained(char aggr, uint32_t limit, uint64_t qty) {
    ++unexplained_logged;
    std::fprintf(unexplained_txt, "event %s loc=%u ts=%llu aggressor=%c qty=%llu limit=%u remainder=%d\n",
                 r.names[loc_].c_str(), loc_, (unsigned long long)ts_, aggr, (unsigned long long)qty, limit,
                 int(has_remainder_));
    for (auto& f : feed_) std::fprintf(unexplained_txt, "  feed %llu %u@%u\n", (unsigned long long)f.resting_id, f.qty, f.price);
    for (auto& f : pred_) std::fprintf(unexplained_txt, "  pred %llu %u@%u\n", (unsigned long long)f.resting_id, f.qty, f.price);
  }

  bool open_ = false;
  uint16_t loc_ = 0;
  uint64_t ts_ = 0;
  char rest_side_ = 0;
  char hidden_aggr_ = 0;  // aggressor side found from a remainder, when only P was seen
  std::vector<ExecMsg> buf_;
  itch::AddOrder remainder_{};
  bool has_remainder_ = false;
  std::vector<Fill> feed_, pred_;
  uint16_t prev_loc_ = 0;
  uint64_t prev_ts_ = 0;
};

}  // namespace nfe
