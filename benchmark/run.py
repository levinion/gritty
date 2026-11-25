#!/usr/bin/env python3
"""Cross-terminal and A/B throughput benchmark runner backed by alacritty/vtebench.

By default this script bootstraps into `nix develop .#throughput` (unless
`vtebench` and `GRITTY_VTEBENCH_BENCHMARKS` are already present or `--no-nix` is
passed), launches each requested terminal emulator with normalized cell geometry,
runs `alacritty/vtebench`, parses the generated `vtebench.dat` sample columns,
and prints a Markdown summary table (plus optional JSON / raw `.dat` export).

For A/B comparisons, `--ab-bins A B` executes rounds in `A -> B -> B -> A`
order to cancel thermal and WindowServer drift.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ABBA_ORDER = ("A", "B", "B", "A")
SUPPORTED_TARGETS = ("gritty", "alacritty", "kitty", "ghostty", "foot")


def _find_nix_command(argv: list[str]) -> list[str]:
    if shutil.which("nix"):
        return [
            "nix",
            "--extra-experimental-features",
            "nix-command flakes",
            "develop",
            ".#throughput",
            "--command",
            sys.executable,
            str(Path(__file__).resolve()),
            *argv,
        ]
    daemon_sh = Path("/nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh")
    if daemon_sh.exists():
        quoted = " ".join(
            shlex.quote(arg)
            for arg in (
                "nix",
                "--extra-experimental-features",
                "nix-command flakes",
                "develop",
                ".#throughput",
                "--command",
                sys.executable,
                str(Path(__file__).resolve()),
                *argv,
            )
        )
        return ["/bin/bash", "-lc", f". {shlex.quote(str(daemon_sh))} && exec {quoted}"]
    raise SystemExit("vtebench is not on PATH and nix could not be found; pass --no-nix or install Nix.")


def ensure_vtebench_env(no_nix: bool) -> Path:
    corpus_env = os.environ.get("GRITTY_VTEBENCH_BENCHMARKS")
    if shutil.which("vtebench") and corpus_env and Path(corpus_env).is_dir():
        return Path(corpus_env)
    if no_nix or os.environ.get("GRITTY_VTEBENCH_BOOTSTRAPPED") == "1":
        raise SystemExit(
            "Missing `vtebench` or `GRITTY_VTEBENCH_BENCHMARKS`. "
            "Run inside `nix develop .#throughput`."
        )
    env = dict(os.environ)
    env["GRITTY_VTEBENCH_BOOTSTRAPPED"] = "1"
    cmd = _find_nix_command(sys.argv[1:])
    os.execvpe(cmd[0], cmd, env)


def discover_cases(corpus: Path) -> list[str]:
    return sorted(
        entry.name
        for entry in corpus.iterdir()
        if entry.is_dir() and (entry / "benchmark").is_file()
    )


def find_gritty(explicit: str | None) -> str | None:
    if explicit:
        path = Path(explicit)
        return str(path) if path.is_file() else shutil.which(explicit)
    for candidate in ("build/release/src/gritty", "build/debug/src/gritty"):
        path = ROOT / candidate
        if path.is_file():
            return str(path)
    return shutil.which("gritty")


def resolve_binaries(gritty_bin: str | None, requested: list[str]) -> tuple[list[str], dict[str, str]]:
    found: dict[str, str] = {}
    missing: list[str] = []
    for name in requested:
        if name not in SUPPORTED_TARGETS:
            raise SystemExit(f"Unsupported target terminal '{name}' (choices: {', '.join(SUPPORTED_TARGETS)})")
        binary = gritty_bin if name == "gritty" else shutil.which(name)
        if binary:
            found[name] = binary
        else:
            missing.append(name)
    if missing:
        print(f"Skipping unavailable terminals: {', '.join(missing)}", file=sys.stderr)
    targets = [name for name in requested if name in found]
    return targets, found


def _control_address(socket: Path) -> str:
    return f"unix:{socket}"


def _run_control(args: list[str], timeout: float = 5.0) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        args,
        cwd=ROOT,
        check=False,
        timeout=timeout,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _wait_for_control(target: str, socket: Path, deadline: float) -> None:
    while time.monotonic() < deadline:
        if target == "kitty":
            if _run_control(["kitten", "@", "--to", _control_address(socket), "ls"]).returncode == 0:
                return
        elif target == "alacritty" and socket.exists():
            if _run_control(["alacritty", "msg", "-s", str(socket), "get-config"]).returncode == 0:
                return
        time.sleep(0.05)
    raise RuntimeError(f"{target} control socket did not become ready: {socket}")


def _enforce_size(target: str, socket: Path, cols: int, rows: int) -> None:
    if target == "kitty":
        args = [
            "kitten", "@", "--to", _control_address(socket),
            "resize-os-window", "--match", "all", "--unit", "cells",
            "--width", str(cols), "--height", str(rows),
        ]
    elif target == "alacritty":
        args = [
            "alacritty", "msg", "-s", str(socket), "config", "-w", "-1",
            f"window.dimensions.columns={cols}",
            f"window.dimensions.lines={rows}",
        ]
    else:
        return
    for _ in range(20):
        if _run_control(args).returncode == 0:
            time.sleep(0.05)
            return
        time.sleep(0.05)
    print(f"warning: failed to force {target} geometry to {cols}x{rows}", file=sys.stderr)


def _pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError):
        return False
    return True


def _kill_kitty_by_socket(socket: Path) -> None:
    needle = _control_address(socket)
    try:
        output = subprocess.run(
            ["ps", "-eo", "pid=,command="],
            check=False,
            capture_output=True,
            text=True,
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
        if needle not in command or "--listen-on" not in command or executable != "kitty":
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
    if not pids:
        return
    time.sleep(0.3)
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


def _terminal_command(
    target: str,
    binary: str,
    inner: list[str],
    cols: int,
    rows: int,
    config: Path | None,
    socket: Path | None,
) -> list[str]:
    command: list[str] = [binary]
    if target == "gritty":
        command += ["--cols", str(cols), "--rows", str(rows)]
        if config is not None:
            command += ["--config", str(config)]
        command += ["--", *inner]
    elif target == "alacritty":
        if socket is None:
            raise RuntimeError("alacritty control socket is required")
        command += ["--socket", str(socket)]
        if config is not None:
            command += ["--config-file", str(config)]
        command += [
            "-o", f"window.dimensions.columns={cols}",
            "-o", f"window.dimensions.lines={rows}",
            "-e", *inner,
        ]
    elif target == "kitty":
        if socket is None:
            raise RuntimeError("kitty control socket is required")
        command += [
            "--detach", "--single-instance=no",
            "--listen-on", f"unix:{socket}",
            "-c", str(config) if config is not None else "NONE",
            "-o", "allow_remote_control=socket-only",
            "-o", "close_on_child_death=yes",
            "-o", "remember_window_size=no",
            "-o", f"initial_window_width={cols}c",
            "-o", f"initial_window_height={rows}c",
            *inner,
        ]
    elif target == "ghostty":
        if config is not None:
            command += [f"--config-file={config}"]
        command += [f"--window-width={cols}", f"--window-height={rows}", "-e", *inner]
    elif target == "foot":
        if config is not None:
            command += [f"--config={config}"]
        command += ["-w", f"{cols}x{rows}", *inner]
    return command


def parse_vtebench_dat(path: Path) -> dict[str, list[float]]:
    rows = [line.split() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len(rows) < 2 or not rows[0]:
        raise RuntimeError(f"Empty or malformed vtebench DAT file: {path}")
    names = [name.strip() for name in rows[0]]
    if len(set(names)) != len(names):
        raise RuntimeError(f"Duplicate columns in vtebench DAT file: {path}")
    samples: dict[str, list[float]] = {name: [] for name in names}
    for row in rows[1:]:
        if len(row) != len(names):
            raise RuntimeError(f"Ragged row in vtebench DAT file: {path}")
        for name, raw in zip(names, row):
            token = raw.strip()
            if token in ("", "_"):
                continue
            val = float(token)
            if not math.isfinite(val) or val <= 0:
                raise RuntimeError(f"Invalid sample '{token}' for {name} in {path}")
            samples[name].append(val)
    return samples


def summarize_series(values: list[float]) -> dict:
    if not values:
        raise RuntimeError("No valid samples collected for benchmark")
    ordered = sorted(values)
    p95 = ordered[math.ceil(len(ordered) * 0.95) - 1]
    return {
        "count": len(values),
        "median_ms": statistics.median(values),
        "mean_ms": statistics.fmean(values),
        "stdev_ms": statistics.stdev(values) if len(values) > 1 else 0.0,
        "p95_ms": p95,
        "min_ms": ordered[0],
        "max_ms": ordered[-1],
        "samples_ms": values,
    }


def summarize_dat(samples_by_case: dict[str, list[float]]) -> dict[str, dict]:
    return {case: summarize_series(vals) for case, vals in samples_by_case.items() if vals}


def run_once(
    target: str,
    binary: str,
    vtebench_bin: str,
    subset_dir: Path | None,
    args: argparse.Namespace,
    save_dat_path: Path | None = None,
) -> tuple[dict[str, list[float]], float]:
    with tempfile.TemporaryDirectory(prefix="gritty-vtebench-") as directory:
        work = Path(directory)
        dat_path = work / "vtebench.dat"
        done_path = work / "done"
        gate = work / "go" if target in ("alacritty", "kitty") else None
        socket = work / "ipc.sock" if target in ("alacritty", "kitty") else None
        empty_cfg = work / "empty.toml"
        empty_cfg.write_text("", encoding="utf-8")

        vte_cmd = [
            vtebench_bin,
            "--silent",
            "--warmup", str(args.warmup),
            "--min-bytes", str(args.min_bytes),
            "--max-samples", str(args.max_samples),
            "--max-secs", str(args.max_secs),
        ]
        if subset_dir is not None:
            vte_cmd += ["--benchmarks", str(subset_dir)]
        vte_cmd += ["--dat", str(dat_path)]

        shell_cmd = " ".join(shlex.quote(part) for part in vte_cmd)
        shell_cmd = f"{shell_cmd}; status=$?; echo $status > {shlex.quote(str(done_path))}; exit $status"
        if gate is not None:
            shell_cmd = f"while [ ! -f {shlex.quote(str(gate))} ]; do sleep 0.01; done; {shell_cmd}"
        inner = ["/bin/sh", "-c", shell_cmd]

        target_config: Path | None = None
        if not args.use_user_config:
            if target == "gritty" and args.config is not None:
                target_config = args.config.resolve()
            elif target != "kitty":
                target_config = empty_cfg

        command = _terminal_command(
            target, binary, inner, args.cols, args.rows,
            target_config,
            socket,
        )
        stderr_path = work / "terminal.stderr"
        started = time.monotonic()
        deadline = started + args.timeout
        with stderr_path.open("w", encoding="utf-8") as stderr_file:
            process = subprocess.Popen(
                command,
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=stderr_file,
                text=True,
            )
        try:
            if gate is not None and socket is not None:
                _wait_for_control(target, socket, deadline)
                _enforce_size(target, socket, args.cols, args.rows)
                gate.write_text("go\n")
            process.wait(timeout=max(0.1, deadline - time.monotonic()))
            if target == "kitty":
                while not done_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
            elapsed = time.monotonic() - started
            if not done_path.exists():
                stderr = stderr_path.read_text(encoding="utf-8", errors="replace").strip() if stderr_path.exists() else ""
                raise RuntimeError(
                    f"{target} exited before finishing vtebench ({elapsed:.1f}s, exit={process.returncode}): {stderr}"
                )
            rc = int(done_path.read_text(encoding="utf-8").strip() or "1")
            if rc != 0 or not dat_path.exists():
                stderr = stderr_path.read_text(encoding="utf-8", errors="replace").strip() if stderr_path.exists() else ""
                raise RuntimeError(f"vtebench failed inside {target} (exit={rc}): {stderr}")
            if save_dat_path is not None:
                save_dat_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dat_path, save_dat_path)
            return parse_vtebench_dat(dat_path), elapsed
        finally:
            _cleanup_control(target, socket)
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2.0)


def _prepare_subset_dir(corpus: Path, cases: list[str], all_cases: list[str], parent: Path) -> Path | None:
    if set(cases) == set(all_cases):
        return None
    subset = parent / "benchmarks"
    subset.mkdir(parents=True, exist_ok=True)
    for case in cases:
        (subset / case).symlink_to(corpus / case)
    return subset


def print_cross_table(results: dict[str, dict[str, dict]], targets: list[str], cases: list[str]) -> None:
    baseline_target = "gritty" if "gritty" in targets else targets[0]
    print(f"\n### alacritty/vtebench ({len(cases)} workloads)")
    print(f"| workload | terminal | n | median ms | p95 ms | mean ± stdev ms | vs {baseline_target} |")
    print("|---|---|---:|---:|---:|---:|---:|")
    for case in cases:
        base_med = results.get(baseline_target, {}).get(case, {}).get("median_ms")
        for target in targets:
            entry = results.get(target, {}).get(case)
            if not entry:
                continue
            ratio = (
                f"{base_med / entry['median_ms']:.2f}x"
                if (base_med and entry["median_ms"] and target != baseline_target)
                else ("1.00x" if target == baseline_target else "-")
            )
            print(
                f"| {case} | {target} | {entry['count']} | "
                f"{entry['median_ms']:.3f} | {entry['p95_ms']:.3f} | "
                f"{entry['mean_ms']:.3f} ± {entry['stdev_ms']:.3f} | {ratio} |"
            )


def print_ab_table(data: dict, cases: list[str]) -> None:
    bins = data["bins"]
    print(f"\n### alacritty/vtebench ABBA A/B comparison (A = {bins['A']}, B = {bins['B']})")
    print("| workload | binary | n | median ms | p95 ms | mean ± stdev ms | delta vs A |")
    print("|---|---|---:|---:|---:|---:|---:|")
    for case in cases:
        entry = data["cases"].get(case)
        if not entry:
            continue
        a = entry["A"]
        b = entry["B"]
        speedup = (a["median_ms"] / b["median_ms"]) if b["median_ms"] > 0 else 0.0
        pct = (b["median_ms"] / a["median_ms"] - 1.0) if a["median_ms"] > 0 else 0.0
        print(
            f"| {case} | A | {a['count']} | {a['median_ms']:.3f} | {a['p95_ms']:.3f} | "
            f"{a['mean_ms']:.3f} ± {a['stdev_ms']:.3f} | - |"
        )
        print(
            f"| {case} | B | {b['count']} | {b['median_ms']:.3f} | {b['p95_ms']:.3f} | "
            f"{b['mean_ms']:.3f} ± {b['stdev_ms']:.3f} | {pct:+.1%} ({speedup:.2f}x) |"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targets", default="gritty", help=f"Comma-separated terminals ({','.join(SUPPORTED_TARGETS)})")
    parser.add_argument("--gritty-bin", help="Explicit path to gritty binary")
    parser.add_argument("--ab-bins", nargs=2, metavar=("BIN_A", "BIN_B"), help="Compare two gritty binaries in ABBA order")
    parser.add_argument("--ab-rounds", type=int, default=1, help="Number of ABBA cycles (each cycle runs A, B, B, A)")
    parser.add_argument("--cases", default="all", help="Comma-separated vtebench workload names, or 'all'")
    parser.add_argument("--list", action="store_true", help="List available upstream vtebench workloads and exit")
    parser.add_argument("--cols", type=int, default=160)
    parser.add_argument("--rows", type=int, default=48)
    parser.add_argument("--config", type=Path, help="Optional gritty config file")
    parser.add_argument("--use-user-config", action="store_true", help="Use each terminal's ~/.config instead of an empty default config")
    parser.add_argument("--warmup", type=int, default=2, help="vtebench --warmup iterations")
    parser.add_argument("--min-bytes", type=int, default=16_777_216, help="vtebench --min-bytes per sample")
    parser.add_argument("--max-samples", type=int, default=20, help="vtebench --max-samples per workload")
    parser.add_argument("--max-secs", type=int, default=10, help="vtebench --max-secs per workload")
    parser.add_argument("--timeout", type=float, default=600.0, help="Per-invocation wall timeout in seconds")
    parser.add_argument("--save", type=Path, help="Write structured JSON results")
    parser.add_argument("--save-dat", type=Path, help="Directory to store raw upstream .dat files")
    parser.add_argument("--no-nix", action="store_true", help="Do not auto-launch nix develop .#throughput")
    args = parser.parse_args()

    if args.warmup < 0 or args.min_bytes < 1 or args.max_samples < 1 or args.max_secs < 1 or args.ab_rounds < 1:
        raise SystemExit("Numeric parameters must be positive (--warmup >= 0)")

    corpus = ensure_vtebench_env(args.no_nix)
    vtebench_bin = shutil.which("vtebench")
    assert vtebench_bin
    all_cases = discover_cases(corpus)
    if not all_cases:
        raise SystemExit(f"No vtebench workloads found under {corpus}")

    if args.list:
        for name in all_cases:
            print(name)
        return 0

    if args.cases.strip().lower() == "all":
        cases = all_cases
    else:
        cases = [name.strip() for name in args.cases.split(",") if name.strip()]
        unknown = [name for name in cases if name not in all_cases]
        if unknown:
            raise SystemExit(
                f"Unknown vtebench cases: {', '.join(unknown)}. Available: {', '.join(all_cases)}"
            )
    if not cases:
        raise SystemExit("No benchmark cases selected")

    vte_version = subprocess.check_output([vtebench_bin, "--version"], text=True).strip()

    with tempfile.TemporaryDirectory(prefix="gritty-vtebench-corpus-") as subset_root:
        subset_dir = _prepare_subset_dir(corpus, cases, all_cases, Path(subset_root))

        if args.ab_bins:
            bin_a, bin_b = args.ab_bins
            for binary in (bin_a, bin_b):
                if not Path(binary).is_file():
                    raise SystemExit(f"Binary not found: {binary}")
            merged: dict[str, dict[str, list[float]]] = {"A": {c: [] for c in cases}, "B": {c: [] for c in cases}}
            for round_idx in range(args.ab_rounds):
                for step_idx, label in enumerate(ABBA_ORDER):
                    binary = bin_a if label == "A" else bin_b
                    print(f"[ABBA r{round_idx + 1}/{args.ab_rounds} {label}] running vtebench ({len(cases)} cases)", file=sys.stderr, flush=True)
                    dat_out = (args.save_dat / f"abba-r{round_idx + 1}-{step_idx}-{label}.dat") if args.save_dat else None
                    samples, _ = run_once("gritty", binary, vtebench_bin, subset_dir, args, dat_out)
                    for case in cases:
                        merged[label][case].extend(samples.get(case, []))
            config_mode = "user" if args.use_user_config else ("explicit" if args.config else "empty")
            data = {
                "mode": "abba",
                "config_mode": config_mode,
                "vtebench": vte_version,
                "geometry": {"columns": args.cols, "lines": args.rows},
                "params": {
                    "warmup": args.warmup,
                    "min_bytes": args.min_bytes,
                    "max_samples": args.max_samples,
                    "max_secs": args.max_secs,
                    "ab_rounds": args.ab_rounds,
                },
                "bins": {"A": str(Path(bin_a).resolve()), "B": str(Path(bin_b).resolve())},
                "cases": {
                    case: {
                        "A": summarize_series(merged["A"][case]),
                        "B": summarize_series(merged["B"][case]),
                    }
                    for case in cases
                    if merged["A"][case] and merged["B"][case]
                },
            }
            print_ab_table(data, cases)
            if args.save:
                args.save.parent.mkdir(parents=True, exist_ok=True)
                args.save.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
                print(f"\nSaved: {args.save}")
            return 0

        gritty = find_gritty(args.gritty_bin)
        requested = [name.strip() for name in args.targets.split(",") if name.strip()]
        targets, binaries = resolve_binaries(gritty, requested)
        if not targets:
            raise SystemExit("No requested terminals are available")

        results: dict[str, dict[str, dict]] = {}
        wall_seconds: dict[str, float] = {}
        for target in targets:
            print(f"[{target}] running vtebench ({len(cases)} cases, warmup={args.warmup}, max_samples={args.max_samples})", file=sys.stderr, flush=True)
            dat_out = (args.save_dat / f"{target}.dat") if args.save_dat else None
            samples, elapsed = run_once(target, binaries[target], vtebench_bin, subset_dir, args, dat_out)
            results[target] = summarize_dat(samples)
            wall_seconds[target] = elapsed

        print_cross_table(results, targets, cases)
        if args.save:
            config_mode = "user" if args.use_user_config else ("explicit" if args.config else "empty")
            payload = {
                "mode": "cross",
                "config_mode": config_mode,
                "vtebench": vte_version,
                "geometry": {"columns": args.cols, "lines": args.rows},
                "params": {
                    "warmup": args.warmup,
                    "min_bytes": args.min_bytes,
                    "max_samples": args.max_samples,
                    "max_secs": args.max_secs,
                },
                "targets": targets,
                "wall_seconds": wall_seconds,
                "results": results,
            }
            args.save.parent.mkdir(parents=True, exist_ok=True)
            args.save.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
            print(f"\nSaved: {args.save}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
