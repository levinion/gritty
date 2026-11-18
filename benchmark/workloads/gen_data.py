#!/usr/bin/env python3
"""Generate deterministic terminal workloads in workloads/.cache."""
from __future__ import annotations

import argparse
import random
from pathlib import Path

CACHE = Path(__file__).resolve().parent / ".cache"
SIZES = {
    "ascii_64m.txt": 64 * 1024 * 1024,
    "truecolor_50k.txt": 50_000,
    "unicode_16m.txt": 16 * 1024 * 1024,
}
BOX = "┌─┬┐│└┴┘"


def ascii_payload(size: int) -> bytes:
    rng = random.Random(42)
    out = bytearray()
    column = 0
    while len(out) < size:
        if column == 120:
            out.append(10)
            column = 0
        else:
            out.append(rng.randrange(0x20, 0x7F))
            column += 1
    return bytes(out[:size])


def truecolor_payload(lines: int) -> bytes:
    rng = random.Random(42)
    out: list[str] = []
    for _ in range(lines):
        row: list[str] = []
        column = 0
        while column < 100:
            fg = rng.randrange(256), rng.randrange(256), rng.randrange(256)
            bg = rng.randrange(256), rng.randrange(256), rng.randrange(256)
            row.append(f"\x1b[38;2;{fg[0]};{fg[1]};{fg[2]}m")
            row.append(f"\x1b[48;2;{bg[0]};{bg[1]};{bg[2]}m")
            if rng.random() < 0.3:
                row.append("\x1b[1m")
            if rng.random() < 0.2:
                row.append("\x1b[3m")
            if rng.random() < 0.2:
                row.append("\x1b[4m")
            text = "".join(rng.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(rng.randint(2, 4)))
            row.append(text)
            column += len(text)
        out.append("".join(row))
    return ("\r\n".join(out) + "\r\n").encode()


def unicode_payload(size: int) -> bytes:
    rng = random.Random(42)
    cjk = "中文世界终端性能测试图形边界"
    units = list(cjk) + list(BOX) + ["e\u0301", "a\u0301", " "]
    out: list[str] = []
    column = 0
    total = 0
    while total < size:
        unit = rng.choice(units)
        out.append(unit)
        total += len(unit.encode())
        column += 2 if unit in cjk or unit in BOX else 1
        if column >= 100:
            out.append("\n")
            total += 1
            column = 0
    return "".join(out).encode()[:size]


def ensure(name: str, data: bytes) -> Path:
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if not path.exists() or path.stat().st_size != len(data):
        path.write_bytes(data)
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", choices=[*SIZES, "all"], default="all")
    args = parser.parse_args()
    names = list(SIZES) if args.name == "all" else [args.name]
    for name in names:
        if name == "ascii_64m.txt":
            data = ascii_payload(SIZES[name])
        elif name == "truecolor_50k.txt":
            data = truecolor_payload(SIZES[name])
        else:
            data = unicode_payload(SIZES[name])
        path = ensure(name, data)
        print(f"{path}: {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
