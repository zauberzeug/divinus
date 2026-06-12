#!/bin/sh
# Static-analysis gate: gcc -fanalyzer over divinus's own sources (everything
# under src/ except the vendored src/lib/). Fails on any analyzer warning in our
# code that is not already recorded in analyze-baseline.txt.
#
# The baseline captures pre-existing findings so the gate goes green now and
# catches NEW regressions on every PR. As baselined findings get fixed (each is
# tracked by its own deck card), delete their line from analyze-baseline.txt.
# Vendored libraries (spng, miniz, schrift, shine, tinysvcmdns) are excluded.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
baseline="$here/analyze-baseline.txt"
cd "$here/../src"
log=$(mktemp)
trap 'rm -f "$log" "$log.norm"' EXIT

make clean >/dev/null 2>&1 || true
make CC="${CC:-gcc}" OPT='-O2 -lm -fanalyzer' >"$log" 2>&1 || true
make clean >/dev/null 2>&1 || true

# Normalize to "<file>: <message> [-Wanalyzer-...]" (drop line:col so the
# baseline is robust to unrelated edits), excluding the vendored libs.
grep -E 'warning:.*-Wanalyzer' "$log" \
    | grep -vE '(^|/)lib/' \
    | sed -E 's/^([^:]+):[0-9]+:[0-9]+: warning: /\1: /' \
    | sort -u >"$log.norm" || true

if [ -f "$baseline" ]; then
    new=$(grep -vxF -f "$baseline" "$log.norm" || true)
else
    new=$(cat "$log.norm")
fi
if [ -n "$new" ]; then
    echo "Static analysis found NEW issues (not in tests/analyze-baseline.txt):"
    echo "$new"
    exit 1
fi
echo "gcc -fanalyzer: no new issues ($(wc -l <"$baseline" 2>/dev/null || echo 0) baselined, vendored src/lib excluded)"
