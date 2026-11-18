"""Write Nasdaq TotalView-ITCH 5.0 messages with 2-byte length prefixes.

Layouts follow the ITCH 5.0 specification. Only what the simulator needs is
here; every message gets the common 11-byte header.
"""
import struct


def _hdr(t, loc, track, ts):
    return t.encode() + struct.pack(">HH", loc, track) + ts.to_bytes(6, "big")


def _sym(s):
    return s.encode().ljust(8)[:8]


class ItchWriter:
    def __init__(self, path):
        self.f = open(path, "wb")
        self.count = 0

    def _emit(self, body):
        self.f.write(struct.pack(">H", len(body)) + body)
        self.count += 1

    def close(self):
        self.f.close()

    def system_event(self, ts, code):
        self._emit(_hdr("S", 0, 0, ts) + code.encode())

    def stock_directory(self, ts, loc, sym, round_lot=100):
        body = (_hdr("R", loc, 0, ts) + _sym(sym) + b"Q" + b"N" + struct.pack(">I", round_lot)
                + b"N" + b"C" + b"Z " + b"P" + b"N" + b"N" + b"1" + b"N" + struct.pack(">I", 0) + b"N")
        self._emit(body)

    def trading_action(self, ts, loc, sym, state, reason="    "):
        self._emit(_hdr("H", loc, 0, ts) + _sym(sym) + state.encode() + b" " + reason.encode()[:4].ljust(4))

    def add(self, ts, loc, ref, side, shares, sym, price, mpid=None):
        if mpid is None:
            self._emit(_hdr("A", loc, 0, ts) + struct.pack(">Q", ref) + side.encode()
                       + struct.pack(">I", shares) + _sym(sym) + struct.pack(">I", price))
        else:
            self._emit(_hdr("F", loc, 0, ts) + struct.pack(">Q", ref) + side.encode()
                       + struct.pack(">I", shares) + _sym(sym) + struct.pack(">I", price) + mpid.encode()[:4].ljust(4))

    def executed(self, ts, loc, ref, shares, match):
        self._emit(_hdr("E", loc, 0, ts) + struct.pack(">QIQ", ref, shares, match))

    def executed_price(self, ts, loc, ref, shares, match, printable, price):
        self._emit(_hdr("C", loc, 0, ts) + struct.pack(">QIQ", ref, shares, match)
                   + (b"Y" if printable else b"N") + struct.pack(">I", price))

    def cancel(self, ts, loc, ref, shares):
        self._emit(_hdr("X", loc, 0, ts) + struct.pack(">QI", ref, shares))

    def delete(self, ts, loc, ref):
        self._emit(_hdr("D", loc, 0, ts) + struct.pack(">Q", ref))

    def replace(self, ts, loc, old, new, shares, price):
        self._emit(_hdr("U", loc, 0, ts) + struct.pack(">QQII", old, new, shares, price))

    def trade(self, ts, loc, side, shares, sym, price, match):
        self._emit(_hdr("P", loc, 0, ts) + struct.pack(">Q", 0) + side.encode()
                   + struct.pack(">I", shares) + _sym(sym) + struct.pack(">IQ", price, match))

    def cross(self, ts, loc, shares, sym, price, match, cross_type):
        self._emit(_hdr("Q", loc, 0, ts) + struct.pack(">Q", shares) + _sym(sym)
                   + struct.pack(">IQ", price, match) + cross_type.encode())

    def broken(self, ts, loc, match):
        self._emit(_hdr("B", loc, 0, ts) + struct.pack(">Q", match))
