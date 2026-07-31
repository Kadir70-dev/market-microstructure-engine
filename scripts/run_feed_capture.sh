#!/usr/bin/env bash
set -euo pipefail

required=(MME_RECORDER MME_PIPE MME_WAL_DIR MME_ACCOUNT MME_EPOCH MME_MARGIN_MODE MME_SERVER_HASH MME_SYMBOL_HASH MME_RUN_SEED)
for name in "${required[@]}"; do
  if [[ -z "${!name:-}" ]]; then
    echo "ERROR missing required environment variable ${name}" >&2
    exit 2
  fi
done

: "${MME_WINE:=wine}"
: "${MME_WINEPATH:=winepath}"
: "${MME_LOG_DIR:=${MME_WAL_DIR}/logs}"
mkdir -p -- "${MME_WAL_DIR}" "${MME_LOG_DIR}"
capture_log="${MME_LOG_DIR}/capture_$(date -u +%Y%m%d_%H%M%S).log"
wal_windows="$(${MME_WINEPATH} -w "${MME_WAL_DIR}")"

echo "Starting Windows named-pipe recorder under Wine. Attach mme_feed.mq5 after startup."
echo "Log: ${capture_log}"
"${MME_WINE}" "${MME_RECORDER}" "${MME_PIPE}" "${wal_windows}" \
  "${MME_ACCOUNT}" "${MME_EPOCH}" "${MME_MARGIN_MODE}" \
  "${MME_SERVER_HASH}" "${MME_SYMBOL_HASH}" "${MME_RUN_SEED}" \
  2>&1 | tee -a "${capture_log}"
