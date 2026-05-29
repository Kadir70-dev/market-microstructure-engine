# systemd unit (optional)

`mme-engine.service` gives the engine **kernel-supervised auto-restart** —
stronger failure recovery than cron's every-10-minutes health probe, because
systemd restarts the process within seconds of any crash.

## Choose ONE lifecycle manager

| | cron + PID (`ops/start_engine.sh`, `crontab.example`) | systemd (`mme-engine.service`) |
|---|---|---|
| Needs root | No | Yes (to install the unit) |
| Restart on crash | Within ≤10 min (health probe → restart) | Within `RestartSec` (10s) |
| Session windowing (start at London open, stop after NY) | Yes, built in | Manual (add a systemd timer, or keep cron just for stop/start) |
| Best for | Free VPS, no root, session-aligned collection | A box you own, 24/5 always-on collection |

Do **not** run both — they fight over the process and the PID file.

## Install

```bash
# 1. Adjust User= and the three paths in mme-engine.service to your box.
# 2. Make sure config/api_key.txt exists (or TWELVEDATA_API_KEY is in ops/.env).
sudo cp ops/systemd/mme-engine.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mme-engine
systemctl status mme-engine
journalctl -u mme-engine -f
```

## Notes

- The engine also writes its own rotating log to `logs/engine.log` (spdlog,
  10 MB × 7), independent of journald — so `ops/health_check.py` and
  `ops/log_health.sh` keep working unchanged.
- For session windowing under systemd, add `mme-engine.timer` or keep a tiny
  cron that calls `systemctl stop/start mme-engine` at the session boundaries.
- This unit starts the engine in the **default (TwelveData) provider**. For the
  MT5 read-only provider, add `Environment=MME_PROVIDER=mt5` (and host/port) and
  ensure the bridge is reachable — the engine refuses to start if the bridge
  reports `demo_only=false`.
