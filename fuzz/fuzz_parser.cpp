// libFuzzer target: the ITCH stream parser on arbitrary bytes, including
// truncated and malformed messages. ASan turns any out-of-bounds read into
// a crash, so "no crash" means the parser stays inside the buffer.
#include <cstddef>
#include <cstdint>

#include "nfe/itch.hpp"
#include "nfe/replay.hpp"

struct Sink {
  uint64_t acc = 0;
  template <class M> void on(const M& m) { acc += m.h.ts + m.h.locate + uint8_t(m.h.type); }
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Sink s;
  auto st = nfe::itch::parse_stream(data, size, s);
  if (st.truncated_tail_bytes > size) __builtin_trap();
  // Also drive the replayer: arbitrary order messages must never corrupt it.
  static nfe::Replayer<>* r = new nfe::Replayer<>();
  nfe::itch::parse_stream(data, size, *r);
  return 0;
}
