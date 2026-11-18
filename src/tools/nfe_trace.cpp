// Print the full history of chosen orders, for diagnosing fidelity residuals.
//
//   nfe_trace <itch-file> REF [REF...]
//
// Follows each ref through Replace messages (the new ref is traced too) and
// also prints the symbol's trading-state (H) and cross (Q) messages and the
// system events, so each order's life can be read against the open.
// Times are printed as seconds after midnight.
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "nfe/io.hpp"
#include "nfe/itch.hpp"

using namespace nfe;

struct Tracer {
  std::set<uint64_t> refs;
  std::set<uint16_t> locs;

  static double t(const itch::Header& h) { return h.ts / 1e9; }

  void on(const itch::SystemEvent& m) { std::printf("%.9f S event=%c\n", t(m.h), m.event); }
  void on(const itch::StockTradingAction& m) {
    if (locs.count(m.h.locate)) std::printf("%.9f H loc=%u state=%c\n", t(m.h), m.h.locate, m.state);
  }
  void on(const itch::CrossTrade& m) {
    if (locs.count(m.h.locate))
      std::printf("%.9f Q loc=%u type=%c shares=%llu px=%u\n", t(m.h), m.h.locate, m.cross_type,
                  (unsigned long long)m.shares, m.price);
  }
  void on(const itch::AddOrder& m) {
    if (refs.count(m.ref)) {
      locs.insert(m.h.locate);
      std::printf("%.9f A ref=%llu loc=%u side=%c shares=%u px=%u\n", t(m.h), (unsigned long long)m.ref,
                  m.h.locate, m.side, m.shares, m.price);
    }
  }
  void on(const itch::OrderExecuted& m) {
    if (refs.count(m.ref))
      std::printf("%.9f E ref=%llu shares=%u match=%llu\n", t(m.h), (unsigned long long)m.ref, m.shares,
                  (unsigned long long)m.match);
  }
  void on(const itch::OrderExecutedWithPrice& m) {
    if (refs.count(m.ref))
      std::printf("%.9f C ref=%llu shares=%u px=%u printable=%c\n", t(m.h), (unsigned long long)m.ref,
                  m.shares, m.price, m.printable);
  }
  void on(const itch::OrderCancel& m) {
    if (refs.count(m.ref)) std::printf("%.9f X ref=%llu shares=%u\n", t(m.h), (unsigned long long)m.ref, m.shares);
  }
  void on(const itch::OrderDelete& m) {
    if (refs.count(m.ref)) std::printf("%.9f D ref=%llu\n", t(m.h), (unsigned long long)m.ref);
  }
  void on(const itch::OrderReplace& m) {
    if (refs.count(m.old_ref)) {
      refs.insert(m.new_ref);
      std::printf("%.9f U old=%llu new=%llu shares=%u px=%u\n", t(m.h), (unsigned long long)m.old_ref,
                  (unsigned long long)m.new_ref, m.shares, m.price);
    }
  }
  template <class M> void on(const M&) {}
};

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: nfe_trace <itch-file> REF [REF...]\n"); return 2; }
  Tracer tr;
  for (int i = 2; i < argc; ++i) tr.refs.insert(std::strtoull(argv[i], nullptr, 10));
  MappedFile f(argv[1]);
  itch::parse_stream(f.data(), f.size(), tr);
  return 0;
}
