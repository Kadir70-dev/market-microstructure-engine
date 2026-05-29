#!/usr/bin/env bash
# Quick check of the MQL5 EA's quote CSV — the sole data feed.
# Exit 0 = fresh, 1 = missing, 2 = stale (EA stalled / terminal closed).
#
#   ops/check_quotes.sh                       # default ../data/mme_quotes.csv
#   ops/check_quotes.sh /abs/path/mme_quotes.csv
#   MME_FILE_STALE_S=60 ops/check_quotes.sh

set -u
OPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$OPS_DIR/.." && pwd)"

CSV="${1:-${MME_QUOTES_CSV_ABS:-$PROJECT_ROOT/data/mme_quotes.csv}}"
STALE="${MME_FILE_STALE_S:-120}"

if [[ ! -f "$CSV" ]]; then
    echo "QUOTES MISSING: $CSV (is the MT5 EA running and writing it?)"
    exit 1
fi

age=$(( $(date +%s) - $(stat -c %Y "$CSV") ))
rows=$(grep -c . "$CSV" 2>/dev/null || echo 0)

echo "file:  $CSV"
echo "age:   ${age}s (threshold ${STALE}s)"
echo "rows:  $rows"
echo "latest:"
sed 's/\r$//' "$CSV" | sed 's/^/  /'

if (( age > STALE )); then
    echo "QUOTES STALE: not updated for ${age}s"
    exit 2
fi
echo "QUOTES OK: fresh"
exit 0
