#!/usr/bin/env python3
"""Thin launchers and fail-closed phase gates for the four acceptance suites."""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path

from perf_gate import compare, read_json, read_samples, require

ROOT = Path(__file__).resolve().parent.parent
VISUAL_CHECKS = ("rgb24", "box_alignment", "sync2026", "images", "kitty_keys")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def executable(name):
    result = shutil.which(str(name))
    if result is None:
        raise ValueError(f"missing executable: {name}; enter the matching Nix shell")
    return str(Path(result).resolve())


def run(command, timeout=300):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, timeout=timeout, cwd=ROOT)


def target(args, payload):
    binary = executable(args.terminal or args.build_dir / "src/gritty")
    prefix = [binary]
    if args.config:
        prefix += ["--config", str(args.config.resolve())]
    return prefix + ["--"] + list(map(str, payload))


def save_run(args, name, artifact=None, command=None):
    record = {
        "terminal_sha256": digest(executable(args.terminal or args.build_dir / "src/gritty")),
        "profile_sha256": digest(args.results / "profile.json"),
        "config_sha256": digest(args.config) if args.config else None,
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "time": datetime.now(UTC).isoformat(),
        "command": command,
    }
    if artifact:
        record["artifact_sha256"] = digest(args.results / artifact)
    (args.results / f"{name}-run.json").write_text(json.dumps(record, indent=2) + "\n")


def fresh_artifact(args, name):
    require(args.config is not None, "measurement needs an explicit --config")
    require((args.results / "profile.json").is_file(), "copy/fill tests/acceptance/profile.example.json first")
    path = args.results / name
    require(not path.exists(), f"refusing to overwrite measurement: {path}; use a fresh results directory")
    return path


def esctest(args, window=False, expected=None):
    # Upstream can exit zero after failed tests. Never treat exit status alone
    # (or a stale/missing/empty summary) as a successful protocol gate.
    log = args.results / ("esctest-window.log" if window else "esctest-headless.log")
    log.unlink(missing_ok=True)
    traces = args.results / ("esctest-window-traces" if window else "esctest-headless-traces")
    traces.mkdir(exist_ok=True)
    payload = [executable("esctest2"), "--expected-terminal=xterm", "--max-vt-level=5",
               "--window-id=0", "--stop-on-failure", "--no-print-logs", "--timeout=1",
               f"--include={args.include}", f"--logfile={log}", f"--test-case-dir={traces}"]
    command = target(args, payload) if window else [executable(args.build_dir / "tests/gritty_protocol_driver")] + payload
    run(command)
    validate_protocol(log, expected)


def validate_protocol(log, expected=None):
    report = log.read_text(encoding="utf-8")
    summaries = re.findall(r"\*\*\* (\d+) tests? passed, (\d+) known bugs?, (\d+) tests? failed \*\*\*", report, re.IGNORECASE)
    require(len(summaries) == 1, f"missing/ambiguous esctest2 summary: {log}")
    passed, known, failed = map(int, summaries[0])
    print(f"esctest2: {passed} passed, {known} upstream-known/skipped, {failed} failed; {log}")
    require(passed > 0 and failed == 0 and not re.search(r"(?m)^Traceback ", report),
            f"protocol gate failed; inspect {log}")
    if expected is not None:
        executed = re.findall(r"(?m)^Run test: (.+)$", report)
        require(known == 0 and passed == len(expected) and len(executed) == len(expected) and set(executed) == set(expected),
                "the fixed protocol baseline must run completely, with zero skips")
    # Full-suite diagnostics retain upstream xterm known-bug reporting. The
    # deliberately small phase baseline permits no skipped or missing cases.


def demo(args):
    path = fresh_artifact(args, "notcurses.json")
    command = target(args, [executable("notcurses-demo"), "-c", "-J", str(path)])
    run(command, timeout=1800)
    validate_demo(path)
    save_run(args, "visual", "notcurses.json", command)
    print("Review actual RGB/box/frame/image output; fill visual-review.json. A fallback image is NOT a protocol pass.")


def validate_demo(path):
    report = read_json(path)["notcurses-demo"]
    runs = report["runs"]
    require({"box", "grid", "uniblock", "view"} <= runs.keys(), "required notcurses demos were skipped/failed")
    require(int(report["rows"]) >= 24 and int(report["cols"]) >= 80, "need >=80x24 demo geometry")
    require(all(int(item["frames"]) > 0 and int(item["ns"]) > 0 for item in runs.values()), "incomplete demo run")


def require_release(args):
    require(read_json(args.results / "profile.json")["build_type"] == "release", "use a release measurement profile")
    binary = executable(args.terminal or args.build_dir / "src/gritty")
    if Path(binary).name == "gritty":
        options = {item["name"]: item["value"] for item in read_json(args.build_dir / "meson-info/intro-buildoptions.json")}
        require(options["buildtype"] == "release" and options["b_sanitize"] in ("none", ["none"], []),
                "measure an unsanitized Meson release build")


def throughput(args):
    require_release(args)
    path = fresh_artifact(args, "vtebench.dat")
    profile = read_json(args.results / "profile.json")
    corpus_name = os.environ.get("GRITTY_VTEBENCH_BENCHMARKS")
    if not corpus_name:
        raise ValueError("missing vtebench corpus; use nix develop .#throughput")
    corpus = Path(corpus_name)
    require(corpus.is_dir(), "missing vtebench corpus directory")
    size, warmup = int(profile["vtebench_bytes"]), int(profile["vtebench_warmup"])
    require(size >= 16777216 and warmup >= 2, "need >=16 MiB samples and >=2 warmups")
    tool = executable("vtebench")
    version = subprocess.check_output([tool, "--version"], text=True).split()[-1]
    require(version == profile["vtebench_version"], "vtebench version does not match the profile")
    # The Nix runner already supplies its patched corpus; do not add another one.
    command = target(args, [tool, "--warmup", str(warmup),
                            "--min-bytes", str(size), "--max-samples", "20", "--max-secs", "10", "--dat", str(path)])
    run(command, timeout=600)
    samples = read_samples(path, "throughput")
    expected = {item.name for item in corpus.iterdir() if item.is_dir()}
    require(samples.keys() == expected, "vtebench skipped a workload from the pinned corpus")
    save_run(args, "throughput", "vtebench.dat", command)


def latency(args):
    require_release(args)
    path = fresh_artifact(args, "latency.csv")
    print(f"Typometer: measure idle AND load in the target terminal, export columns idle,load to {path}.")
    print("Use synchronous measurement / AWT Robot. Do not grant OS permissions automatically.")
    require(read_json(args.results / "profile.json")["typometer_version"] == "1.0.1", "use the pinned Typometer release")
    command = [executable("typometer")]
    run(command, timeout=7200)
    read_samples(path, "latency")
    save_run(args, "latency", "latency.csv", command)


def keys(args):
    require(args.config is not None, "use an explicit --config")
    require((args.results / "profile.json").is_file(), "profile.json is required")
    command = target(args, [executable("kitten"), "show-key", "-m", "kitty"])
    run(command, timeout=600)
    save_run(args, "keyboard", command=command)


def verify_run(directory, name, artifact=None, binary=None, config=None):
    record = read_json(directory / f"{name}-run.json")
    require(record["profile_sha256"] == digest(directory / "profile.json"), f"profile changed after {name} measurement")
    if artifact:
        require(record["artifact_sha256"] == digest(directory / artifact), f"{name} artifact changed after measurement")
    if binary:
        require(record["terminal_sha256"] == digest(binary), f"{name} evidence is not for the current terminal binary")
    if config:
        require(record["config_sha256"] == digest(config), f"{name} evidence used a different configuration")
    return record


def late_gate(args):
    require(args.baseline is not None, "phase 3/4 needs --baseline with real same-profile measurements")
    require(args.config is not None, "window/performance gates need an explicit --config")
    native_platform = {"darwin": "macos", "linux": "linux"}.get(sys.platform)
    require(read_json(args.results / "profile.json")["platform"] == native_platform, "measurement profile is for another OS")
    require_release(args)
    binary = executable(args.terminal or args.build_dir / "src/gritty")
    visual = verify_run(args.results, "visual", "notcurses.json", binary, args.config)
    validate_demo(args.results / "notcurses.json")
    verify_run(args.results, "keyboard", binary=binary, config=args.config)
    review = read_json(args.results / "visual-review.json")
    require(isinstance(review.get("reviewer"), str) and review["reviewer"].strip(), "visual review needs a named reviewer")
    require(review.get("run_sha256") == visual["artifact_sha256"], "visual review is not attached to this demo run")
    require(all(review.get(key) is True for key in VISUAL_CHECKS), "visual/Kitty acceptance is missing or failed")
    for directory in (args.results, args.baseline):
        for name, artifact in (("throughput", "vtebench.dat"), ("latency", "latency.csv")):
            verify_run(directory, name, artifact, binary if directory == args.results else None,
                       args.config if directory == args.results else None)
    compare(args.results, args.baseline, args.max_regression)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["phase1", "phase2", "phase3", "phase4", "esctest2", "demo", "vtebench", "typometer", "keys", "check"])
    parser.add_argument("--build-dir", type=Path, default=Path("build/release"))
    parser.add_argument("--terminal", help="gritty by default; foot is also accepted for Linux reference measurements")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--results", type=Path, default=Path("artifacts/acceptance/current"))
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--include", default=".*", help="esctest2 diagnostic selection only; phase gates use the fixed checked-in profile")
    parser.add_argument("--window", action="store_true", help="esctest2 in the real window instead of the headless driver")
    parser.add_argument("--max-regression", type=float, default=0.10, help="reviewed regression allowance, not a fabricated baseline")
    args = parser.parse_args()
    require(os.environ.get("IN_NIX_SHELL"), "run inside the matching Nix devShell")
    args.build_dir = args.build_dir.resolve()
    if args.config:
        args.config = args.config.resolve()
    args.results = args.results.resolve()
    args.results.mkdir(parents=True, exist_ok=True)
    if args.baseline:
        args.baseline = args.baseline.resolve()
    if args.action.startswith("phase"):
        require(args.terminal is None, "phase gates must target the current gritty build, not a reference terminal")
        if args.action in ("phase3", "phase4"):
            require(sys.platform == ("linux" if args.action == "phase3" else "darwin"), "phase3 is Linux; phase4 is macOS")
        require(args.include == ".*", "phase gates do not allow overriding the fixed protocol profile")
        expected: list[str] = read_json(ROOT / "tests/acceptance/esctest2-profile.json")["tests"]
        require(isinstance(expected, list) and all(isinstance(name, str) for name in expected), "invalid protocol baseline")
        require(expected and len(set(expected)) == len(expected), "empty/duplicate protocol baseline cases")
        args.include = "^(?:" + "|".join(re.escape(name) for name in expected) + ")$"
        run(["meson", "compile", "-C", args.build_dir])
        early = args.action in ("phase1", "phase2")
        command = ["meson", "test", "-C", args.build_dir, "--print-errorlogs"]
        run(command + (["--suite", "core"] if early else []))
        esctest(args, window=not early, expected=expected)
        if not early:
            late_gate(args)
        print(args.action + " gate passed")
    elif args.action == "esctest2":
        esctest(args, args.window)
    elif args.action == "check":
        late_gate(args)
    else:
        {"demo": demo, "vtebench": throughput, "typometer": latency, "keys": keys}[args.action](args)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"BLOCKED/FAILED: {error}", file=sys.stderr)
        sys.exit(1)
