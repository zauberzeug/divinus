#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["openapi-spec-validator>=0.7", "pyyaml>=6"]
# ///
"""Static linter for the Divinus OpenAPI spec.

Runs two passes over ``doc/openapi.yaml``:

  1. Spec validity - validates the document against the OpenAPI 3.1
     meta-schema via openapi-spec-validator (catches malformed specs,
     dangling ``$ref``s, wrong types).
  2. House style    - a few lightweight rules so the published API docs
     stay consistent: every operation has a summary, every parameter has
     a schema and description, every response has a description.

Invoked through ``pixi run lint-api``. Exits non-zero on any problem so
CI fails. Dependencies are declared inline (PEP 723) and resolved by uv
at run time, so this adds no entry to the locked pixi environment.
"""
from __future__ import annotations

import sys
from pathlib import Path

import yaml
from openapi_spec_validator import validate

try:  # error location moved across openapi-spec-validator releases
    from openapi_spec_validator.validation.exceptions import OpenAPIValidationError
except Exception:  # pragma: no cover - fall back to a broad catch
    OpenAPIValidationError = Exception  # type: ignore[assignment, misc]

HTTP_METHODS = {'get', 'put', 'post', 'delete', 'patch', 'head', 'options', 'trace'}
SPEC = Path(__file__).resolve().parent.parent / 'doc' / 'openapi.yaml'


def iter_operations(spec: dict):
    """Yield (route, method, operation) for every HTTP operation in the spec."""
    for route, item in (spec.get('paths') or {}).items():
        if not isinstance(item, dict):
            continue
        for method, op in item.items():
            if method.lower() in HTTP_METHODS and isinstance(op, dict):
                yield route, method, op


def lint_style(spec: dict) -> list[str]:
    """House rules beyond schema validity. Returns a list of problems."""
    problems: list[str] = []
    for route, method, op in iter_operations(spec):
        where = f'{method.upper()} {route}'
        if not op.get('summary'):
            problems.append(f'{where}: missing summary')
        for param in op.get('parameters', []):
            if '$ref' in param:
                continue
            name = param.get('name', '?')
            if not param.get('description'):
                problems.append(f"{where}: parameter '{name}' missing description")
            if 'schema' not in param:
                problems.append(f"{where}: parameter '{name}' missing schema")
        responses = op.get('responses') or {}
        if not responses:
            problems.append(f'{where}: no responses defined')
        for code, resp in responses.items():
            if isinstance(resp, dict) and '$ref' not in resp and not resp.get('description'):
                problems.append(f"{where}: response '{code}' missing description")
    return problems


def main() -> int:
    if not SPEC.exists():
        print(f'✗ spec not found: {SPEC}', file=sys.stderr)
        return 1

    spec = yaml.safe_load(SPEC.read_text(encoding='utf-8'))

    try:
        validate(spec)
    except OpenAPIValidationError as exc:
        print(f'✗ {SPEC.name} is not a valid OpenAPI document:\n{exc}', file=sys.stderr)
        return 1

    problems = lint_style(spec)
    n_ops = sum(1 for _ in iter_operations(spec))
    if problems:
        print(f'✗ {len(problems)} style issue(s) in {SPEC.name}:', file=sys.stderr)
        for problem in problems:
            print(f'  - {problem}', file=sys.stderr)
        return 1

    print(f'✓ {SPEC.name}: valid OpenAPI 3.1, {n_ops} operation(s), style clean')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
