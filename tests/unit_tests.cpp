// Unit tests: ITCH decoding, book operations, priority rules, matching.
#include <cstdio>
#include <cstring>
#include <vector>

#include "nfe/book.hpp"
#include "nfe/itch.hpp"
#include "nfe/matcher.hpp"

using namespace nfe;

static int failures = 0, checks = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    ++checks;                                                                 \
    if (!(x)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } \
  } while (0)

// ---- ITCH encoding helpers ----
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 255); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((x >> (8 * i)) & 255); }
static void put48(std::vector<uint8_t>& v, uint64_t x) { for (int i = 5; i >= 0; --i) v.push_back((x >> (8 * i)) & 255); }
static void put64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 7; i >= 0; --i) v.push_back((x >> (8 * i)) & 255); }
static void hdr(std::vector<uint8_t>& v, char t, uint16_t loc, uint64_t ts) { v.push_back(t); put16(v, loc); put16(v, 0); put48(v, ts); }
static void sym(std::vector<uint8_t>& v, const char* s) { char b[8]; std::memset(b, ' ', 8); std::memcpy(b, s, std::strlen(s)); v.insert(v.end(), b, b + 8); }
static void framed(std::vector<uint8_t>& out, const std::vector<uint8_t>& m) { put16(out, uint16_t(m.size())); out.insert(out.end(), m.begin(), m.end()); }

struct Capture {
  std::vector<itch::AddOrder> adds;
  std::vector<itch::OrderExecuted> execs;
  std::vector<itch::OrderReplace> reps;
  std::vector<itch::CrossTrade> crosses;
  void on(const itch::AddOrder& m) { adds.push_back(m); }
  void on(const itch::OrderExecuted& m) { execs.push_back(m); }
  void on(const itch::OrderReplace& m) { reps.push_back(m); }
  void on(const itch::CrossTrade& m) { crosses.push_back(m); }
  template <class M> void on(const M&) {}
};

static void test_itch_decode() {
  std::vector<uint8_t> buf, m;
  hdr(m, 'A', 7, 34200000000123ULL); put64(m, 42); m.push_back('B'); put32(m, 300); sym(m, "AAPL"); put32(m, 1234500);
  CHECK(m.size() == 36);
  framed(buf, m); m.clear();
  hdr(m, 'E', 7, 34200000000124ULL); put64(m, 42); put32(m, 100); put64(m, 9);
  CHECK(m.size() == 31);
  framed(buf, m); m.clear();
  hdr(m, 'U', 7, 34200000000125ULL); put64(m, 42); put64(m, 43); put32(m, 200); put32(m, 1234600);
  CHECK(m.size() == 35);
  framed(buf, m); m.clear();
  hdr(m, 'Q', 7, 57600000000000ULL); put64(m, 5000); sym(m, "AAPL"); put32(m, 1234700); put64(m, 10); m.push_back('C');
  CHECK(m.size() == 40);
  framed(buf, m);

  Capture c;
  auto st = itch::parse_stream(buf.data(), buf.size(), c);
  CHECK(st.messages == 4 && st.malformed == 0 && st.truncated_tail_bytes == 0);
  CHECK(c.adds.size() == 1 && c.adds[0].ref == 42 && c.adds[0].side == 'B' && c.adds[0].shares == 300);
  CHECK(c.adds[0].price == 1234500 && c.adds[0].stock.view() == "AAPL" && c.adds[0].h.locate == 7);
  CHECK(c.adds[0].h.ts == 34200000000123ULL);
  CHECK(c.execs.size() == 1 && c.execs[0].shares == 100 && c.execs[0].match == 9);
  CHECK(c.reps.size() == 1 && c.reps[0].old_ref == 42 && c.reps[0].new_ref == 43 && c.reps[0].price == 1234600);
  CHECK(c.crosses.size() == 1 && c.crosses[0].cross_type == 'C' && c.crosses[0].shares == 5000);

  // A truncated final message is reported, not read.
  Capture c2;
  auto st2 = itch::parse_stream(buf.data(), buf.size() - 5, c2);
  CHECK(st2.messages == 3 && st2.truncated_tail_bytes > 0);
  // A declared length shorter than the type's size is malformed.
  std::vector<uint8_t> bad;
  std::vector<uint8_t> shortm(buf.begin() + 2, buf.begin() + 2 + 20);
  framed(bad, shortm);
  auto st3 = itch::parse_stream(bad.data(), bad.size(), c2);
  CHECK(st3.malformed == 1);
}

static std::vector<uint64_t> ids(const std::vector<Fill>& f) {
  std::vector<uint64_t> v;
  for (auto& x : f) v.push_back(x.resting_id);
  return v;
}

// Orders accepted before the open but displayed after the cross keep their
// entry-order priority: with priority_by_ref the lower reference number goes
// ahead of a resting order with a higher one. Default stays feed order.
static void test_priority_by_ref() {
  for (bool by_ref : {false, true}) {
    BaselineBook b;
    b.priority_by_ref = by_ref;
    CHECK(b.add(1, 50, 'B', 100, 10, 1) == Result::Ok);   // displayed first, entered later
    CHECK(b.add(1, 20, 'B', 100, 10, 2) == Result::Ok);   // released late, entered earlier
    CHECK(b.add(1, 30, 'B', 100, 10, 3) == Result::Ok);
    CHECK(b.add(1, 60, 'B', 100, 10, 4) == Result::Ok);   // normal arrival
    std::vector<Fill> f;
    predict(b, 1, 'S', 100, 40, f);
    if (by_ref) {
      CHECK((ids(f) == std::vector<uint64_t>{20, 30, 50, 60}));
      CHECK(b.find(20)->late && b.find(30)->late && !b.find(50)->late && !b.find(60)->late);
      CHECK(b.late_inserts == 2);
    } else {
      CHECK((ids(f) == std::vector<uint64_t>{50, 20, 30, 60}));
      CHECK(b.late_inserts == 0);
    }
    CHECK(b.level_qty(1, 'B', 100) == 40);
    // Removing the head and a middle order keeps the level consistent.
    CHECK(b.remove(by_ref ? 20 : 50) == Result::Ok);
    CHECK(b.reduce(30, 10) == Result::Ok);
    CHECK(b.level_qty(1, 'B', 100) == 20);
  }
}

static void test_book_and_priority() {
  BaselineBook b;
  CHECK(b.add(1, 1, 'S', 100, 100, 1) == Result::Ok);
  CHECK(b.add(1, 2, 'S', 100, 100, 2) == Result::Ok);
  CHECK(b.add(1, 3, 'S', 101, 100, 3) == Result::Ok);
  CHECK(b.add(1, 1, 'S', 100, 100, 4) == Result::DuplicateRef);
  CHECK(b.level_qty(1, 'S', 100) == 200);
  CHECK(b.best_ask(1)->price == 100 && !b.best_bid(1));

  // Partial cancel keeps priority.
  CHECK(b.reduce(1, 50) == Result::Ok);
  std::vector<Fill> f;
  predict(b, 1, 'B', 100, 60, f);
  CHECK((ids(f) == std::vector<uint64_t>{1, 2}));
  CHECK(f[0].qty == 50 && f[1].qty == 10);

  // Replace loses priority even at the same price and size.
  CHECK(b.replace(1, 4, 100, 50, 5) == Result::Ok);
  f.clear();
  predict(b, 1, 'B', 100, 60, f);
  CHECK((ids(f) == std::vector<uint64_t>{2}));

  // Price beats time; the limit stops the sweep.
  f.clear();
  predict(b, 1, 'B', 101, 1000, f);
  CHECK((ids(f) == std::vector<uint64_t>{2, 4, 3}));
  f.clear();
  predict(b, 1, 'B', 99, 1000, f);
  CHECK(f.empty());

  // Overfill and unknown refs are reported.
  CHECK(b.reduce(2, 1000) == Result::Overfill);
  CHECK(b.reduce(99, 1) == Result::UnknownRef);
  CHECK(b.remove(99) == Result::UnknownRef);

  // Submit matches then rests the remainder.
  f.clear();
  uint32_t rested = submit(b, 1, 10, 'B', 100, 200, 6, f);
  CHECK(rested == 50);
  CHECK(b.best_bid(1)->price == 100 && b.best_bid(1)->qty == 50);
  CHECK(b.best_ask(1)->price == 101);
  CHECK(!b.find(2) && !b.find(4));
}

int main() {
  test_itch_decode();
  test_book_and_priority();
  test_priority_by_ref();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
