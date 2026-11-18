#!/usr/bin/env python3
"""Cross-terminal throughput/latency comparison for gritty.

Runs deterministic workloads inside each available terminal, records structured
results, prints a Markdown comparison table, and optionally saves JSON.

For cross-version comparisons, --ab-bins A B runs each case in A -> B -> B -> A
order and reports medians from both binaries. Alternating in both directions
cancels most macOS WindowServer/thermal drift.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
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
COLUMNS = 160
LINES = 48
ABBA_ORDER = ("A", "B", "B", "A")


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


def _quote(value: str | Path) -> str:
    value = str(value)
    return "'" + value.replace("'", "'\\''") + "'"


def _workload_command(payload: tuple[Path, str], out: Path, gate: Path | None) -> list[str]:
    inner = [
        sys.executable, str(WORKLOADS / "run_in_term.py"),
        "--payload", str(payload[0]), "--out", str(out), "--label", payload[1],
    ]
    command = " ".join(_quote(part) for part in inner)
    if gate is not None:
        command = f"while [ ! -f {_quote(gate)} ]; do sleep 0.01; done; exec {command}"
    return ["/bin/sh", "-c", command]


def terminal_command(
    target: str,
    binary: str,
    payload: tuple[Path, str],
    out: Path,
    gate: Path | None,
    socket: Path | None,
) -> list[str]:
    inner = _workload_command(payload, out, gate)
    command: list[str] = [binary]
    if target == "gritty":
        command += ["--cols", str(COLUMNS), "--rows", str(LINES), "-e", *inner]
    elif target == "alacritty":
        if socket is None:
            raise RuntimeError("alacritty control socket is required")
        command += [
            "--socket", str(socket),
            "-o", f"window.dimensions.columns={COLUMNS}",
            "-o", f"window.dimensions.lines={LINES}",
            "-e", *inner,
        ]
    elif target == "kitty":
        if socket is None:
            raise RuntimeError("kitty control socket is required")
        # -c NONE prevents host config from changing geometry or close policy.
        # A private control socket lets the harness resize after window-manager
        # rules have run, then close the window reliably when the workload ends.
        command += [
            "--detach", "--single-instance=no",
            "--listen-on", f"unix:{socket}",
            "-c", "NONE",
            "-o", "allow_remote_control=socket-only",
            "-o", "close_on_child_death=yes",
            "-o", "remember_window_size=no",
            "-o", f"initial_window_width={COLUMNS}c",
            "-o", f"initial_window_height={LINES}c",
            *inner,
        ]
    elif target == "foot":
        command += ["-w", f"{COLUMNS}x{LINES}", *inner]
    return command


def _control_address(socket: Path) -> str:
    return f"unix:{socket}"


def _run_control(args: list[str], timeout: float = 5.0) -> subprocess.CompletedProcess:
    return subprocess.run(
        args, cwd=ROOT, check=False, timeout=timeout,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def _wait_for_control(target: str, socket: Path, deadline: float) -> None:
    while time.monotonic() < deadline:
        if target == "kitty":
            result = _run_control(["kitten", "@", "--to", _control_address(socket), "ls"])
            if result.returncode == 0:
                return
        elif target == "alacritty":
            if socket.exists():
                result = _run_control(["alacritty", "msg", "-s", str(socket), "get-config"])
                if result.returncode == 0:
                    return
        time.sleep(0.05)
    raise RuntimeError(f"{target} control socket did not become ready: {socket}")


def _enforce_size(target: str, socket: Path) -> None:
    if target == "kitty":
        args = [
            "kitten", "@", "--to", _control_address(socket),
            "resize-os-window", "--match", "all", "--unit", "cells",
            "--width", str(COLUMNS), "--height", str(LINES),
        ]
    elif target == "alacritty":
        args = [
            "alacritty", "msg", "-s", str(socket), "config", "-w", "-1",
            f"window.dimensions.columns={COLUMNS}",
            f"window.dimensions.lines={LINES}",
        ]
    else:
        return
    for _ in range(20):
        if _run_control(args).returncode == 0:
            time.sleep(0.05)  # Let the window manager settle before the workload.
            return
        time.sleep(0.05)
    print(f"warning: failed to force {target} geometry to {COLUMNS}x{LINES}", file=sys.stderr)


def _pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError):
        return False
    return True


def _kill_kitty_by_socket(socket: Path) -> None:
    # Match the real kitty process, not this Python process (whose command line
    # also contains the temporary socket path) and not helper `kitten` tools.
    # macOS `ps` renders argv as `--listen-on unix:/path` (space separated).
    needle = _control_address(socket)
    try:
        output = subprocess.run(
            ["ps", "-eo", "pid=,command="], check=False, capture_output=True, text=True,
            timeout=5.0,
        ).stdout
    except (OSError, subprocess.TimeoutExpired):
        return
    pids: list[int] = []
    for line in output.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) != 2:
            continue
        command = fields[1]
        executable = Path(command.split(maxsplit=1)[0]).name
        if (needle not in command or "--listen-on" not in command or
                executable != "kitty"):
            continue
        try:
            pids.append(int(fields[0]))
        except ValueError:
            pass
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass
    time.sleep(0.5)
    for pid in pids:
        if _pid_alive(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass


def _cleanup_control(target: str, socket: Path | None) -> None:
    if target != "kitty" or socket is None:
        return
    _run_control([
        "kitten", "@", "--to", _control_address(socket),
        "close-window", "--match", "all", "--ignore-no-match", "--no-response",
    ])
    _kill_kitty_by_socket(socket)


def run_once(target: str, binary: str, payload: tuple[Path, str], timeout: float) -> dict:
    with tempfile.TemporaryDirectory(prefix="gritty-bench-") as directory:
        work = Path(directory)
        out = work / "result.json"
        gate = work / "go" if target in ("alacritty", "kitty") else None
        socket = work / "ipc.sock" if target in ("alacritty", "kitty") else None
        command = terminal_command(target, binary, payload, out, gate, socket)
        started = time.monotonic()
        deadline = started + timeout
        process = subprocess.Popen(
            command, cwd=ROOT,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            if gate is not None and socket is not None:
                _wait_for_control(target, socket, deadline)
                _enforce_size(target, socket)
                gate.write_text("go\n")
            process.wait(timeout=max(0.1, deadline - time.monotonic()))
            while not out.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            elapsed = time.monotonic() - started
            if not out.exists():
                raise RuntimeError(f"{target} did not produce {out}: exit={process.returncode}, {elapsed:.1f}s")
            result = json.loads(out.read_text())
            if result.get("returncode") != 0:
                raise RuntimeError(f"{target} workload failed: {result}")
            result["wall_s"] = elapsed
            return result
        finally:
            _cleanup_control(target, socket)
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2.0)


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


def print_ab_table(data: dict) -> None:
    bins = data["bins"]
    print("\nA/B medians (ABBA order; A = " + bins["A"] + ", B = " + bins["B"] + ")")
    for case in CASES:
        result = data["cases"].get(case)
        if not result:
            continue
        a = result["A"]["throughput_mb_s_median"]
        b = result["B"]["throughput_mb_s_median"]
        print(f"\n### {case}")
        print("| binary | median ms | median MB/s | delta vs A |")
        print("|---|---:|---:|---:|")
        print(f"| A | {result['A']['elapsed_ms_median']:.1f} | {a:.2f} | - |")
        print(f"| B | {result['B']['elapsed_ms_median']:.1f} | {b:.2f} | "
              f"{b - a:+.2f} MB/s ({b / a - 1:+.1%}) |")


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


def run_ab(bin_a: str, bin_b: str, cases: list[str], rounds: int, timeout: float) -> dict:
    payloads = ensure_payloads()
    data: dict = {
        "mode": "abba",
        "order": list(ABBA_ORDER),
        "ab_rounds": rounds,
        "bins": {"A": str(Path(bin_a).resolve()), "B": str(Path(bin_b).resolve())},
        "geometry": {"columns": COLUMNS, "lines": LINES},
        "cases": {},
    }
    for case in cases:
        payload = (payloads[case], case)
        samples: dict[str, list[dict]] = {"A": [], "B": []}
        order_seen: list[str] = []
        for _ in range(rounds):
            for label in ABBA_ORDER:
                binary = bin_a if label == "A" else bin_b
                print(f"[{label}] {case}", file=sys.stderr)
                samples[label].append(run_once("gritty", binary, payload, timeout))
                order_seen.append(label)
        data["cases"][case] = {
            "A": summarize(samples["A"]),
            "B": summarize(samples["B"]),
            "order": order_seen,
        }
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targets", default="gritty,alacritty,kitty")
    parser.add_argument("--gritty-bin")
    parser.add_argument("--ab-bins", nargs=2, metavar=("BIN_A", "BIN_B"))
    parser.add_argument("--ab-rounds", type=int, default=2)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--vtebench", action="store_true")
    parser.add_argument("--cases", default=",".join(CASES))
    parser.add_argument("--save", type=Path)
    args = parser.parse_args()

    if args.rounds < 1 or args.ab_rounds < 1:
        raise SystemExit("--rounds and --ab-rounds must be >= 1")
    cases = [name.strip() for name in args.cases.split(",") if name.strip()]
    unknown = [name for name in cases if name not in CASES]
    if unknown:
        raise SystemExit(f"Unknown cases: {', '.join(unknown)}")

    if args.ab_bins:
        if args.vtebench:
            raise SystemExit("--vtebench is not supported with --ab-bins")
        bin_a, bin_b = args.ab_bins
        for binary in (bin_a, bin_b):
            if not Path(binary).is_file():
                raise SystemExit(f"Binary not found: {binary}")
        data = run_ab(bin_a, bin_b, cases, args.ab_rounds, args.timeout)
        print_ab_table(data)
        if args.save:
            args.save.parent.mkdir(parents=True, exist_ok=True)
            args.save.write_text(json.dumps(data, indent=2) + "\n")
            print(f"\nSaved: {args.save}")
        return 0

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
