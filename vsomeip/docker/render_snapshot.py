#!/usr/bin/env python3
"""Render vsomeip/results-snapshot.md from CSV via Jinja."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

# Same ladder as notes/benchmarks.md ReliablePingPong table.
CANONICAL_SIZES = (64, 1024, 16384, 65536, 262144, 1048576, 4194304)


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--git-sha", required=True)
    parser.add_argument("--date-utc", required=True)
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--warmup", type=int, required=True)
    args = parser.parse_args()

    try:
        from jinja2 import Environment, FileSystemLoader, select_autoescape
    except ImportError:
        print("render_snapshot.py requires jinja2: pip install jinja2", file=sys.stderr)
        return 1

    rows = load_rows(args.csv)
    csv_raw = args.csv.read_text(encoding="utf-8").rstrip() + "\n"

    cov_labels, cov_means, cov_p99 = series_for_stack(rows, "covesa")
    sgm_labels, sgm_means, sgm_p99 = series_for_stack(rows, "sgmenon")

    # Shared x-axis: union of sizes present in either stack (canonical order).
    size_labels: list[str] = []
    for size in CANONICAL_SIZES:
        lab = size_label(size)
        if lab in cov_labels or lab in sgm_labels:
            size_labels.append(lab)

    def align(labels: list[str], values: list[float]) -> list[float | None]:
        m = dict(zip(labels, values, strict=False))
        return [m.get(lab) for lab in size_labels]

    cov_line = align(cov_labels, cov_means)
    sgm_line = align(sgm_labels, sgm_means)
    all_numeric = [v for v in cov_line + sgm_line if v is not None]

    def fmt_line(vals: list[float | None]) -> str:
        parts: list[str] = []
        for v in vals:
            if v is None:
                parts.append("0")
            else:
                parts.append(f"{v:.3f}")
        return ", ".join(parts)

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
        count=args.count,
        warmup=args.warmup,
        rows=rows,
        size_labels=size_labels,
        covesa_line=fmt_line(cov_line),
        sgmenon_line=fmt_line(sgm_line),
        chart_y_max=_y_max(all_numeric),
        csv_raw=csv_raw,
    )
    args.out.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
