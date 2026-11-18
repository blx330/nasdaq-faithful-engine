"""Synthetic Nasdaq-like exchange that writes an ITCH 5.0 file with ground truth.

Why this exists: the real sample files only show resting orders and the
executions against them. Here every incoming order is known, so the fidelity
pipeline's aggressor inference and grouping can be scored against the truth.

What it simulates, per symbol:
  * Pre-open (04:00-09:30): orders rest without matching, so the book may cross.
  * Opening cross at 09:30 (non-printable C messages + Cross Trade 'O').
  * Continuous trading: passive adds (some with MPID), hidden orders, partial
    and full cancels, replaces (priority lost), and aggressive limit orders
    that sweep the book in price-time order, displayed ahead of hidden at a
    price. Some resting orders execute at a price different from their
    displayed price (printable C), and some aggressors share a timestamp.
  * One symbol is halted at 12:00 and reopens with a halt cross at 12:05.
  * Closing cross at 16:00 (non-printable C + Cross Trade 'C').

Outputs: the ITCH file, truth.jsonl (one line per continuous-trading
aggressor) and closes.csv (the "official" closing prices).
"""
import argparse
import bisect
import json
import math
import random
from collections import deque

from itch_writer import ItchWriter

NS = 10**9
TICK = 100  # $0.01 in ITCH 4-decimal price units


def hms(h, m, s=0.0):
    return int(((h * 60 + m) * 60 + s) * NS)


class Order:
    __slots__ = ("ref", "side", "price", "qty", "hidden", "reprice")

    def __init__(self, ref, side, price, qty, hidden=False, reprice=False):
        self.ref, self.side, self.price, self.qty = ref, side, price, qty
        self.hidden, self.reprice = hidden, reprice


class Book:
    def __init__(self):
        self.disp = {"B": {}, "S": {}}
        self.hid = {"B": {}, "S": {}}
        self.prices = {"B": [], "S": []}  # ascending, any liquidity
        self.orders = {}

    def _touch_price(self, side, p):
        lst = self.prices[side]
        i = bisect.bisect_left(lst, p)
        if i == len(lst) or lst[i] != p:
            lst.insert(i, p)

    def drop_if_empty(self, side, p):
        if not self.disp[side].get(p) and not self.hid[side].get(p):
            self.disp[side].pop(p, None)
            self.hid[side].pop(p, None)
            lst = self.prices[side]
            i = bisect.bisect_left(lst, p)
            if i < len(lst) and lst[i] == p:
                lst.pop(i)

    def rest(self, o):
        d = (self.hid if o.hidden else self.disp)[o.side]
        d.setdefault(o.price, deque()).append(o)
        self._touch_price(o.side, o.price)
        self.orders[o.ref] = o

    def remove(self, o):
        d = (self.hid if o.hidden else self.disp)[o.side]
        d[o.price].remove(o)
        del self.orders[o.ref]
        self.drop_if_empty(o.side, o.price)

    def best_any(self, side):
        lst = self.prices[side]
        if not lst:
            return None
        return lst[-1] if side == "B" else lst[0]

    def best_disp(self, side):
        lst = self.prices[side]
        for p in (reversed(lst) if side == "B" else lst):
            if self.disp[side].get(p):
                return p
        return None

    def levels(self, side):
        lst = self.prices[side]
        return list(reversed(lst)) if side == "B" else list(lst)


class Sym:
    def __init__(self, loc, name, price, half_spread_ticks, lot_mean, weight):
        self.loc, self.name = loc, name
        self.fair = float(price)
        self.half = half_spread_ticks
        self.lot_mean = lot_mean
        self.weight = weight
        self.book = Book()
        self.refs = []  # candidate refs for cancel/replace (lazily pruned)
        self.state = "T"


class Exchange:
    def __init__(self, path, seed):
        self.w = ItchWriter(path)
        self.rng = random.Random(seed)
        self.ref = 1000
        self.match = 0
        self.ts = 0
        self.truth = []
        self.closes = {}

    # ---- helpers ----
    def next_ref(self):
        self.ref += self.rng.randint(1, 4)  # rising but not dense, like real refs
        return self.ref

    def next_match(self):
        self.match += 1
        return self.match

    def at(self, t):
        self.ts = max(self.ts + 1, t)
        return self.ts

    def lots(self, s):
        n = max(1, int(self.rng.expovariate(1.0 / s.lot_mean)))
        return n * 100 if self.rng.random() < 0.9 else n * 100 + self.rng.randint(1, 99)

    def pick_ref(self, s, displayed_only=False):
        for _ in range(8):
            if not s.refs:
                return None
            i = self.rng.randrange(len(s.refs))
            o = s.book.orders.get(s.refs[i])
            if o is None:
                s.refs[i] = s.refs[-1]
                s.refs.pop()
                continue
            if displayed_only and o.hidden:
                continue
            return o
        return None

    # ---- order entry ----
    def add_resting(self, s, side, price, qty, ts, hidden=False, mpid=None):
        o = Order(self.next_ref(), side, price, qty, hidden,
                  reprice=(not hidden and self.rng.random() < 0.01))
        s.book.rest(o)
        s.refs.append(o.ref)
        if not hidden:
            self.w.add(ts, s.loc, o.ref, side, qty, s.name, price, mpid)
        return o

    def aggress(self, s, side, limit, qty, ts, post, group):
        b = s.book
        opp = "S" if side == "B" else "B"
        fills, left = [], qty
        while left > 0:
            p = b.best_any(opp)
            if p is None or (side == "B" and p > limit) or (side == "S" and p < limit):
                break
            for queue in (b.disp[opp].get(p), b.hid[opp].get(p)):
                while queue and left > 0:
                    o = queue[0]
                    q = min(o.qty, left)
                    m = self.next_match()
                    if o.hidden:
                        self.w.trade(ts, s.loc, opp, q, s.name, p, m)
                        kind, px = "hidden", p
                    elif o.reprice:
                        px = p - TICK if opp == "S" else p + TICK
                        self.w.executed_price(ts, s.loc, o.ref, q, m, True, px)
                        kind = "nondisplay_price"
                    else:
                        self.w.executed(ts, s.loc, o.ref, q, m)
                        kind, px = "displayed", p
                    fills.append([o.ref if not o.hidden else 0, q, px, kind])
                    o.qty -= q
                    left -= q
                    if o.qty == 0:
                        queue.popleft()
                        del b.orders[o.ref]
            b.drop_if_empty(opp, p)
        posted = 0
        if post and left > 0:
            o = Order(self.next_ref(), side, limit, left)
            b.rest(o)
            s.refs.append(o.ref)
            self.w.add(ts, s.loc, o.ref, side, left, s.name, limit)
            posted = left
        if fills:
            self.truth.append({"loc": s.loc, "ts": ts, "side": side, "qty": qty, "limit": limit,
                               "posted": posted, "group": group, "fills": fills})
        return fills

    # ---- auctions ----
    def run_cross(self, s, ts, cross_type, extra_sell=0):
        b = s.book
        prices = sorted(set(b.prices["B"]) | set(b.prices["S"]))
        best_p, best_v = None, -1
        for p in prices:
            bv = sum(o.qty for lp in b.prices["B"] if lp >= p for o in list(b.disp["B"].get(lp, ())) + list(b.hid["B"].get(lp, ())))
            sv = sum(o.qty for lp in b.prices["S"] if lp <= p for o in list(b.disp["S"].get(lp, ())) + list(b.hid["S"].get(lp, ())))
            sv += extra_sell
            v = min(bv, sv)
            if v > best_v or (v == best_v and best_p is not None and abs(p - s.fair) < abs(best_p - s.fair)):
                best_p, best_v = p, v
        if best_p is None or best_v <= 0:
            px = int(round(s.fair / TICK)) * TICK
            self.w.cross(ts, s.loc, 0, s.name, px, 0, cross_type)
            return px
        for side in ("B", "S"):
            left = best_v - (extra_sell if side == "S" else 0)
            for lp in b.levels(side):
                if left <= 0 or (side == "B" and lp < best_p) or (side == "S" and lp > best_p):
                    break
                for queue in (b.disp[side].get(lp), b.hid[side].get(lp)):
                    while queue and left > 0:
                        o = queue[0]
                        q = min(o.qty, left)
                        if not o.hidden:
                            self.w.executed_price(ts, s.loc, o.ref, q, self.next_match(), False, best_p)
                        o.qty -= q
                        left -= q
                        if o.qty == 0:
                            queue.popleft()
                            del b.orders[o.ref]
                b.drop_if_empty(side, lp)
        self.w.cross(ts, s.loc, best_v, s.name, best_p, self.next_match(), cross_type)
        return best_p

    # ---- event generators ----
    def passive(self, s, ts, matching):
        b = s.book
        side = "B" if self.rng.random() < 0.5 else "S"
        k = int(self.rng.expovariate(0.9))
        ft = s.fair / TICK
        if side == "B":
            px = (math.floor(ft - s.half) - k) * TICK
            ob = b.best_any("S")
            if matching and ob is not None and px >= ob:
                px = ob - TICK
        else:
            px = (math.ceil(ft + s.half) + k) * TICK
            ob = b.best_any("B")
            if matching and ob is not None and px <= ob:
                px = ob + TICK
        if px <= 0:
            return
        hidden = self.rng.random() < 0.06
        mpid = "MMKR" if self.rng.random() < 0.03 else None
        self.add_resting(s, side, px, self.lots(s), ts, hidden, mpid)

    def cancel(self, s, ts):
        o = self.pick_ref(s)
        if o is None:
            return
        if self.rng.random() < 0.5:  # makers pull the order most at risk from fair value
            for _ in range(3):
                c = self.pick_ref(s)
                if c is not None and self._risk(s, c) > self._risk(s, o):
                    o = c
        if not o.hidden and o.qty > 100 and self.rng.random() < 0.3:
            q = self.rng.randint(1, o.qty - 1)
            o.qty -= q
            self.w.cancel(ts, s.loc, o.ref, q)
        else:
            s.book.remove(o)
            if not o.hidden:
                self.w.delete(ts, s.loc, o.ref)

    @staticmethod
    def _risk(s, o):
        return (o.price - s.fair) if o.side == "B" else (s.fair - o.price)

    def replace(self, s, ts, matching):
        o = self.pick_ref(s, displayed_only=True)
        if o is None:
            return
        b = s.book
        px = o.price + self.rng.choice((-TICK, 0, 0, TICK))
        opp = "S" if o.side == "B" else "B"
        ob = b.best_any(opp)
        if px <= 0 or (matching and ob is not None and ((o.side == "B" and px >= ob) or (o.side == "S" and px <= ob))):
            px = o.price
        qty = max(100, o.qty + self.rng.choice((-100, 0, 100, 200)))
        b.remove(o)
        n = Order(self.next_ref(), o.side, px, qty)
        b.rest(n)
        s.refs.append(n.ref)
        self.w.replace(ts, s.loc, o.ref, n.ref, qty, px)

    def aggressive(self, s, ts, group):
        b = s.book
        bb, ba = b.best_any("B"), b.best_any("S")
        mid = ((bb or ba or s.fair) + (ba or bb or s.fair)) / 2
        z = (s.fair - mid) / (TICK * max(1.0, s.half))
        side = "B" if self.rng.random() < 1 / (1 + math.exp(-2.0 * z)) else "S"
        opp_best = ba if side == "B" else bb
        if opp_best is None:
            return
        j = 0 if self.rng.random() < 0.7 else self.rng.randint(1, 3)
        limit = opp_best + j * TICK if side == "B" else opp_best - j * TICK
        if limit <= 0:
            return
        qty = self.lots(s) * (1 if self.rng.random() < 0.8 else 3)
        if self.rng.random() < 0.2:  # sweep the whole touch
            q = b.disp["S" if side == "B" else "B"].get(opp_best, ())
            qty += sum(o.qty for o in q)
        self.aggress(s, side, limit, qty, ts, post=self.rng.random() < 0.3, group=group)
        return side

    # ---- the day ----
    def run(self, syms, events):
        w = self.w
        w.system_event(self.at(hms(3, 5)), "O")
        for s in syms:
            w.stock_directory(self.at(hms(3, 10)), s.loc, s.name)
        w.system_event(self.at(hms(4, 0)), "S")
        for s in syms:
            w.trading_action(self.at(hms(4, 0, 1)), s.loc, s.name, "T")

        weights = [s.weight for s in syms]
        # Pre-open: resting only, book may cross.
        n_pre = events // 40
        t, t_end = hms(4, 0, 2), hms(9, 29, 59)
        for i in range(n_pre):
            s = self.rng.choices(syms, weights)[0]
            ts = self.at(t + (t_end - t) * i // n_pre)
            r = self.rng.random()
            if r < 0.8:
                self.passive(s, ts, matching=False)
                if self.rng.random() < 0.05:  # a crossing pre-market order
                    side = self.rng.choice("BS")
                    px = int(s.fair / TICK) * TICK + (2 * TICK if side == "B" else -2 * TICK)
                    self.add_resting(s, side, px, self.lots(s), ts)
            else:
                self.cancel(s, ts)

        w.system_event(self.at(hms(9, 30)), "Q")
        for s in syms:
            self.run_cross(s, self.at(hms(9, 30, 0.001)), "O")

        halted = syms[-1]
        halt_on, quote_on, halt_off = hms(12, 0), hms(12, 4), hms(12, 5)
        t, t_end = hms(9, 30, 0.01), hms(15, 59, 59)
        n_cont = events - n_pre
        dt = (t_end - t) / n_cont
        phase = 0
        for i in range(n_cont):
            ts = self.at(int(t + i * dt + self.rng.random() * dt * 0.5))
            if phase == 0 and ts >= halt_on:
                halted.state = "H"
                w.trading_action(ts, halted.loc, halted.name, "H", "LUDP")
                phase = 1
            if phase == 1 and ts >= quote_on:
                halted.state = "Q"
                w.trading_action(ts, halted.loc, halted.name, "Q", "LUDP")
                phase = 2
            if phase == 2 and ts >= halt_off:
                halted.state = "T"
                w.trading_action(ts, halted.loc, halted.name, "T")
                self.run_cross(halted, self.at(ts + 1000), "H")
                ts = self.ts
                phase = 3
            s = self.rng.choices(syms, weights)[0]
            s.fair += self.rng.gauss(0, TICK * 0.15 * max(1.0, s.half))
            matching = s.state == "T"
            r = self.rng.random()
            if r < 0.42:
                self.passive(s, ts, matching)
            elif r < 0.78:
                self.cancel(s, ts)
            elif r < 0.84:
                self.replace(s, ts, matching)
            elif matching:
                side = self.aggressive(s, ts, group=i)
                if side and self.rng.random() < 0.04:  # a second aggressor, same timestamp
                    self.aggressive(s, ts, group=i)
            else:  # halted or quote-only: orders rest unmatched and may cross
                self.passive(s, ts, matching=False)
                if self.rng.random() < 0.2:
                    side = self.rng.choice("BS")
                    px = int(s.fair / TICK) * TICK + (2 * TICK if side == "B" else -2 * TICK)
                    self.add_resting(s, side, px, self.lots(s), ts)
            if i == n_cont // 2 and self.match > 10:
                w.broken(ts, s.loc, self.match - 5)

        w.system_event(self.at(hms(16, 0)), "M")
        for s in syms:
            moc = self.lots(s) * 5
            self.closes[s.name] = self.run_cross(s, self.at(hms(16, 0, 0.5)), "C", extra_sell=moc)
        w.system_event(self.at(hms(20, 0)), "E")
        w.system_event(self.at(hms(20, 5)), "C")
        w.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="ITCH output path")
    ap.add_argument("--truth", required=True, help="ground-truth JSONL path")
    ap.add_argument("--closes", required=True, help="official closes CSV path")
    ap.add_argument("--events", type=int, default=200_000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    syms = [
        Sym(1, "TICKA", 12_0000, 0.05, 30, 3.0),   # tick-constrained, deep queues
        Sym(2, "TICKB", 25_0000, 0.05, 20, 2.0),
        Sym(3, "TICKC", 8_0000, 0.05, 40, 1.5),
        Sym(4, "MIDA", 60_0000, 1.5, 5, 1.5),
        Sym(5, "MIDB", 95_0000, 1.5, 4, 1.0),
        Sym(6, "WIDEA", 180_0000, 3.0, 2, 1.0),
        Sym(7, "WIDEB", 320_0000, 4.0, 2, 0.6),
        Sym(8, "HALTX", 40_0000, 1.0, 6, 0.8),
    ]
    ex = Exchange(a.out, a.seed)
    ex.run(syms, a.events)
    with open(a.truth, "w") as f:
        for t in ex.truth:
            f.write(json.dumps(t) + "\n")
    with open(a.closes, "w") as f:
        f.write("symbol,close\n")
        for k, v in ex.closes.items():
            f.write(f"{k},{v / 1e4:.4f}\n")
    print(f"wrote {ex.w.count} messages, {len(ex.truth)} aggressor events")


if __name__ == "__main__":
    main()
