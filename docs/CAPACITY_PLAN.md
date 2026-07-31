# Capacity Plan

Values are frozen by Architecture Version 1.0, Part 16.

| Quantity | Required capacity |
|---|---|
| Fixed event record | 136 B framed |
| Book snapshot record | approximately 1,060 B framed |
| Normal events per day, five symbols | approximately 1.0 million |
| Busy events per day | approximately 2.5 million |
| Raw WAL per day | approximately 350 MB; peak approximately 600 MB |
| Compressed WAL per day | approximately 70–120 MB |
| Fourteen-day recording campaign | approximately 5 GB raw / 1.5 GB compressed |
| Ninety-day retention | approximately 32 GB raw / 9 GB compressed |
| Minimum provisioned disk | 200 GB |
| Trading halt threshold | 20 GB free |
| Fixed RSS budget | at most 512 MB |

Segments rotate at 256 MB or hourly. Closed segments use zstd level 3;
retention is 90 days for raw WAL and indefinite for Parquet. These are planning
requirements only in Phase 0; persistence is not implemented here.
