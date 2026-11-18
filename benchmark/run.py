#!/usr/bin/env python3
"""Cross-terminal throughput/latency comparison for gritty.

Runs deterministic workloads inside each available terminal, records structured
results, prints a Markdown comparison table, and optionally saves JSON.
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKLOADS = ROOT / "benchmark" / "workloads"
RESULTS = ROOT / "benchmark" / "results"
CASES = ("ascii_64m", "truecolor_50k", "unicode_16m", "burst_100k")


def find_gritty(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for candidate in ("build-rel/src/gritty", "build/src/gritty", "build/release/src/gritty"):
        path = ROOT / candidate
        if path.is_file():
            return str(path)
    return None


def available_targets(gritty_bin: str | None, requested: list[str]) -> list[str]:
    binaries = {"gritty": gritty_bin}
    for name in ("alacritty", "kitty", "foot"):
        binaries[name] = shutil.which(name)
    missing = [name for name in requested if not binaries.get(name)]
    if missing:
        print(f"Skipping unavailable terminals: {', '.join(missing)}", file=sys.stderr)
    return [name for name in requested if binaries.get(name)]


def ensure_payloads() -> dict[str, Path]:
    subprocess.run([sys.executable, str(WORKLOADS / "gen_data.py")], check=True, stdout=subprocess.DEVNULL)
    return {
        "ascii_64m": WORKLOADS / ".cache" / "ascii_64m.txt",
        "truecolor_50k": WORKLOADS / ".cache" / "truecolor_50k.txt",
        "unicode_16m": WORKLOADS / ".cache" / "unicode_16m.txt",
        "burst_100k": WORKLOADS / "burst_100k.py",
    }


def terminal_command(target: str, binary: str, runner: list[str], out: Path) -> list[str]:
    inner = [sys.executable, str(WORKLOADS / "run_in_term.py"),
             "--payload", str(runner[0]), "--out", str(out), "--label", runner[1]]
    command: list[str] = [binary]
    if target == "gritty":
        command += ["-e", "/bin/sh", "-c", " ".join(_quote(part) for part in inner)]
    elif target == "alacritty":
        command += ["-o", "window.dimensions.columns=160", "-o", "window.dimensions.lines=48",
                    "-e", "/bin/sh", "-c", " ".join(_quote(part) for part in inner)]
    elif target == "kitty":
        # macOS kitty launcher stays alive until the OS window closes unless
        # detached; poll the JSON file instead of waiting on the launcher.
        command += ["--detach", "-o", "initial_window_width=160c", "-o", "initial_window_height=48c",
                    "-o", "remember_window_size=no", "/bin/sh", "-c",
                    " ".join(_quote(part) for part in inner)]
    elif target == "foot":
        command += ["-w", "160x48", "/bin/sh", "-c", " ".join(_quote(part) for part in inner)]
    return command


def _quote(value: str) -> str:
    return "'" + value.replace("'", "'\\''") + "'"


def run_once(target: str, binary: str, payload: tuple[Path, str], timeout: float) -> dict:
    with tempfile.TemporaryDirectory(prefix="gritty-bench-") as directory:
        out = Path(directory) / "result.json"
        command = terminal_command(target, binary, [str(payload[0]), payload[1]], out)
        started = time.monotonic()
        completed = subprocess.run(command, cwd=ROOT, check=False, timeout=timeout)
        while not out.exists() and time.monotonic() - started < timeout:
            time.sleep(0.05)
        elapsed = time.monotonic() - started
        if not out.exists():
            raise RuntimeError(f"{target} did not produce {out}: exit={completed.returncode}, {elapsed:.1f}s")
        result = json.loads(out.read_text())
        if result.get("returncode") != 0:
            raise RuntimeError(f"{target} workload failed: {result}")
        result["wall_s"] = elapsed
        return result


def summarize(samples: list[dict]) -> dict:
    elapsed = [sample["elapsed_ms"] for sample in samples]
    throughput = [sample["throughput_mb_s"] for sample in samples]
    return {
        "elapsed_ms_median": statistics.median(elapsed),
        "elapsed_ms_mean": statistics.fmean(elapsed),
        "elapsed_ms_stdev": statistics.stdev(elapsed) if len(elapsed) > 1 else 0.0,
        "throughput_mb_s_median": statistics.median(throughput),
        "throughput_mb_s_mean": statistics.fmean(throughput),
        "throughput_mb_s_stdev": statistics.stdev(throughput) if len(throughput) > 1 else 0.0,
        "samples": samples,
    }


def print_table(data: dict, targets: list[str]) -> None:
    for case in CASES:
        print(f"\n### {case}")
        print("| terminal | median ms | mean ± stdev ms | median MB/s | speedup vs gritty |")
        print("|---|---:|---:|---:|---:|")
        baseline = data.get("gritty", {}).get(case, {}).get("elapsed_ms_median") if "gritty" in targets else None
        for target in targets:
            result = data.get(target, {}).get(case)
            if not result:
                continue
            speedup = f"{baseline / result['elapsed_ms_median']:.2f}x" if baseline and result['elapsed_ms_median'] else "-"
            print(f"| {target} | {result['elapsed_ms_median']:.1f} | "
                  f"{result['elapsed_ms_mean']:.1f} ± {result['elapsed_ms_stdev']:.1f} | "
                  f"{result['throughput_mb_s_median']:.2f} | {speedup} |")


def run_vtebench(targets: list[str], binaries: dict[str, str | None], rounds: int) -> dict:
    results: dict[str, object] = {}
    for target in targets:
        binary = binaries.get(target)
        if not binary or not shutil.which("vtebench"):
            continue
        samples = []
        for _ in range(rounds):
            command = ["vtebench", "scrolling", "--terminal", binary]
            started = time.perf_counter()
            subprocess.run(command, cwd=ROOT, check=False)
            samples.append((time.perf_counter() - started) * 1000.0)
        results[target] = {"elapsed_ms_median": statistics.median(samples), "samples": samples}
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targets", default="gritty,alacritty,kitty")
    parser.add_argument("--gritty-bin")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--vtebench", action="store_true")
    parser.add_argument("--cases", default=",".join(CASES))
    parser.add_argument("--save", type=Path)
    args = parser.parse_args()

    if args.rounds < 1:
        raise SystemExit("--rounds must be >= 1")
    cases = [name.strip() for name in args.cases.split(",") if name.strip()]
    unknown = [name for name in cases if name not in CASES]
    if unknown:
        raise SystemExit(f"Unknown cases: {', '.join(unknown)}")
    gritty = find_gritty(args.gritty_bin)
    requested = [name.strip() for name in args.targets.split(",") if name.strip()]
    targets = available_targets(gritty, requested)
    if not targets:
        raise SystemExit("No requested terminals are available")
    binaries = {"gritty": gritty, "alacritty": shutil.which("alacritty"),
                "kitty": shutil.which("kitty"), "foot": shutil.which("foot")}
    payloads = ensure_payloads()

    data: dict[str, dict[str, dict]] = {}
    for target in targets:
        assert binaries[target]
        data[target] = {}
        for case in cases:
            payload = (payloads[case], case)
            print(f"[{target}] warmup {case}", file=sys.stderr)
            try:
                run_once(target, str(binaries[target]), payload, args.timeout)
                samples = []
                for round_index in range(args.rounds):
                    print(f"[{target}] round {round_index + 1}/{args.rounds} {case}", file=sys.stderr)
                    samples.append(run_once(target, str(binaries[target]), payload, args.timeout))
            except (subprocess.TimeoutExpired, RuntimeError) as error:
                print(f"[{target}] {case} failed: {error}", file=sys.stderr)
                continue
            data[target][case] = summarize(samples)

    if args.vtebench:
        data["vtebench"] = run_vtebench(targets, binaries, args.rounds)
    print_table(data, targets)
    if not any(data.get(target) for target in targets):
        raise SystemExit("No benchmark case completed")

    if args.save:
        args.save.parent.mkdir(parents=True, exist_ok=True)
        args.save.write_text(json.dumps({"targets": targets, "rounds": args.rounds, "data": data}, indent=2) + "\n")
        print(f"\nSaved: {args.save}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
