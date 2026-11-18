"""Read upstream vtebench DAT / Typometer CSV, not a new benchmark runner."""
import csv
import json
import math
import statistics
from pathlib import Path

# Baselines are host/display/configuration specific, never cross-platform ratios.
PROFILE_KEYS = (
    "machine", "platform", "backend", "display", "geometry", "font", "build_type",
    "vtebench_version", "vtebench_bytes", "vtebench_warmup",
    "typometer_version", "typometer_mode",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def read_samples(path, kind):
    with Path(path).open(encoding="utf-8", newline="") as source:
        rows = list(csv.reader(source)) if kind == "latency" else [line.split() for line in source if line.strip()]
    require(len(rows) > 1 and rows[0], f"empty {kind} data: {path}")
    names = [name.strip() for name in rows[0]]
    require(all(names) and len(set(names)) == len(names), f"invalid/duplicate columns: {path}")
    result = {name: [] for name in names}
    for row in rows[1:]:
        require(len(row) == len(names), f"ragged result row: {path}")
        for name, value in zip(names, row):
            if value.strip() in ("", "_"):
                continue  # Upstream's missing-sample markers, not zero samples.
            number = float(value)
            require(math.isfinite(number) and number > 0, f"nonpositive/nonfinite sample: {path}: {name}")
            result[name].append(number)
    minimum = 100 if kind == "latency" else 10
    require(all(len(values) >= minimum for values in result.values()), f"need >= {minimum} samples per column: {path}")
    if kind == "latency":
        require(set(names) == {"idle", "load"}, "Typometer CSV must contain exactly idle and load columns (milliseconds)")
    else:
        require({"scrolling", "dense_cells"} <= set(names), "vtebench is missing scrolling/SGR workloads")
    return result


def percentile95(values):
    return sorted(values)[math.ceil(len(values) * 0.95) - 1]


def compare(current_dir, baseline_dir, max_regression=0.10):
    require(math.isfinite(max_regression) and 0 <= max_regression <= 1, "invalid regression allowance")
    current_dir, baseline_dir = Path(current_dir), Path(baseline_dir)
    require(current_dir.resolve() != baseline_dir.resolve(), "candidate cannot be its own baseline")
    current = read_json(current_dir / "profile.json")
    baseline = read_json(baseline_dir / "profile.json")
    for key in PROFILE_KEYS:
        value = current.get(key)
        require(value is not None and value != "" and not str(value).startswith("REPLACE"), f"missing profile field: {key}")
        if key not in ("vtebench_bytes", "vtebench_warmup"):
            require(isinstance(value, str), f"profile field must be a string: {key}")
        require(value == baseline.get(key), f"incomparable benchmark profiles: {key}")
    require(current["build_type"] == "release", "performance evidence must use a release build")
    require(current["platform"] in ("linux", "macos"), "unsupported measurement platform")
    require(current["backend"] == ("wayland" if current["platform"] == "linux" else "cocoa"), "not the native backend under test")
    require(int(current["vtebench_bytes"]) >= 16777216 and int(current["vtebench_warmup"]) >= 2, "insufficient vtebench payload/warmup")
    require(current["typometer_mode"] == "awt-sync", "use the same documented Typometer sync/Robot mode")
    for filename, kind in (("vtebench.dat", "throughput"), ("latency.csv", "latency")):
        measured = read_samples(current_dir / filename, kind)
        reference = read_samples(baseline_dir / filename, kind)
        require(measured.keys() == reference.keys(), f"benchmark case set changed: {filename}")
        for name, values in measured.items():
            for label, metric in (("median", statistics.median), ("p95", percentile95)):
                actual, expected = metric(values), metric(reference[name])
                ratio = actual / expected  # Both upstream formats store durations: lower is better.
                print(f"{kind}/{name}/{label}: {actual:.3f} ms, baseline {expected:.3f} ms ({ratio:.3f}x)")
                require(ratio <= 1 + max_regression, f"regression exceeds {max_regression:.0%}: {kind}/{name}/{label}")
