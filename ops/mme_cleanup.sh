#!/usr/bin/env bash
# Weekly disk-cleanup policy for 24/5 operation. Read-mostly and safe to run
# while the engine is live. Invoked by mme-cleanup.service/.timer (as root, so it
# can vacuum the system journal). Does NOT touch collected data rows — only
# bounds logs and reclaims WAL space.
#
#   1. Vacuum journald to a retention window (default 21 days).
#   2. Checkpoint + truncate the SQLite WAL (reclaims -wal growth; WAL-safe).
#   3. Prune stray rotated/nohup logs older than 30 days.
#   4. Report DB + disk + journald usage to ops.log.

set -u

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="$PROJECT_ROOT/logs"
DB_PATH="$PROJECT_ROOT/data/engine.db"
OPS_LOG="$LOG_DIR/ops.log"
JOURNAL_KEEP="${MME_JOURNAL_KEEP:-21d}"
LOG_KEEP_DAYS="${MME_LOG_KEEP_DAYS:-30}"

mkdir -p "$LOG_DIR"
log() { printf '%s [cleanup] %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$*" | tee -a "$OPS_LOG"; }

log "=== cleanup start ==="

# 1. journald retention (system journal needs root; ignore failure if unprivileged).
if command -v journalctl >/dev/null 2>&1; then
    before="$(journalctl --disk-usage 2>/dev/null | head -1)"
    journalctl --vacuum-time="$JOURNAL_KEEP" >/dev/null 2>&1 \
        && log "journald vacuumed to $JOURNAL_KEEP (was: $before)" \
        || log "journald vacuum skipped (need root?)"
fi

# 2. WAL checkpoint — reclaims engine.db-wal space. TRUNCATE is safe concurrently
#    with the live writer in WAL mode; sqlite serializes the checkpoint.
if [[ -f "$DB_PATH" ]] && command -v sqlite3 >/dev/null 2>&1; then
    if sqlite3 "$DB_PATH" "PRAGMA wal_checkpoint(TRUNCATE);" >/dev/null 2>&1; then
        log "WAL checkpoint(TRUNCATE) done"
    else
        log "WAL checkpoint skipped (db busy/locked — harmless, retried next week)"
    fi
fi

# 3. Prune stray nohup/rotated logs (NOT the active engine.log, which spdlog caps).
pruned=$(find "$LOG_DIR" -type f \( -name '*.out' -o -name '*.err' -o -name 'engine.*.log' \) \
    -mtime "+$LOG_KEEP_DAYS" -print -delete 2>/dev/null | wc -l)
log "pruned $pruned stray log file(s) older than ${LOG_KEEP_DAYS}d"

# 4. Report.
db_sz=$(du -h "$DB_PATH" 2>/dev/null | cut -f1)
rows=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM ticks;" 2>/dev/null || echo '?')
disk=$(df -h "$PROJECT_ROOT" | tail -1 | awk '{print $4" free of "$2" ("$5" used)"}')
jrnl=$(journalctl --disk-usage 2>/dev/null | head -1)
log "DB=$db_sz ($rows ticks) | disk: $disk | $jrnl"
log "=== cleanup done ==="
