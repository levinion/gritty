#!/usr/bin/env python3
"""Run one workload inside a terminal and write structured timing data."""
from __future__ import annotations

import argparse
import json
import os
import resource
import shlex
import subprocess
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def rss_kib() -> int:
    try:
        for line in Path("/proc/self/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except OSError:
        pass
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--payload", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--label", default="")
    args = parser.parse_args()

    payload = Path(args.payload)
    command = ["python3", str(payload)] if payload.suffix == ".py" else ["/bin/sh", "-c", f"cat {shlex.quote(str(payload))}"]
    peak = [rss_kib()]
    stop = threading.Event()

    def sample() -> None:
        while not stop.wait(0.01):
            value = rss_kib()
            if value > peak[0]:
                peak[0] = value

    thread = threading.Thread(target=sample, daemon=True)
    thread.start()
    start = time.perf_counter()
    completed = subprocess.run(command, stdout=None, stderr=subprocess.DEVNULL, cwd=ROOT)
    elapsed = time.perf_counter() - start
    stop.set()
    thread.join(timeout=0.1)

    size = payload.stat().st_size if payload.exists() else 0
    # burst_100k.py produces ~1.6 MiB of visible output, not 1.6 KiB of source.
    if payload.name == "burst_100k.py":
        size = 100_000 * 16
    result = {
        "label": args.label or payload.name,
        "payload": str(payload),
        "elapsed_ms": elapsed * 1000.0,
        "throughput_mb_s": (size / (1024 * 1024)) / elapsed if elapsed else 0.0,
        "peak_parent_rss_kib": peak[0],
        "peak_children_rss_kib": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
        "returncode": completed.returncode,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result))
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
