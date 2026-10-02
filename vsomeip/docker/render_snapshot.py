#!/usr/bin/env python3
"""Render vsomeip/results-snapshot.md from CSV via Jinja."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path


def _y_max(values: list[float], *, floor: float = 1.0) -> int:
    if not values:
        return int(floor)
    return int(math.ceil(max(values) * 1.15 + 1))


def _bar_value(mean: str) -> float:
    if mean in ("", "NA"):
        return 0.0
    return float(mean)


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        return list(reader)


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

    chart_labels: list[str] = []
    chart_values: list[float] = []
    for r in rows:
        mean = r.get("mean_us", "")
        n = r.get("n", "0")
        if mean in ("", "NA") or n == "0":
            continue
        chart_labels.append(
            f"{r['stack']}/{r['transport']}/{r['size']}@{r['rate_hz']}Hz"
        )
        chart_values.append(float(mean))

    compare_keys = {
        ("covesa", "tcp"): "covesa_tcp",
        ("sgmenon", "tcp"): "sgmenon_tcp",
        ("covesa", "udp"): "covesa_udp",
        ("sgmenon", "udp"): "sgmenon_udp",
    }
    compare_means: dict[str, str] = {v: "NA" for v in compare_keys.values()}
    for r in rows:
        if r.get("size") != "4096" or r.get("rate_hz") != "500":
            continue
        key = (r.get("stack"), r.get("transport"))
        if key in compare_keys:
            compare_means[compare_keys[key]] = r.get("mean_us", "NA")

    compare_numeric = [
        float(v) for v in compare_means.values() if v not in ("", "NA")
    ]

    env = Environment(
        loader=FileSystemLoader(args.template.parent),
        autoescape=select_autoescape(default_for_string=False, default=False),
        trim_blocks=True,
        lstrip_blocks=True,
    )
    template = env.get_template(args.template.name)
    compare_bars = {k: _bar_value(v) for k, v in compare_means.items()}
    rendered = template.render(
        git_sha=args.git_sha,
        date_utc=args.date_utc,
        count=args.count,
        warmup=args.warmup,
        rows=rows,
        chart_labels=chart_labels,
        chart_bar_values=chart_values,
        chart_y_max=_y_max(chart_values),
        compare_y_max=_y_max(compare_numeric),
        compare_bars=compare_bars,
        csv_raw=csv_raw,
    )
    args.out.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
