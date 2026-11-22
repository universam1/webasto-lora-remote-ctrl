"""
Capture file formats and offline analysis (summarize, diff, decode_3e5).

Pure module: functions take paths or in-memory frames; nothing here touches
the serial port.
"""

from __future__ import annotations

import csv
import json
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from statistics import mean
from typing import Iterable, Sequence

CSV_HEADER = "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8"


@dataclass(frozen=True)
class Frame:
    ts_us: int  # device timestamp (unwrapped, microseconds)
    can_id: int
    extended: bool
    direction: str  # "Rx" or "Tx"
    bus: int
    data: bytes


@dataclass(frozen=True)
class Marker:
    label: str
    ts_us: int
    host_time: float | None = None


@dataclass
class Capture:
    name: str
    frames: list[Frame]
    markers: list[Marker]

    @property
    def origin_us(self) -> int:
        if self.frames:
            return self.frames[0].ts_us
        if self.markers:
            return self.markers[0].ts_us
        return 0

    def rel(self, ts_us: int) -> float:
        return (ts_us - self.origin_us) / 1e6


# --------------------------------------------------------------------------- formats


def format_csv_row(f: Frame) -> str:
    """SavvyCAN GVRET CSV row; every field is followed by a comma, data padded to 8."""
    data = list(f.data[:8]) + [0] * (8 - min(len(f.data), 8))
    fields = [
        str(f.ts_us),
        f"{f.can_id:08X}",
        "true" if f.extended else "false",
        f.direction,
        str(f.bus),
        str(len(f.data)),
    ] + [f"{b:02X}" for b in data]
    return ",".join(fields) + ","


def format_candump(f: Frame, epoch_s: float) -> str:
    """candump -L line, e.g. `(1700000000.123456) can0 3E5#6080EE8627000000`."""
    cid = f"{f.can_id:08X}" if f.extended else f"{f.can_id:03X}"
    return f"({epoch_s:.6f}) can{f.bus} {cid}#{f.data.hex().upper()}"


def parse_csv_row(row: Sequence[str]) -> Frame | None:
    if len(row) < 6:
        return None
    try:
        ts = int(row[0])
        cid = int(row[1], 16)
        ext = row[2].strip().lower() == "true"
        direction = row[3].strip() or "Rx"
        bus = int(row[4])
        length = int(row[5])
        data = bytes(int(x, 16) for x in row[6 : 6 + length])
    except ValueError:
        return None
    return Frame(ts, cid, ext, direction, bus, data)


def load_markers(path: Path) -> list[Marker]:
    out: list[Marker] = []
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
            out.append(Marker(str(d["label"]), int(d["device_ts_us"]), d.get("host_time")))
        except (ValueError, KeyError, TypeError):
            continue
    return out


def resolve_capture(name_or_path: str, captures_dir: Path) -> Path:
    p = Path(name_or_path)
    candidates = [p, p.with_suffix(".csv"), captures_dir / p, captures_dir / f"{name_or_path}.csv"]
    for c in candidates:
        if c.is_file() and c.suffix.lower() == ".csv":
            return c
    raise FileNotFoundError(f"capture not found: {name_or_path} (looked in {captures_dir})")


def load_capture(name_or_path: str, captures_dir: Path) -> Capture:
    path = resolve_capture(name_or_path, captures_dir)
    frames: list[Frame] = []
    with path.open(newline="") as fh:
        for row in csv.reader(fh):
            if not row or row[0].startswith("Time"):
                continue
            f = parse_csv_row(row)
            if f is not None:
                frames.append(f)
    frames.sort(key=lambda f: f.ts_us)
    markers = load_markers(path.with_name(path.stem + ".markers.jsonl"))
    return Capture(path.stem, frames, markers)


# --------------------------------------------------------------------------- helpers


def _fmt_id(can_id: int, extended: bool) -> str:
    return f"{can_id:08X}" if extended else f"{can_id:03X}"


def _key(f: Frame) -> tuple[int, bool]:
    return (f.can_id, f.extended)


def _slice(cap: Capture, start_s: float | None, end_s: float | None, rx_only: bool = True) -> list[Frame]:
    o = cap.origin_us
    lo = None if start_s is None else o + int(start_s * 1e6)
    hi = None if end_s is None else o + int(end_s * 1e6)
    return [
        f
        for f in cap.frames
        if (not rx_only or f.direction == "Rx")
        and (lo is None or f.ts_us >= lo)
        and (hi is None or f.ts_us < hi)
    ]


def _fmt_values(values: Iterable[int], limit: int = 8) -> str:
    vals = sorted(set(values))
    if len(vals) <= limit:
        return "{" + ",".join(f"{v:02X}" for v in vals) + "}"
    return f"{vals[0]:02X}..{vals[-1]:02X} ({len(vals)} distinct)"


@dataclass
class IdStats:
    key: tuple[int, bool]
    count: int
    period_ms: float | None
    dlcs: list[int]
    byte_values: list[set[int]]  # per byte position
    toggled: list[int]  # per byte position: OR of bits that changed between frames
    bit_ones: list[list[int]]  # per byte, per bit: count of frames with bit set
    bit_total: list[int]  # per byte: frames that had this byte


def id_stats(frames: Sequence[Frame]) -> dict[tuple[int, bool], IdStats]:
    groups: dict[tuple[int, bool], list[Frame]] = defaultdict(list)
    for f in frames:
        groups[_key(f)].append(f)
    out = {}
    for key, fs in groups.items():
        maxlen = max(len(f.data) for f in fs)
        byte_values: list[set[int]] = [set() for _ in range(maxlen)]
        toggled = [0] * maxlen
        bit_ones = [[0] * 8 for _ in range(maxlen)]
        bit_total = [0] * maxlen
        prev: bytes | None = None
        for f in fs:
            for i, b in enumerate(f.data):
                byte_values[i].add(b)
                bit_total[i] += 1
                for bit in range(8):
                    if b >> bit & 1:
                        bit_ones[i][bit] += 1
                if prev is not None and i < len(prev):
                    toggled[i] |= b ^ prev[i]
            prev = f.data
        period = None
        if len(fs) > 1:
            period = (fs[-1].ts_us - fs[0].ts_us) / (len(fs) - 1) / 1000.0
        out[key] = IdStats(key, len(fs), period, sorted({len(f.data) for f in fs}), byte_values, toggled, bit_ones, bit_total)
    return out


def _bits_str(i: int, mask: int) -> str:
    return " ".join(f"B{i}.{b}" for b in range(8) if mask >> b & 1)


# --------------------------------------------------------------------------- summarize


def summarize(cap: Capture, start_s: float | None = None, end_s: float | None = None) -> str:
    frames = _slice(cap, start_s, end_s)
    tx = sum(1 for f in cap.frames if f.direction == "Tx")
    if not frames:
        return f"{cap.name}: no Rx frames in selected range"
    dur = (frames[-1].ts_us - frames[0].ts_us) / 1e6
    lines = [
        f"{cap.name}: {len(frames)} Rx frames, {dur:.2f} s"
        + (f", {tx} Tx frames (not analysed)" if tx else "")
        + f", range t={cap.rel(frames[0].ts_us):.3f}..{cap.rel(frames[-1].ts_us):.3f} s",
    ]
    if cap.markers:
        lines.append("markers: " + ", ".join(f"{m.label}@{cap.rel(m.ts_us):.3f}s" for m in cap.markers))
    lines.append("ID        count  period_ms  DLC   changing bytes (values) | toggled bits")
    stats = id_stats(frames)
    for key in sorted(stats):
        s = stats[key]
        period = f"{s.period_ms:9.1f}" if s.period_ms is not None else "        -"
        dlc = ",".join(map(str, s.dlcs))
        changing = [i for i, v in enumerate(s.byte_values) if len(v) > 1]
        if changing:
            ch = " ".join(f"B{i}{_fmt_values(s.byte_values[i])}" for i in changing)
            bits = " ".join(_bits_str(i, s.toggled[i]) for i in changing if s.toggled[i])
            detail = f"{ch} | {bits}"
        else:
            detail = "static"
        lines.append(f"{_fmt_id(*key):<9} {s.count:5d}  {period}  {dlc:<4}  {detail}")
    return "\n".join(lines)


# --------------------------------------------------------------------------- diff


def find_marker(cap: Capture, marker: str) -> Marker:
    """Marker by label (first match), by `label#n` (n-th match, 1-based) or by index."""
    if not cap.markers:
        raise ValueError(f"{cap.name} has no markers")
    if marker.lstrip("-").isdigit() and not any(m.label == marker for m in cap.markers):
        return cap.markers[int(marker)]
    label, _, nth = marker.rpartition("#") if "#" in marker else (marker, "", "1")
    matches = [m for m in cap.markers if m.label == label]
    if not matches:
        matches = [m for m in cap.markers if label.lower() in m.label.lower()]
    n = int(nth) if nth.isdigit() else 1
    if len(matches) < n:
        raise ValueError(f"marker {marker!r} not found; available: {[m.label for m in cap.markers]}")
    return matches[n - 1]


def diff_frames(a: Sequence[Frame], b: Sequence[Frame], dur_a: float, dur_b: float, label_a: str = "A", label_b: str = "B", max_ids: int = 40) -> str:
    sa, sb = id_stats(a), id_stats(b)
    lines = [f"A={label_a}: {len(a)} frames / {dur_a:.2f} s    B={label_b}: {len(b)} frames / {dur_b:.2f} s"]
    only_a = sorted(set(sa) - set(sb))
    only_b = sorted(set(sb) - set(sa))
    if only_a:
        lines.append("only in A: " + " ".join(f"{_fmt_id(*k)}({sa[k].count})" for k in only_a))
    if only_b:
        lines.append("only in B: " + " ".join(f"{_fmt_id(*k)}({sb[k].count})" for k in only_b))

    rate_lines = []
    scored: list[tuple[float, str]] = []
    for key in sorted(set(sa) & set(sb)):
        x, y = sa[key], sb[key]
        ra = x.count / dur_a if dur_a > 0 else 0.0
        rb = y.count / dur_b if dur_b > 0 else 0.0
        if ra > 0 and abs(rb - ra) / ra > 0.20:
            rate_lines.append(f"{_fmt_id(*key)} {ra:.1f}->{rb:.1f}/s ({(rb - ra) / ra:+.0%})")

        score = 0.0
        parts = []
        nbytes = max(len(x.byte_values), len(y.byte_values))
        for i in range(nbytes):
            va = x.byte_values[i] if i < len(x.byte_values) else set()
            vb = y.byte_values[i] if i < len(y.byte_values) else set()
            if va == vb:
                continue
            flipped = []  # bits constant on both sides with different values
            shifted = []  # bits whose duty changed noticeably
            for bit in range(8):
                fa = x.bit_ones[i][bit] / x.bit_total[i] if i < len(x.bit_total) and x.bit_total[i] else None
                fb = y.bit_ones[i][bit] / y.bit_total[i] if i < len(y.bit_total) and y.bit_total[i] else None
                if fa is None or fb is None:
                    continue
                if fa in (0.0, 1.0) and fb in (0.0, 1.0) and fa != fb:
                    flipped.append(f"b{bit}:{int(fa)}->{int(fb)}")
                    score += 10
                elif abs(fa - fb) >= 0.25:
                    shifted.append(f"b{bit}:{fa:.0%}->{fb:.0%}")
                    score += abs(fa - fb) * 2
            if len(va) == 1 and len(vb) == 1:
                score += 3  # constant byte changed to another constant
            elif not (va & vb):
                score += 1
            else:
                score += 0.2
            desc = f"B{i} {_fmt_values(va)}->{_fmt_values(vb)}"
            if flipped:
                desc += " flip " + " ".join(flipped)
            if shifted:
                desc += " duty " + " ".join(shifted)
            parts.append(desc)
        if parts:
            scored.append((score, f"{_fmt_id(*key)} [{score:.0f}] " + "; ".join(parts)))

    if rate_lines:
        lines.append("rate change >20%: " + ", ".join(rate_lines))
    scored.sort(key=lambda t: -t[0])
    if scored:
        lines.append("payload differences (most relevant first; score in []):")
        lines.extend("  " + s for _, s in scored[:max_ids])
        if len(scored) > max_ids:
            lines.append(f"  ... {len(scored) - max_ids} more IDs with minor differences")
    if not (only_a or only_b or rate_lines or scored):
        lines.append("no differences found")
    return "\n".join(lines)


def diff_marker(cap: Capture, marker: str, window_s: float = 5.0) -> str:
    m = find_marker(cap, marker)
    t = cap.rel(m.ts_us)
    a = _slice(cap, t - window_s, t)
    b = _slice(cap, t, t + window_s)
    head = f"{cap.name}: marker '{m.label}' at t={t:.3f} s, window {window_s} s before (A) vs after (B)"
    return head + "\n" + diff_frames(a, b, _span(a, window_s), _span(b, window_s), "before", "after")


def diff_captures(cap_a: Capture, cap_b: Capture) -> str:
    a = _slice(cap_a, None, None)
    b = _slice(cap_b, None, None)
    return diff_frames(a, b, _span(a, 0), _span(b, 0), cap_a.name, cap_b.name)


def _span(frames: Sequence[Frame], default: float) -> float:
    if default > 0:
        return default
    if len(frames) < 2:
        return 0.0
    return (frames[-1].ts_us - frames[0].ts_us) / 1e6


# --------------------------------------------------------------------------- 0x3E5

ERROR_CODES = {0: "none", 1: "battery_low", 2: "low_fuel", 3: "heater_defective", 4: "crash", 5: "other", 6: "overheat"}


def error_code_3e5(d: bytes) -> int:
    """Error code of a 0x3E5 payload (byte 1 bits 4-6, see ERROR_CODES); 0 means no error."""
    return d[1] >> 4 & 0x07 if len(d) > 1 else 0


def decode_3e5_payload(d: bytes) -> dict[str, object]:
    d = bytes(d) + b"\x00" * max(0, 8 - len(d))
    b0, b1 = d[0], d[1]
    u23 = d[2] | d[3] << 8
    coolant = "error" if d[5] == 0xFF else round(-48 + 0.75 * d[5], 2)
    err = error_code_3e5(d)
    return {
        "on_via_radio": b0 & 1,
        "off_via_radio": b0 >> 1 & 1,
        "heating_on": b0 >> 2 & 1,
        "ventilation_on": b0 >> 3 & 1,
        "ac_off": b0 >> 4 & 1,
        "heater_mode_activating": b0 >> 5 & 1,
        "heater_programming": b0 >> 6 & 1,
        "wake_komfort_can": b0 >> 7 & 1,
        "vehicle_blower": b1 & 1,
        "control_led": b1 >> 1 & 1,
        "combustion_on": b1 >> 2 & 1,
        "water_pump_request": b1 >> 3 & 1,
        "error_code": f"{err}:{ERROR_CODES.get(err, '?')}",
        "error_stored": b1 >> 7 & 1,
        "fuel_used_ml": u23 & 0x3FFF,
        "parking_heater": u23 >> 14 & 1,
        "kl30_lost": u23 >> 15 & 1,
        "heating_power_raw": d[4],
        "coolant_c": coolant,
        "fuel_pump_on": d[6] & 1,
        "water_warmed_up": d[6] >> 1 & 1,
        "ventilation_active": d[7] & 1,
    }


def decode_3e5(cap: Capture, max_rows: int = 50, can_id: int = 0x3E5) -> str:
    frames = [f for f in cap.frames if f.can_id == can_id and not f.extended and f.direction == "Rx"]
    note = f"NOTE: 0x{can_id:03X} layout is unverified (taken from T5.1 documentation); treat as hypothesis."
    if not frames:
        return f"{cap.name}: no frames with ID 0x{can_id:03X}\n{note}"
    lines = [f"{cap.name}: {len(frames)} frames of 0x{can_id:03X}; rows shown only where the decoded value changes", note]
    events: list[tuple[int, int, object]] = [(m.ts_us, 0, m) for m in cap.markers]
    events += [(f.ts_us, 1, f) for f in frames]
    events.sort(key=lambda e: (e[0], e[1]))
    prev: dict[str, object] | None = None
    rows = 0
    truncated = 0
    for ts, kind, obj in events:
        t = cap.rel(ts)
        if kind == 0:
            if rows < max_rows:
                lines.append(f"{t:10.3f}  --- marker: {obj.label} ---")  # type: ignore[union-attr]
            continue
        dec = decode_3e5_payload(obj.data)  # type: ignore[union-attr]
        if prev is None:
            changed = {k: v for k, v in dec.items()}
        else:
            changed = {k: v for k, v in dec.items() if prev[k] != v}
        prev = dec
        if not changed:
            continue
        if rows >= max_rows:
            truncated += 1
            continue
        rows += 1
        raw = obj.data.hex(" ").upper()  # type: ignore[union-attr]
        kv = " ".join(f"{k}={v}" for k, v in changed.items())
        lines.append(f"{t:10.3f}  {raw:<23}  {kv}")
    if truncated:
        lines.append(f"... {truncated} more change rows (raise max_rows)")
    return "\n".join(lines)
