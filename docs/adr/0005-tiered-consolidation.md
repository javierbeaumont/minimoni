# ADR-0005: Tiered write-time consolidation

**Date:** 2026-05-25
**Status:** Accepted

## Context

The application stored every metric sample at the user's collect interval and
relied on read-time aggregation (`GROUP BY` + `AVG`) for downsampling. This worked
at `interval=60s` and `retention≤90d` but degraded for long ranges and finer
intervals:

- A 90-day query at `interval=60s` aggregated 129,600 raw rows on every request —
  400 ms to 2 s on a Pi 3B.
- A 10-year retention at `interval=1s` would store 315 million raw rows (~53 GB),
  unusable on the target hardware.

The config validator allows `interval ∈ [1, 3600] s` and `retention` up to ~10 years
(3,653 d, 3-leap-year margin). Read-time aggregation cannot serve those extremes.

This is solved with **write-time tiered consolidation**: raw rows are progressively
averaged into coarser buckets as they age. Queries read pre-aggregated data, with
bounded latency regardless of range or retention.

## Alternatives considered

| Option | Reason rejected |
|---|---|
| Continue with read-time aggregation only | Does not scale beyond 90 d × 60 s; ruled out by performance target on Pi 3B. |
| Two-tier (raw + coarse), like Zabbix | Saves no significant disk vs full read-time aggregation; long-range queries still scan thousands of rows. |
| Three-tier (raw + 5 m + 30 m), the initial sketch | Covers the default use case but produces visualization gaps when the requested output resolution is finer than the coarse tier's bucket size. The cap on `points` becomes aspirational rather than guaranteed. |
| Many-tier (8+ tiers) with non-human buckets | More tiers do reduce disk asymptotically, but each transition is a place where the bucket-end predicate must be exact (see "bug history" below). Diminishing returns after ~6 tiers. |
| Configurable tier ladder via TOML | Multiplies test surface by the number of possible ladders, with marginal benefit for the median user. Deferred to a future release. |

## Decision

A **fixed 6-tier ladder** with **human-meaningful bucket sizes and age boundaries**.

| Tier | Bucket | Up to age | `bucket_sec` value |
|---|---|---|---|
| Raw | 1 s | 2 h | 1 |
| T1 | 5 s | 12 h | 5 |
| T2 | 30 s | 5 d | 30 |
| T3 | 5 m | 60 d | 300 |
| T4 | 1 h | 365 d (1 y) | 3,600 |
| T5 | 6 h | retention | 21,600 |

Bucket ratios: **5 / 6 / 10 / 12 / 6** (monotonic ascending up to T4; T5 closes
defensively to bound disk at long retention).

### Cap on `points` query parameter: 1,440

`1,440 = 60 × 24` (the number of minutes in a day — a useful coincidence). More
importantly, it is the **design point** of the ladder: every tier boundary is
exactly large enough that `bucket × 1,440 ≤ boundary`. Verified at each transition:

| Transition | `bucket_next × 1,440` | Boundary | Margin |
|---|---|---|---|
| Raw → T1 (5 s) | 7,200 s = 2 h | 2 h | exact |
| T1 → T2 (30 s) | 43,200 s = 12 h | 12 h | exact |
| T2 → T3 (5 m) | 432,000 s = 5 d | 5 d | exact |
| T3 → T4 (1 h) | 5,184,000 s = 60 d | 60 d | exact |
| T4 → T5 (6 h) | 31,104,000 s = 360 d | 365 d | 5 d |

Therefore `cap = 1,440` is **deliverable in every range ≥ ~24 min** (interval-limited
below that). Raising it any higher requires extending at least one boundary; lowering
it wastes capacity the ladder already provides.

### Tier-skip rule for `interval ≠ 1 s`

A tier applies iff `tier.bucket > interval` (strict). Tiers with `bucket ≤ interval`
are skipped. The raw tier extends through any skipped tiers up to the first applicable
tier's start age (= the predecessor's `max_age` in the canonical ladder).

Examples:

| Interval | Applicable tiers | Raw covers |
|---|---|---|
| 1 s | Raw, T1, T2, T3, T4, T5 | 0 – 2 h |
| 60 s (default) | Raw, T3, T4, T5 (T1, T2 skipped) | 0 – 5 d (= T2.max_age) |
| 3,600 s | Raw, T5 (T1–T4 skipped) | 0 – 365 d (= T4.max_age) |

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
the same bucket window. At default `interval = 60 s` the bug produces 5 duplicates per
5-min bucket; at `interval = 1 s` it produces 300. The bucket-level predicate
guarantees that all rows in a given bucket qualify together or not at all.

This predicate applies to all 5 tier transitions. See `tests/unit.c` for regression
tests.

### Single `BEGIN IMMEDIATE / COMMIT` per cycle

All 5 consolidate passes execute within one transaction, called once per collect cycle
after `db_insert` and before `db_prune`. Most passes are no-ops (cheap index scans) at
any given moment; consolidation only happens when a bucket's age crosses the
corresponding threshold.

## Consequences

### Disk

| Configuration | Total rows | Disk |
|---|---|---|
| Default (`interval = 60 s`, `retention = 90 d`) | 23,760 | ~4 MB |
| Extreme (`interval = 1 s`, `retention = 10 y`) | 63,672 | ~10.7 MB |

Compared to the initial 3-tier sketch (4.65 MB extreme / 1.37 MB default), the 6-tier
ladder costs ~2.3× / ~2.9× more disk. The increase pays for:

- `cap = 1,440` deliverable in every range (vs aspirational in the 3-tier).
- No visualization gaps at any range within `[1 h, retention]`.
- Smoother resolution transitions across tier boundaries.

### Query latency

`/api/metrics?range=*` now reads pre-aggregated rows: bounded at ~5,000 rows per query
worst-case (range = 10 y touches T5 at 6 h resolution). Expected p99 < 10 ms on Pi 3B
(vs 400 ms – 2 s in v0.1 for equivalent long-range queries).

### Schema

A new column `bucket_sec INTEGER` is added to the `metrics` table. Raw rows carry
`bucket_sec = interval`; consolidated rows carry the bucket size of their tier
(5, 30, 300, 3,600, or 21,600).

`PRAGMA application_id` and `PRAGMA user_version` are used to mark new databases.
Migration is deferred to a future release.

### Boundaries that did not survive iteration

The design went through many candidate ladders before converging. Notable rejected
variants:

- **T4 ending at 30 d**: leaves a "visualization gap" between 30 d and `retention` for
  default users (T5 at 1 d resolution gives < 1 source per typical output bucket).
  Replaced by T4 ending at 365 d.
- **Adding a 1 m tier between T2 and T3**: saves ~1 MB at `interval = 1 s`, but does
  not apply at `interval ≥ 60 s` (the common case) and breaks the monotonic
  bucket-ratio property.
- **Adding a 12 h or 1 d tier after T5**: saves ~1 MB across all configs but adds a
  6th transition's worth of consolidate code. It could be considered if disk pressure
  becomes a real complaint.
- **`cap = 5,120` (initial proposal based on 5K monitor width)**: not deliverable in
  most ranges; would have required ~38 MB of disk to satisfy.

### Open questions deferred to future releases

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
