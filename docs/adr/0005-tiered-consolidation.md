# ADR-0005: Tiered write-time consolidation

**Date:** 2026-06-20
**Status:** Accepted

## Context

The application stored every metric sample at the user's collect interval and
relied on read-time aggregation (`GROUP BY` + `AVG`) for downsampling. This worked
at `interval=60` and `retention<=90d` but degraded for long ranges and finer
intervals:

- A 90-day query at `interval=60` aggregated 129,600 raw rows on every request,
  taking 2 to 3 s on a Pi 3B (measured).
- A 10-year retention at `interval=1` would store 315 million raw rows (~35 GB at the
  measured row size), unusable on the target hardware.

The config validator allows `interval in [1, 3600]` seconds and `retention` up to ~10 years
(3,653 days, 3-leap-year margin). Read-time aggregation cannot serve those extremes.

This is solved with **write-time tiered consolidation**: raw rows are progressively
averaged into coarser buckets as they age. Queries read pre-aggregated data, with
bounded latency regardless of range or retention.

## Alternatives considered

| Option                                  | Reason rejected                                    |
|-----------------------------------------|----------------------------------------------------|
| Read-time aggregation only              | Does not scale beyond 90 d x 60 s (the v0.1 path)  |
| Two-tier (raw + coarse), like Zabbix    | Little disk saved; still scans thousands of rows   |
| Three-tier (raw + 5 m + 30 m)           | Visualization gaps; cap not guaranteed             |
| Many-tier (8+ tiers), non-human buckets | Diminishing returns after ~6 tiers                 |
| Configurable tier ladder via TOML       | Test surface x ladders; marginal benefit; deferred |

**On the three-tier sketch (the initial design):** it covers the default use case but
produces visualization gaps when the requested output resolution is finer than the coarse
tier's bucket size, so the cap on `points` becomes aspirational rather than guaranteed.

**On many tiers vs six:** more tiers reduce disk asymptotically, but each transition is a
place where the bucket-end predicate must be exact; returns diminish after ~6 tiers.

**On a configurable ladder:** a TOML-defined ladder multiplies the test surface by the
number of possible ladders for marginal benefit to the median user; deferred to future work.

## Decision

A **fixed 6-tier ladder** with **human-meaningful bucket sizes and age boundaries**.

| Tier | Bucket |   Up to age | `bucket_sec` value |
|------|-------:|------------:|-------------------:|
| Raw  |    1 s |         2 h |                  1 |
| T1   |    5 s |        12 h |                  5 |
| T2   |   30 s |         5 d |                 30 |
| T3   |    5 m |        60 d |                300 |
| T4   |    1 h | 365 d (1 y) |              3,600 |
| T5   |    6 h |   retention |             21,600 |

Bucket ratios: **5 / 6 / 10 / 12 / 6** (monotonic ascending up to T4; T5 closes
defensively to bound disk at long retention).

The table is the canonical ladder at the finest `interval = 1`. Raw rows carry
`bucket_sec = the configured collect interval`, so at the default `interval = 60` the
raw tier stores 60 s samples and any tier whose bucket is <= the interval is skipped
(see Tier-skip below). The consolidated tiers T1-T5 always use the fixed `bucket_sec`
shown.

### Cap on `points` query parameter: 1,440

`1,440 = 60 x 24` is one data point per minute over a 24-hour window, the resolution the
dashboard targets. It is also the **design point** of the ladder: every tier boundary is
exactly large enough that `bucket x 1,440 <= boundary`. Verified at each transition:

| Transition      | `bucket_next x 1,440` | Boundary | Margin |
|-----------------|-----------------------|----------|--------|
| Raw -> T1 (5 s) | 7,200 s = 2 h         | 2 h      | exact  |
| T1 -> T2 (30 s) | 43,200 s = 12 h       | 12 h     | exact  |
| T2 -> T3 (5 m)  | 432,000 s = 5 d       | 5 d      | exact  |
| T3 -> T4 (1 h)  | 5,184,000 s = 60 d    | 60 d     | exact  |
| T4 -> T5 (6 h)  | 31,104,000 s = 360 d  | 365 d    | 5 d    |

Therefore `cap = 1,440` is **deliverable in every range >= ~24 min** (interval-limited
below that). Raising it any higher requires extending at least one boundary; lowering
it wastes capacity the ladder already provides.

### Tier-skip rule for `interval != 1`

A tier applies iff `tier.bucket > interval` (strict). Tiers with `bucket <= interval`
are skipped. The raw tier extends through any skipped tiers up to the first applicable
tier's start age (= the predecessor's `max_age` in the canonical ladder).

Examples:

| Interval       | Applicable tiers                 | Raw covers               |
|----------------|----------------------------------|--------------------------|
| 1 s            | Raw, T1, T2, T3, T4, T5          | 0 - 2 h                  |
| 60 s (default) | Raw, T3, T4, T5 (T1, T2 skipped) | 0 - 5 d (= T2.max_age)   |
| 3,600 s        | Raw, T5 (T1-T4 skipped)          | 0 - 365 d (= T4.max_age) |

### Consolidation predicate: bucket-end, not row-level

For each transition, the `WHERE` clause operates on the bucket boundary:

```sql
WHERE (CAST(strftime('%s', timestamp) AS INTEGER) / <bucket_next>) * <bucket_next>
        + <bucket_next>
      <= CAST(strftime('%s', 'now') AS INTEGER) - <threshold_seconds>
  AND (bucket_sec IS NULL OR bucket_sec < <bucket_next>)
```

A row-level predicate (`timestamp < now - threshold`) would fire as each raw row
crosses the threshold, producing one duplicate medium row **per collect cycle** inside
the same bucket window. At default `interval = 60` the bug produces 5 duplicates per
5-min bucket; at `interval = 1` it produces 300. The bucket-level predicate
guarantees that all rows in a given bucket qualify together or not at all.

This predicate applies to all 5 tier transitions. See `tests/unit-db.c` for regression
tests.

### Single `BEGIN IMMEDIATE / COMMIT` per cycle

All 5 consolidate passes execute within one transaction, called once per collect cycle
after `db_insert` and before `db_prune`. Most passes are no-ops (cheap index scans) at
any given moment; consolidation only happens when a bucket's age crosses the
corresponding threshold.

## Consequences

### Disk

Measured on Alpine/musl with no VACUUM, the way the daemon runs; the extreme is the
modelled steady state (its 315 M raw rows cannot be populated directly).

| Configuration                                 | Total rows | Disk    |
|-----------------------------------------------|------------|---------|
| Default (`interval = 60`, `retention = 90 d`) | ~23,850    | ~2.6 MB |
| Extreme (`interval = 1`, `retention = 10 y`)  | ~63,672    | ~7.7 MB |

The 6-tier ladder uses more disk than a coarser ladder would, in exchange for gap-free
coverage at every range and smoother resolution across tier boundaries:

- `cap = 1,440` deliverable in every range (aspirational under a coarser ladder).
- No visualization gaps at any range within `[1 h, retention]`.
- Smoother resolution transitions across tier boundaries.

### Query latency

`/api/metrics?range=*` now reads pre-aggregated rows. Crucially, latency scales with
the number of points returned (the tier resolution for the range), not with how much
raw history the database holds: consolidation collapses old samples into coarse buckets,
so a query groups far fewer rows than the equivalent raw scan. The points returned are
bounded: a range within the ladder yields at most the `points` cap (1,440); only a
10-year range exceeds that at the coarsest (1-day) bucket, returning ~3,654 points.

Measured end-to-end on a Pi 3B (median of 5, build `60a8304`, full HTTP request including
JSON serialisation), on a full 90-day dataset:

| Range | v0.2 tiered | v0.1 raw-scan |
|-------|-------------|---------------|
| 1 d   | 61 ms       | 101 ms        |
| 7 d   | 156 ms      | 457 ms        |
| 30 d  | 278 ms      | ~1.7 s        |
| 90 d  | 421 ms      | ~3.0 s        |

Long-range queries drop from the v0.1 multi-second raw-scan regime to sub-500 ms, ~6-7x
faster at 30-90 d. Extending the dataset from 65 to a full 90 days raised the 90-day tab
by only ~23 ms (131 -> 181 points): the tiered read is bounded by resolution, not data
depth: the design goal of this ADR.

### Schema

A new column `bucket_sec INTEGER` is added to the `metrics` table. Raw rows carry
`bucket_sec = interval`; consolidated rows carry the bucket size of their tier
(5, 30, 300, 3,600, or 21,600).

`PRAGMA application_id` and `PRAGMA user_version` are used to mark new databases.
Migration is deferred to a future commit.

### Boundaries that did not survive iteration

The design went through many candidate ladders before converging. Notable rejected
variants:

- **T4 ending at 30 d**: leaves a "visualization gap" between 30 d and `retention` for
  default users (T5 at 1 d resolution gives < 1 source per typical output bucket).
  Replaced by T4 ending at 365 d.
- **Adding a 1 m tier between T2 and T3**: saves a marginal amount of disk (under 1 MB)
  at `interval = 1`, but does not apply at `interval >= 60` (the common case) and
  breaks the monotonic bucket-ratio property.
- **Adding a 12 h or 1 d tier after T5**: saves a marginal amount of disk (under 1 MB)
  across all configs but adds a 6th transition's worth of consolidate code. It could be
  considered if disk pressure becomes a real complaint.

### Open questions deferred to future work

- Configurable tier ladder via TOML.
- Adaptive cap per range (the server already returns `min(requested, available)`).
- Schema fingerprint verification via canonical `PRAGMA table_info` hash.
- The 1 m and 12 h / 1 d optimisation tiers identified above.

### Clock-skew assumption

The bucket-end predicate assumes a monotonically advancing wall clock. NTP corrections
that jump backwards across a bucket boundary can produce duplicate consolidated rows.
The risk is low on well-configured systems (NTP step typically < seconds). Possible
future hardening: cheap detection via `MAX(timestamp)` check before insert, or
migration to `CLOCK_BOOTTIME` for timestamps (would require a schema change).
