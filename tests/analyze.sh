#!/bin/sh
# gcc -fanalyzer over src/ (excluding system headers and vendored src/lib/),
# failing on any finding not already in analyze-baseline.txt. Delete a line from
# the baseline once that finding is fixed.
#
# The finding set depends on the gcc version, so the baseline is keyed to the CI
# toolchain (ubuntu-24.04, gcc 13); run with that gcc to reproduce CI locally.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
baseline="$here/analyze-baseline.txt"
cd "$here/../src"
log=$(mktemp)
trap 'rm -f "$log" "$log.norm" "$log.base"' EXIT

make clean >/dev/null 2>&1 || true
# fd-phase-mismatch is disabled: gcc 13's fd-state checker misreads the plain
# socket → setsockopt → bind/listen/connect sequence and flags every such site.
# -lpthread: the conda sysroot glibc predates the libpthread merge. The build
# must succeed — a partial build silently drops every finding past the failure.
if ! make CC="${CC:-gcc}" OPT='-O2 -lm -lpthread -fanalyzer -Wno-analyzer-fd-phase-mismatch' >"$log" 2>&1; then
    echo "analyze build failed:"
    tail -20 "$log"
    exit 1
fi
make clean >/dev/null 2>&1 || true

# Normalize to "<file>: <message> [-Wanalyzer-...]" (drop line:col so the
# baseline is robust to unrelated edits), excluding system headers (absolute
# paths) and the vendored libs.
grep -E 'warning:.*-Wanalyzer' "$log" \
    | sed -E 's/^([^:]+):[0-9]+:[0-9]+: warning: /\1: /' \
    | grep -vE '^(/|lib/)' \
    | sort -u >"$log.norm" || true

# Baseline lines starting with '#' are comments justifying the entry below them.
grep -v '^#' "$baseline" 2>/dev/null >"$log.base" || true
new=$(grep -vxF -f "$log.base" "$log.norm" || true)
if [ -n "$new" ]; then
    echo "Static analysis found NEW issues (not in tests/analyze-baseline.txt):"
    echo "$new"
    exit 1
fi
echo "gcc -fanalyzer: no new issues ($(grep -c . "$log.base") baselined, vendored src/lib excluded)"
