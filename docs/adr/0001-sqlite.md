# ADR-0001: SQLite as the metric store

**Date:** 2026-05-18
**Status:** Accepted

## Context

minimoni needs persistent metric storage compiled into a single static binary with zero
runtime dependencies. The target hardware (resource-constrained Linux systems) imposes
~1.5–2 MB RAM and expects no installation steps beyond dropping the binary.

The collect loop writes one row per minute; HTTP handlers read concurrently. Any storage
solution must support concurrent read/write and efficient range queries with aggregation
for downsampling.

## Alternatives considered

| Option | Reason rejected |
|---|---|
| RRDtool | External binary, not embeddable, no SQL query interface |
| Custom flat file | ~500+ lines for indexing, pruning, range queries, and aggregation |
| In-memory only | No persistence across restarts |

## Decision

Use the **SQLite amalgamation** (`sqlite3.c` + `sqlite3.h`, version 3.53.1, public domain).
Compiles directly into the binary. WAL mode enables concurrent readers with a single writer
without application-level mutexes.

Compiled with four tuning flags — dead code removal is delegated to LTO and linker stripping
rather than `SQLITE_OMIT_*` flags. LTO alone reduces the vendor contribution more effectively
(748 KB vs 889 KB with OMIT flags) and avoids hard-to-debug linker issues.

```
-DSQLITE_THREADSAFE=0
-DSQLITE_DEFAULT_MEMSTATUS=0
-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
-DSQLITE_LIKE_DOESNT_MATCH_BLOBS
```

At runtime: `PRAGMA journal_mode=WAL` and `PRAGMA cache_size=-256` (256 KB cap).

## Consequences

- Vendor binary contribution: ~600 KB stripped (largest of the three vendors).
- DB size at 90-day retention (1 row/min): ~25 MB + 1–4 MB WAL.
- Downsampling at query time (GROUP BY + AVG) — raw 1-minute data always preserved.
- Single-file backup: `cp metrics.db metrics.db.bak`.
