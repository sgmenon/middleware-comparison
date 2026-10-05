#!/usr/bin/env python3
"""Render vsomeip/results-snapshot.md from CSV via Jinja."""

from __future__ import annotations

import argparse
import csv
import io
import math
import sys
from pathlib import Path

# Same ladder as notes/benchmarks.md ReliablePingPong table.
CANONICAL_SIZES = (64, 1024, 16384, 65536, 262144, 1048576, 4194304, 10485760)


def _y_max(values: list[float], *, floor: float = 1.0) -> int:
    if not values:
        return int(floor)
    return int(math.ceil(max(values) * 1.15 + 1))


def size_label(num_bytes: int) -> str:
    if num_bytes >= 1_048_576 and num_bytes % 1_048_576 == 0:
        return f"{num_bytes // 1_048_576}MiB"
    if num_bytes >= 1024 and num_bytes % 1024 == 0:
        return f"{num_bytes // 1024}KiB"
    return f"{num_bytes}B"


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        return list(reader)


def filter_run_label(rows: list[dict[str, str]], run_label: str | None) -> list[dict[str, str]]:
    if not run_label:
        return rows
    return [r for r in rows if r.get("run_label") == run_label]


def latest_per_config(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Keep newest row per (stack, size, rate_hz) by recorded_utc."""
    best: dict[tuple[str, str, str], dict[str, str]] = {}
    for r in rows:
        key = (r.get("stack", ""), r.get("size", ""), r.get("rate_hz", ""))
        prev = best.get(key)
        if prev is None or (r.get("recorded_utc", "") >= prev.get("recorded_utc", "")):
            best[key] = r
    out = list(best.values())
    out.sort(key=lambda r: (r.get("stack", ""), int(r.get("size", "0") or 0)))
    return out


def _row_ok(r: dict[str, str]) -> bool:
    mean = r.get("mean_us", "")
    n = r.get("n", "0")
    return mean not in ("", "NA") and n not in ("", "0")


def series_for_stack(rows: list[dict[str, str]], stack: str) -> tuple[list[str], list[float], list[float]]:
    by_size: dict[int, dict[str, str]] = {}
    for r in rows:
        if r.get("stack") != stack:
            continue
        try:
            size = int(r["size"])
        except (KeyError, ValueError):
            continue
        if _row_ok(r):
            by_size[size] = r

    labels: list[str] = []
    means: list[float] = []
    p99s: list[float] = []
    for size in CANONICAL_SIZES:
        r = by_size.get(size)
        if not r:
            continue
        labels.append(size_label(size))
        means.append(float(r["mean_us"]))
        p99s.append(float(r["p99_us"]))
    return labels, means, p99s


def comparison_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Pair covesa/sgmenon results by size and rate for the summary table."""
    by_config: dict[tuple[int, str], dict[str, dict[str, str]]] = {}
    for row in rows:
        try:
            size = int(row["size"])
        except (KeyError, ValueError):
            continue
        stack = row.get("stack", "")
        if stack not in ("covesa", "sgmenon"):
            continue
        by_config.setdefault((size, row.get("rate_hz", "")), {})[stack] = row

    out: list[dict[str, str]] = []
    for (size, rate), pair in sorted(by_config.items()):
        cov = pair.get("covesa")
        sgm = pair.get("sgmenon")
        cov_mean = cov.get("mean_us", "NA") if cov else "NA"
        sgm_mean = sgm.get("mean_us", "NA") if sgm else "NA"
        improvement = "NA"
        try:
            cov_value = float(cov_mean)
            sgm_value = float(sgm_mean)
            improvement = f"{(cov_value - sgm_value) / cov_value * 100.0:.1f}%"
        except (ValueError, ZeroDivisionError):
            pass

        cov_n = cov.get("n", "NA") if cov else "NA"
        sgm_n = sgm.get("n", "NA") if sgm else "NA"
        out.append(
            {
                "size": str(size),
                "size_label": size_label(size),
                "rate_hz": rate,
                "n": cov_n if cov_n == sgm_n else f"{cov_n} / {sgm_n}",
                "cov_mean_us": cov_mean,
                "sgm_mean_us": sgm_mean,
                "cov_p99_us": cov.get("p99_us", "NA") if cov else "NA",
                "sgm_p99_us": sgm.get("p99_us", "NA") if sgm else "NA",
                "sgm_improvement": improvement,
            }
        )
    return out


def make_dataset(csv_path: Path, run_label: str, transport: str) -> dict[str, object]:
    all_rows = load_rows(csv_path) if csv_path.exists() else []
    rows = filter_run_label(all_rows, run_label) if run_label else latest_per_config(all_rows)

    csv_raw = ""
    if rows:
        buf = io.StringIO()
        writer = csv.DictWriter(buf, fieldnames=list(rows[0].keys()), extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
        csv_raw = buf.getvalue()

    cov_labels, cov_means, _ = series_for_stack(rows, "covesa")
    sgm_labels, sgm_means, _ = series_for_stack(rows, "sgmenon")
    size_labels = [
        size_label(size)
        for size in CANONICAL_SIZES
        if size_label(size) in cov_labels or size_label(size) in sgm_labels
    ]

    def align(labels: list[str], values: list[float]) -> list[float | None]:
        values_by_label = dict(zip(labels, values, strict=False))
        return [values_by_label.get(label) for label in size_labels]

    def fmt_line(values: list[float | None]) -> str:
        return ", ".join("0" if value is None else f"{value:.3f}" for value in values)

    cov_line = align(cov_labels, cov_means)
    sgm_line = align(sgm_labels, sgm_means)
    all_numeric = [value for value in cov_line + sgm_line if value is not None]
    return {
        "transport": transport,
        "run_label": run_label or "(latest per stack/size/rate)",
        "history_csv": str(csv_path),
        "comparisons": comparison_rows(rows),
        "size_labels": size_labels,
        "covesa_line": fmt_line(cov_line),
        "sgmenon_line": fmt_line(sgm_line),
        "chart_y_max": _y_max(all_numeric),
        "csv_raw": csv_raw,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--udp-csv", type=Path)
    parser.add_argument("--tcp-csv", type=Path)
    # Compatibility with a run_snapshot.sh process started before the dual-CSV renderer update.
    parser.add_argument("--csv", type=Path)
    parser.add_argument("--transport", choices=("udp", "tcp"))
    parser.add_argument("--run-label", default="")
    parser.add_argument("--history-csv", type=Path)
    parser.add_argument("--count", type=int)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--git-sha", required=True)
    parser.add_argument("--date-utc", required=True)
    parser.add_argument("--warmup", type=int, required=True)
    parser.add_argument("--udp-run-label", default="")
    parser.add_argument("--tcp-run-label", default="")
    args = parser.parse_args()

    if args.csv:
        csv_dir = args.csv.parent
        if args.transport == "tcp":
            args.tcp_csv = args.csv
            args.tcp_run_label = args.run_label
            args.udp_csv = args.udp_csv or csv_dir / "snapshot.csv"
        else:
            args.udp_csv = args.csv
            args.udp_run_label = args.run_label
            args.tcp_csv = args.tcp_csv or csv_dir / "snapshot-tcp.csv"
    if not args.udp_csv or not args.tcp_csv:
        parser.error("--udp-csv and --tcp-csv are required")

    try:
        from jinja2 import Environment, FileSystemLoader, select_autoescape
    except ImportError:
        print("render_snapshot.py requires jinja2: pip install jinja2", file=sys.stderr)
        return 1

    env = Environment(
        loader=FileSystemLoader(args.template.parent),
        autoescape=select_autoescape(default_for_string=False, default=False),
        trim_blocks=True,
        lstrip_blocks=True,
    )
    template = env.get_template(args.template.name)
    rendered = template.render(
        git_sha=args.git_sha,
        date_utc=args.date_utc,
        warmup=args.warmup,
        datasets=[
            make_dataset(args.tcp_csv, args.tcp_run_label, "tcp"),
            make_dataset(args.udp_csv, args.udp_run_label, "udp"),
        ],
    )
    args.out.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
