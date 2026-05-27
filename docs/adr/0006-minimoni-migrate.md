# ADR-0006: minimoni-migrate as separate binary

**Date:** 2026-05-27
**Status:** Accepted

## Context

Schema and data migrations are needed between minimoni versions
(v0.1→v0.2 added the `bucket_sec` column, normalized timestamps to
T+Z format, and bumped `user_version` to 1). Future versions will
introduce more.

Requirements:

- Versioned and explicit: the user knows which migration ran.
- Atomic per migration: a failed step rolls back, never half-applied.
- Decoupled from the daemon lifecycle: a user can migrate offline,
  before starting the new daemon.
- Reviewable: every SQL change visible in a PR diff.
- Distributable separately from the daemon if backporting is needed.

## Alternatives considered

| Option | Reason rejected |
|---|---|
| Built-in `minimoni db migrate --to N` subcommand | Couples migration logic to the daemon binary; cannot be backported or shipped independently; mixes "operate" and "evolve" concerns in one CLI. |
| Separate `minimoni-migrate` binary that links sqlite directly | Two binaries both touching the DB file. Doubles the surface that must agree on PRAGMAs, WAL mode, file path resolution. ~600 KB extra binary (sqlite amalgamation) just to repeat what minimoni already does. |
| External shell scripts driving the `sqlite3` CLI | Adds a runtime dependency (`sqlite3`) that minimoni otherwise avoids. Shell quoting of SQL is a foot-gun. |

## Decision

`minimoni-migrate` is a **separate C binary** built from `minimoni/`
(same repo, `make minimoni-migrate`). It is the **only** place that
knows the migration matrix: source/target `user_version`, ordered
list of SQL steps per migration, and rollback strategy on failure.

It does **not** link sqlite3 and does not open the `.db` file
directly. For SQL execution it `fork`+`execvp`'s `minimoni db exec`,
passing the SQL as an argv element (no shell, no quoting). Each
migration runs as a single multi-statement script in one call;
preflight uses one call per probe query. Exit codes signal
success/failure.

Migration model is **Flyway/pg_upgrade-style**: per-version step
arrays embedded as C constants, applied in strict order, with a
`user_version` bump as the final step of each migration.

## Consequences

**Positive**

- One binary (minimoni) opens the DB file; locks, WAL, PRAGMAs in
  one place.
- minimoni-migrate stays tiny: 67 KB stripped against musl-libc
  static (measured on alpine-amd64 with `-O2 -static`).
- Migration SQL reviewable as C string constants in PR diffs.
- minimoni-migrate can be cut as its own GitHub release if a user
  needs to migrate a corrupt or stalled DB without upgrading the
  daemon.
- The `minimoni db exec` contract becomes a public, stable API:
  any change to it requires bumping the consumer alongside.

**Negative**

- `minimoni db exec` must exist and remain stable across versions.
  Breaking changes to its CLI require a coordinated bump.
- fork+exec per `minimoni db exec` call adds ~1–3 ms overhead.
  A typical migration is ~6 calls (4 preflight + 1 fingerprint +
  1 migration script), so total overhead is sub-20 ms. Negligible.
- Two binaries to ship per release. Same pipeline, minor cost.

## Implementation notes

- v0.2 ships the first migration: `user_version` 0 (the v0.1
  schema) → `user_version` 1 (the v0.2 schema). Three schema
  changes plus one data reconstruction:
    - add `bucket_sec INTEGER` (tier marker for consolidation)
    - add `net_rx_bps REAL`, `net_tx_bps REAL`; drop the v0.1
      `net_rx_bytes INTEGER`, `net_tx_bytes INTEGER` cumulative
      counters. Rates are reconstructed from byte deltas over
      timestamp deltas between consecutive rows (the same
      arithmetic the v0.2 daemon performs at insert time). First
      row of the DB has no predecessor and ends up with NULL bps —
      one-row gap accepted.
    - normalise `timestamp` strings to `YYYY-MM-DDTHH:MM:SSZ`
    - set `application_id` to the moni magic and `user_version` to 1
- `minimoni db exec` accepts **multi-statement scripts** in a
  single invocation (executes them in order, aborts at the first
  error, exit code reflects which statement failed).

### Safety: defense in depth

`minimoni-migrate` runs four layers on every migration:

1. **Preflight + version routing** (read-only, no writes). Refuses
   to migrate (or exits cleanly when "nothing to do") on any of:
   - `PRAGMA integrity_check` is not `ok` → refuse
   - `PRAGMA application_id` is neither `0` (legacy/unset, the v0.1
     daemon never wrote it) nor `0x6D6F6E69` (moni magic) → refuse
   - The `metrics` table or its `timestamp` column is missing →
     refuse ("this is a SQLite file but not a minimoni metrics DB")
   - `PRAGMA user_version` equals the latest version this build
     knows → exit 0, nothing to do
   - `PRAGMA user_version` exceeds the latest version this build
     knows → refuse (DB from a future version)
   - No migration starts from the current `user_version` → refuse
     (this build cannot continue the chain)
2. **Schema fingerprint** (read-only). The source migration's
   `verify_source` reads every entry from `sqlite_master.sql`
   ordered by `(type, name)`, joins them with `'\n'`, and compares
   the result byte-for-byte against the canonical schema the
   daemon of that version produced. Any deviation — reordered
   columns, different types, extra tables, renamed indexes,
   extraneous whitespace — fails the fingerprint and aborts before
   any write or snapshot. This catches manual interventions on the
   DB (e.g. someone added a column with `sqlite3` CLI) that would
   otherwise silently invalidate the migration's assumptions.
3. **Snapshot** (default on, `--no-backup` opts out). `cp <db>
   <db>.backup-pre-migrate-vN` before any write. Last-resort
   recovery for unknown unknowns (logic bugs in the migration
   script, SQLite engine bugs, post-hoc regret).
4. **Transaction**. The migration script is `BEGIN; ...steps...;
   COMMIT;` — any SQL error inside aborts and `ROLLBACK`s
   automatically. Idempotent retries become safe.

`--no-backup` exists for the case of huge DBs on space-constrained
hosts where the 1× disk cost is unacceptable. Documented as
"only use if you have your own backup strategy".

`minimoni-migrate` exits 0 only if the final `PRAGMA user_version`
bump succeeded.
