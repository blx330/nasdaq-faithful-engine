// Phase 1: reconstruct the displayed book from ITCH and check it.
//
// `Replayer` is an ITCH handler. It applies every order message to the book,
// tracks system and per-symbol trading state, and counts anything that would
// mean the book is wrong: unknown references, duplicate references, fills
// larger than the order, and a crossed book during continuous trading.
//
// An observer gets hooks before and after each book change. The queue study
// uses them; the fidelity pipeline instead buffers messages and feeds them
// to the replayer itself.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "book.hpp"
#include "itch.hpp"

namespace nfe {

constexpr uint64_t kNsPerSec = 1'000'000'000ULL;
constexpr uint64_t kOpenGraceNs = 5 * kNsPerSec;  // continuous if no cross printed by then

struct NullObserver {
  template <class R> void pre_add(R&, const itch::AddOrder&) {}
  template <class R> void post_add(R&, Order&) {}
  template <class R> void pre_exec(R&, Order&, uint32_t, uint32_t, uint64_t, bool) {}
  template <class R> void pre_cancel(R&, Order&, uint32_t, uint64_t) {}
  template <class R> void pre_delete(R&, Order&, uint64_t) {}
  template <class R> void pre_replace(R&, Order&, const itch::OrderReplace&) {}
  template <class R> void post_replace(R&, Order&) {}
  template <class R> void on_trade(R&, const itch::Trade&) {}
  template <class R> void on_cross(R&, const itch::CrossTrade&) {}
  template <class R> void touched(R&, uint16_t, uint64_t) {}
};

struct ReplayCounters {
  std::array<uint64_t, 128> by_type{};
  uint64_t unknown_ref = 0, duplicate_ref = 0, overfill = 0;
  uint64_t crossed_continuous = 0;   // must be zero
  uint64_t crossed_other = 0;        // allowed: halts, IPO quoting, pre-cross
  uint64_t broken_trades = 0;
  uint64_t c_printable = 0, c_nonprintable = 0, p_trades = 0;
  uint64_t n_msgs = 0, first_anomaly_msg = 0;  // message number of the first anomaly
};

struct CrossPrint { char type; uint32_t price; uint64_t shares; uint64_t ts; };

template <class Book = BaselineBook, class Obs = NullObserver>
class Replayer {
 public:
  explicit Replayer(Obs* obs = nullptr) : obs_(obs ? obs : &own_) {}

  Book book;
  ReplayCounters c;
  std::vector<std::string> names = std::vector<std::string>(65536);
  std::vector<char> state = std::vector<char>(65536, ' ');      // H message state
  std::vector<uint64_t> state_ts = std::vector<uint64_t>(65536, 0);
  std::vector<uint8_t> cross_since_t = std::vector<uint8_t>(65536, 0);
  std::map<uint16_t, std::vector<CrossPrint>> crosses;
  char system = ' ';             // last S event code
  uint64_t market_open_ts = 0, market_close_ts = 0;
  uint64_t last_ts = 0;

  bool market_hours() const { return system == 'Q'; }

  // Continuous trading for a symbol: market hours, state T, and either a
  // cross has printed since the symbol entered T or the grace period passed.
  bool continuous(uint16_t loc, uint64_t ts) const {
    if (!market_hours() || state[loc] != 'T') return false;
    const uint64_t since = std::max(state_ts[loc], market_open_ts);
    return cross_since_t[loc] || ts >= since + kOpenGraceNs;
  }

  // ---- ITCH handlers ----
  void on(const itch::SystemEvent& m) {
    count(m.h);
    system = m.event == 'M' || m.event == 'E' || m.event == 'C' ? 'M' : (m.event == 'Q' ? 'Q' : m.event);
    if (m.event == 'Q') market_open_ts = m.h.ts;
    if (m.event == 'M') market_close_ts = m.h.ts;
  }
  void on(const itch::StockDirectory& m) { count(m.h); names[m.h.locate] = std::string(m.stock.view()); }
  void on(const itch::StockTradingAction& m) {
    count(m.h);
    if (state[m.h.locate] != m.state) {
      state[m.h.locate] = m.state;
      state_ts[m.h.locate] = m.h.ts;
      cross_since_t[m.h.locate] = 0;
    }
  }
  void on(const itch::AddOrder& m) {
    count(m.h);
    obs_->pre_add(*this, m);
    if (book.add(m.h.locate, m.ref, m.side, m.price, m.shares, m.h.ts) != Result::Ok) { flag(c.duplicate_ref); return; }
    obs_->post_add(*this, *book.find(m.ref));
    check_crossed(m.h.locate, m.h.ts);
    obs_->touched(*this, m.h.locate, m.h.ts);
  }
  void on(const itch::OrderExecuted& m) {
    count(m.h);
    Order* o = book.find(m.ref);
    if (!o) { flag(c.unknown_ref); return; }
    obs_->pre_exec(*this, *o, m.shares, o->price, m.h.ts, true);
    tally(book.reduce(m.ref, m.shares));
    obs_->touched(*this, m.h.locate, m.h.ts);
  }
  void on(const itch::OrderExecutedWithPrice& m) {
    count(m.h);
    (m.printable == 'Y' ? c.c_printable : c.c_nonprintable)++;
    Order* o = book.find(m.ref);
    if (!o) { flag(c.unknown_ref); return; }
    obs_->pre_exec(*this, *o, m.shares, m.price, m.h.ts, m.printable == 'Y');
    tally(book.reduce(m.ref, m.shares));
    obs_->touched(*this, m.h.locate, m.h.ts);
  }
  void on(const itch::OrderCancel& m) {
    count(m.h);
    Order* o = book.find(m.ref);
    if (!o) { flag(c.unknown_ref); return; }
    obs_->pre_cancel(*this, *o, m.shares, m.h.ts);
    tally(book.reduce(m.ref, m.shares));
    obs_->touched(*this, m.h.locate, m.h.ts);
  }
  void on(const itch::OrderDelete& m) {
    count(m.h);
    Order* o = book.find(m.ref);
    if (!o) { flag(c.unknown_ref); return; }
    obs_->pre_delete(*this, *o, m.h.ts);
    book.remove(m.ref);
    obs_->touched(*this, m.h.locate, m.h.ts);
  }
  void on(const itch::OrderReplace& m) {
    count(m.h);
    Order* o = book.find(m.old_ref);
    if (!o) { flag(c.unknown_ref); return; }
    obs_->pre_replace(*this, *o, m);
    const Result r = book.replace(m.old_ref, m.new_ref, m.price, m.shares, m.h.ts);
    if (r != Result::Ok) { tally(r); return; }
    obs_->post_replace(*this, *book.find(m.new_ref));
    check_crossed(m.h.locate, m.h.ts);
    obs_->touched(*this, m.h.locate, m.h.ts);
  }
  void on(const itch::Trade& m) { count(m.h); ++c.p_trades; obs_->on_trade(*this, m); }
  void on(const itch::CrossTrade& m) {
    count(m.h);
    crosses[m.h.locate].push_back(CrossPrint{m.cross_type, m.price, m.shares, m.h.ts});
    cross_since_t[m.h.locate] = 1;
    obs_->on_cross(*this, m);
  }
  void on(const itch::BrokenTrade& m) { count(m.h); ++c.broken_trades; }
  template <class M> void on(const M& m) { count(m.h); }

 private:
  void count(const itch::Header& h) { ++c.by_type[uint8_t(h.type) & 127]; ++c.n_msgs; last_ts = h.ts; }
  void flag(uint64_t& ctr) { ++ctr; if (!c.first_anomaly_msg) c.first_anomaly_msg = c.n_msgs; }
  void tally(Result r) {
    if (r == Result::UnknownRef) flag(c.unknown_ref);
    else if (r == Result::DuplicateRef) flag(c.duplicate_ref);
    else if (r == Result::Overfill) flag(c.overfill);
  }
  void check_crossed(uint16_t loc, uint64_t ts) {
    auto b = book.best_bid(loc);
    auto a = book.best_ask(loc);
    if (b && a && b->price >= a->price) {
      if (continuous(loc, ts)) flag(c.crossed_continuous); else ++c.crossed_other;
    }
  }

  Obs own_{};
  Obs* obs_;
};

}  // namespace nfe
