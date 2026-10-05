#!/usr/bin/env python3
"""Render and validate the shared vsomeip benchmark configuration."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from jinja2 import Environment, FileSystemLoader, StrictUndefined


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--unicast", required=True)
    parser.add_argument("--routing", required=True)
    parser.add_argument("--crc-offset", choices=("0", "64"), required=True)
    parser.add_argument("--transport", choices=("udp", "tcp"), required=True)
    args = parser.parse_args()

    env = Environment(
        loader=FileSystemLoader(args.template.parent),
        undefined=StrictUndefined,
        autoescape=False,
        keep_trailing_newline=True,
    )
    rendered = env.get_template(args.template.name).render(
        unicast=args.unicast,
        routing=args.routing,
        crc_offset=args.crc_offset,
        transport=args.transport,
    )
    json.loads(rendered)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(rendered, encoding="utf-8")


if __name__ == "__main__":
    main()
