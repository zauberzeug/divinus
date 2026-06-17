#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Generate a single self-contained interactive API page from the spec.

Reads ``doc/openapi.yaml`` and embeds it into ``doc/api.html`` next to the
Scalar API reference loaded from a CDN, so the result opens by double-clicking -
no local web server and no Node toolchain required.

Invoked through ``pixi run generate-api-docs``. The generated ``doc/api.html`` is
git-ignored (regenerate on demand); ``doc/openapi.yaml`` stays the single
source of truth.
"""
from __future__ import annotations

import json
from pathlib import Path

import yaml

DOC = Path(__file__).resolve().parent.parent / 'doc'
SPEC = DOC / 'openapi.yaml'
OUT = DOC / 'api.html'

TEMPLATE = """<!doctype html>
<html>
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{title}</title></head>
<body>
<script id="api-reference" type="application/json">{spec}</script>
<script src="https://cdn.jsdelivr.net/npm/@scalar/api-reference"></script>
</body>
</html>
"""


def main() -> int:
    spec = yaml.safe_load(SPEC.read_text(encoding='utf-8'))
    title = spec.get('info', {}).get('title', 'API') + ' - Reference'
    OUT.write_text(TEMPLATE.format(title=title, spec=json.dumps(spec)), encoding='utf-8')
    print(f'✓ wrote {OUT.relative_to(DOC.parent)} - open it in a browser')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
