// Nasdaq TotalView-ITCH 5.0 decoder.
//
// The public sample files are a stream of messages, each prefixed by a
// 2-byte big-endian length. Every message starts with the same 11-byte
// header: type (1), stock locate (2), tracking number (2), timestamp (6,
// nanoseconds since midnight). All integers are big-endian.
//
// Decoding is bounds-checked: a message whose declared length is shorter
// than the fixed size of its type is reported as malformed instead of read.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace nfe::itch {

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t be48(const uint8_t* p) {
  return uint64_t(be16(p)) << 32 | be32(p + 2);
}
inline uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

struct Header {
  char type;
  uint16_t locate;
  uint16_t tracking;
  uint64_t ts;  // ns since midnight
};

struct Symbol {
  char s[8];
  std::string_view view() const {
    size_t n = 8;
    while (n > 0 && s[n - 1] == ' ') --n;
    return {s, n};
  }
};

struct SystemEvent { Header h; char event; };                        // S
struct StockDirectory {                                              // R
  Header h; Symbol stock; char market_category; char fsi; uint32_t round_lot;
  char round_lots_only; char issue_class; char issue_subtype[2]; char authenticity;
  char short_sale_threshold; char ipo_flag; char luld_tier; char etp_flag;
  uint32_t etp_leverage; char inverse;
};
struct StockTradingAction { Header h; Symbol stock; char state; char reserved; char reason[4]; };  // H
struct RegSho { Header h; Symbol stock; char action; };              // Y
struct MarketParticipantPosition {                                   // L
  Header h; char mpid[4]; Symbol stock; char primary; char mode; char state;
};
struct MwcbDecline { Header h; uint64_t l1, l2, l3; };               // V
struct MwcbStatus { Header h; char level; };                          // W
struct IpoQuotingPeriod {                                            // K
  Header h; Symbol stock; uint32_t release_time; char qualifier; uint32_t price;
};
struct LuldCollar {                                                  // J
  Header h; Symbol stock; uint32_t ref_price, upper, lower, extension;
};
struct OperationalHalt { Header h; Symbol stock; char market; char action; };  // h
struct AddOrder {                                                    // A and F
  Header h; uint64_t ref; char side; uint32_t shares; Symbol stock; uint32_t price;
  char mpid[4]; bool has_mpid;
};
struct OrderExecuted { Header h; uint64_t ref; uint32_t shares; uint64_t match; };  // E
struct OrderExecutedWithPrice {                                      // C
  Header h; uint64_t ref; uint32_t shares; uint64_t match; char printable; uint32_t price;
};
struct OrderCancel { Header h; uint64_t ref; uint32_t shares; };     // X
struct OrderDelete { Header h; uint64_t ref; };                      // D
struct OrderReplace { Header h; uint64_t old_ref, new_ref; uint32_t shares, price; };  // U
struct Trade {                                                       // P
  Header h; uint64_t ref; char side; uint32_t shares; Symbol stock; uint32_t price; uint64_t match;
};
struct CrossTrade {                                                  // Q
  Header h; uint64_t shares; Symbol stock; uint32_t price; uint64_t match; char cross_type;
};
struct BrokenTrade { Header h; uint64_t match; };                    // B
struct Noii {                                                        // I
  Header h; uint64_t paired, imbalance; char direction; Symbol stock;
  uint32_t far_price, near_price, ref_price; char cross_type; char variation;
};
struct Rpii { Header h; Symbol stock; char flag; };                  // N
struct Dlcr {                                                        // O
  Header h; Symbol stock; char eligible; uint32_t min_px, max_px, near_px;
  uint64_t near_time; uint32_t lower_collar, upper_collar;
};

// Fixed message sizes (including the type byte), from the ITCH 5.0 spec.
constexpr size_t message_size(char t) {
  switch (t) {
    case 'S': return 12;  case 'R': return 39;  case 'H': return 25;  case 'Y': return 20;
    case 'L': return 26;  case 'V': return 35;  case 'W': return 12;  case 'K': return 28;
    case 'J': return 35;  case 'h': return 21;  case 'A': return 36;  case 'F': return 40;
    case 'E': return 31;  case 'C': return 36;  case 'X': return 23;  case 'D': return 19;
    case 'U': return 35;  case 'P': return 44;  case 'Q': return 40;  case 'B': return 19;
    case 'I': return 50;  case 'N': return 20;  case 'O': return 48;
    default: return 0;
  }
}

inline Header header(const uint8_t* p) {
  return Header{char(p[0]), be16(p + 1), be16(p + 3), be48(p + 5)};
}
inline Symbol symbol(const uint8_t* p) { Symbol s; std::memcpy(s.s, p, 8); return s; }

enum class Status { Ok, UnknownType, Malformed };

// Decode one message body (without its length prefix) and call the handler.
// The handler must provide `on(const T&)` for every message struct; a
// catch-all template overload is the usual way to ignore the rest.
template <class Handler>
Status decode(const uint8_t* p, size_t len, Handler& hd) {
  if (len == 0) return Status::Malformed;
  const char t = char(p[0]);
  const size_t need = message_size(t);
  if (need == 0) return Status::UnknownType;
  if (len < need) return Status::Malformed;
  const Header h = header(p);
  const uint8_t* b = p + 11;
  switch (t) {
    case 'S': hd.on(SystemEvent{h, char(b[0])}); break;
    case 'R': {
      StockDirectory m{h, symbol(b), char(b[8]), char(b[9]), be32(b + 10), char(b[14]),
                       char(b[15]), {char(b[16]), char(b[17])}, char(b[18]), char(b[19]),
                       char(b[20]), char(b[21]), char(b[22]), be32(b + 23), char(b[27])};
      hd.on(m); break;
    }
    case 'H': {
      StockTradingAction m{h, symbol(b), char(b[8]), char(b[9]), {}};
      std::memcpy(m.reason, b + 10, 4); hd.on(m); break;
    }
    case 'Y': hd.on(RegSho{h, symbol(b), char(b[8])}); break;
    case 'L': {
      MarketParticipantPosition m{h, {}, symbol(b + 4), char(b[12]), char(b[13]), char(b[14])};
      std::memcpy(m.mpid, b, 4); hd.on(m); break;
    }
    case 'V': hd.on(MwcbDecline{h, be64(b), be64(b + 8), be64(b + 16)}); break;
    case 'W': hd.on(MwcbStatus{h, char(b[0])}); break;
    case 'K': hd.on(IpoQuotingPeriod{h, symbol(b), be32(b + 8), char(b[12]), be32(b + 13)}); break;
    case 'J': hd.on(LuldCollar{h, symbol(b), be32(b + 8), be32(b + 12), be32(b + 16), be32(b + 20)}); break;
    case 'h': hd.on(OperationalHalt{h, symbol(b), char(b[8]), char(b[9])}); break;
    case 'A':
    case 'F': {
      AddOrder m{h, be64(b), char(b[8]), be32(b + 9), symbol(b + 13), be32(b + 21), {}, t == 'F'};
      if (t == 'F') std::memcpy(m.mpid, b + 25, 4);
      hd.on(m); break;
    }
    case 'E': hd.on(OrderExecuted{h, be64(b), be32(b + 8), be64(b + 12)}); break;
    case 'C': hd.on(OrderExecutedWithPrice{h, be64(b), be32(b + 8), be64(b + 12), char(b[20]), be32(b + 21)}); break;
    case 'X': hd.on(OrderCancel{h, be64(b), be32(b + 8)}); break;
    case 'D': hd.on(OrderDelete{h, be64(b)}); break;
    case 'U': hd.on(OrderReplace{h, be64(b), be64(b + 8), be32(b + 16), be32(b + 20)}); break;
    case 'P': hd.on(Trade{h, be64(b), char(b[8]), be32(b + 9), symbol(b + 13), be32(b + 21), be64(b + 25)}); break;
    case 'Q': hd.on(CrossTrade{h, be64(b), symbol(b + 8), be32(b + 16), be64(b + 20), char(b[28])}); break;
    case 'B': hd.on(BrokenTrade{h, be64(b)}); break;
    case 'I': hd.on(Noii{h, be64(b), be64(b + 8), char(b[16]), symbol(b + 17), be32(b + 25),
                         be32(b + 29), be32(b + 33), char(b[37]), char(b[38])}); break;
    case 'N': hd.on(Rpii{h, symbol(b), char(b[8])}); break;
    case 'O': hd.on(Dlcr{h, symbol(b), char(b[8]), be32(b + 9), be32(b + 13), be32(b + 17),
                         be64(b + 21), be32(b + 29), be32(b + 33)}); break;
  }
  return Status::Ok;
}

struct StreamStats {
  uint64_t messages = 0, unknown = 0, malformed = 0, truncated_tail_bytes = 0;
};

// Walk a buffer of length-prefixed messages. Never reads past `n`.
template <class Handler>
StreamStats parse_stream(const uint8_t* p, size_t n, Handler& hd) {
  StreamStats st;
  size_t off = 0;
  while (off + 2 <= n) {
    const size_t len = be16(p + off);
    if (off + 2 + len > n) { st.truncated_tail_bytes = n - off; return st; }
    switch (decode(p + off + 2, len, hd)) {
      case Status::Ok: ++st.messages; break;
      case Status::UnknownType: ++st.unknown; break;
      case Status::Malformed: ++st.malformed; break;
    }
    off += 2 + len;
  }
  st.truncated_tail_bytes = n - off;
  return st;
}

}  // namespace nfe::itch
