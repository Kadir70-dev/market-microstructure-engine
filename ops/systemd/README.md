# systemd — 24/5 data-collection stack

Production path for running the engine **continuously, read-only, with no
trading**, to accumulate a trainable dataset over 2–4 weeks. systemd owns the
lifecycle (auto-restart, boot-start); timers handle health and cleanup.

> Use systemd **or** the cron lifecycle (`ops/crontab.example`) — **never both**;
> they would fight over the process.

## Units

| Unit | Role |
|---|---|
| `mme-engine.service` | The collector. `Restart=always`, crash-loop protection (5 fails / 300s → stop + alert), boot-start, SIGTERM-clean. |
| `mme-alert.service` | `OnFailure=` target — Telegram-pages when the engine **enters the failed state** (crash-looped past the retry limit = real outage). A single transient crash that restarts cleanly does **not** page. |
| `mme-health.service` + `.timer` | Read-only `health_check.py --skip-process` every 10 min. Pages only on FAIL; WARN (weekend-closed/frozen feed) is logged, not paged. |
| `mme-cleanup.service` + `.timer` | Weekly: journald vacuum (21d), SQLite WAL checkpoint, prune stray logs (30d), usage report. |

## Install

```bash
# 1. Build + key + (recommended) Telegram creds first:
cmake --build build
test -s config/api_key.txt
cp ops/.env.example ops/.env   # fill TELEGRAM_BOT_TOKEN + TELEGRAM_CHAT_ID for death alerts

# 2. Install + enable everything (start now + on boot):
sudo ops/install_systemd.sh
```

The installer preflights (engine binary, API key, Telegram creds), copies units to
`/etc/systemd/system`, verifies syntax, and enables the engine + both timers.

## Operate

```bash
systemctl status mme-engine
journalctl -u mme-engine -f                 # live engine log
systemctl list-timers 'mme-*'               # next health/cleanup runs
journalctl -u mme-health -n 20              # recent health results
sudo systemctl restart mme-engine           # manual restart
sudo systemctl disable --now mme-engine mme-health.timer mme-cleanup.timer   # stop everything
```

## Notes

- **Why continuous, not session-windowed?** For *data collection* you want every
  cycle, including the quiet hours and the weekend Closed/stale flags — those are
  signal. The engine already tags session + staleness, so the dataset stays clean.
- **Telegram is optional** — everything runs without it; you just won't get death
  alerts. Creds live in `ops/.env` (gitignored).
- **The engine never trades.** These units run the same read-only binary; no order
  path exists. `mme-alert`/`telegram_notify.sh` carry infra messages only.
- **Disk:** the DB grows ~tens of MB over a month; logs are bounded by spdlog
  (70 MB) + the weekly journald vacuum.
