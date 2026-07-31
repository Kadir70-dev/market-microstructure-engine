# Clock Discipline

Requirements are frozen by Architecture Version 1.0, Part 14.

- Local monotonic time (`CLOCK_MONOTONIC` or calibrated TSC) is authoritative
  for latency and ordering.
- Startup must verify `constant_tsc` and `nonstop_tsc`. If either is absent,
  the engine must fall back to `clock_gettime(CLOCK_MONOTONIC)`.
- The hot thread is pinned to one core before TSC timing is used.
- TSC-to-nanosecond calibration runs at startup and is recorded in every WAL
  and journal header.
- Terminal-to-local offset is continuously estimated from heartbeat round
  trips using a rolling minimum filter.
- All event timestamps are normalized to UTC nanoseconds.
- NTP must slew rather than step. A detected wall-clock step emits a
  `ClockStepEvent`; monotonic ordering remains authoritative and the interval
  is excluded from research.

Phase 0 documents these requirements only. Clock and TSC implementation belongs
to Phase 1 and has not started.
