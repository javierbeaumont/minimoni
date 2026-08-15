# ADR-0006: minimoni-migrate as separate binary

**Date:** 2026-06-23
**Status:** Accepted

## Context

Schema and data migrations are needed between minimoni versions
(v0.1 -> v0.2 added the `bucket_sec` column, replaced the cumulative
net byte counters with rates, and bumped `user_version` to 1). Future
versions will introduce more.

Requirements:

- Versioned and explicit: the user knows which migration ran.
- Atomic per migration: a failed step rolls back, never half-applied.
- Decoupled from the daemon lifecycle: a user can migrate offline,
  before starting the new daemon.
- Reviewable: every SQL change visible in a PR diff.
- Distributable separately from the daemon if backporting is needed.

## Alternatives considered

| Option                                  | Reason rejected                                   |
|-----------------------------------------|---------------------------------------------------|
| Built-in `db migrate` subcommand        | Couples migration to the daemon; not backportable |
| Separate binary linking sqlite directly | Two binaries on the DB; ~600 KB extra             |
| Shell scripts over the `sqlite3` CLI    | Adds a runtime dep; SQL shell-quoting foot-gun    |

The built-in subcommand mixes "operate" and "evolve" concerns in one CLI and
cannot be shipped or backported independently. A separate binary that links
sqlite would have two binaries both touching the DB file, doubling the surface
that must agree on PRAGMAs, WAL mode, and path resolution, plus ~600 KB of
amalgamation just to repeat what minimoni already does. Shell scripts would
reintroduce the `sqlite3` runtime dependency minimoni otherwise avoids, and
quoting SQL in shell is a foot-gun.

## Decision

`minimoni-migrate` is a **separate C binary** built from `minimoni/`
(same repo, `make minimoni-migrate`). It is the **only** place that
knows the migration matrix: source/target `user_version`, ordered
list of SQL steps per migration, and rollback strategy on failure.

It does **not** link sqlite3 and does not open the `.db` file
directly. For SQL execution it `fork`+`execvp`'s `minimoni db exec`,
passing the SQL as an argv element (no shell, no quoting).

Migration model is **Flyway/pg_upgrade-style**: per-version step
arrays applied in strict order, with a `user_version` bump as the
final step of each migration.

## Consequences

**Positive**

- One binary (minimoni) opens the DB file; locks, WAL, PRAGMAs in
  one place.
- minimoni-migrate stays tiny: it links no sqlite, only the migration
  matrix and the `db exec` glue.
- Migration SQL reviewable as C string constants in PR diffs.
- minimoni-migrate can be cut as its own GitHub release if a user
  needs to migrate a corrupt or stalled DB without upgrading the
  daemon.
- The two binaries couple through one narrow contract, internal to
  the pair rather than a command offered to operators.

**Negative**

- `minimoni db exec` must exist and remain stable across versions.
  Breaking changes to its CLI require a coordinated bump.
- fork+exec per `minimoni db exec` call adds a small overhead,
  negligible for a one-shot offline migration.
- Two binaries to ship per release. Same pipeline, minor cost.

## Implementation notes

- v0.2 ships the first migration: `user_version` 0 (the v0.1
  schema) -> `user_version` 1 (the v0.2 schema).

### Safety: defense in depth

`minimoni-migrate` runs four layers on every migration:

1. **Preflight + version routing** (read-only). Confirms the file is a
   minimoni database (application_id, integrity, the expected tables)
   and routes on `user_version`: nothing to do if already current,
   refuse if it comes from a newer build or if no migration starts from
   it. The exact probes live in `preflight.c`.
2. **Schema fingerprint** (read-only). Compares a STRUCTURAL fingerprint
   of the source schema (columns and types, not raw SQL text) against
   the canonical one for that version. Being structural makes it
   insensitive to column order and `CREATE TABLE` whitespace, so a DB
   whose columns were appended with `ALTER TABLE ADD COLUMN` still
   migrates without manual surgery; a genuine divergence (a different
   type, an extra or missing column/table/index) aborts before any
   write. `--force` skips this one layer for a schema the operator has
   reviewed.
3. **Snapshot** (default on, `--no-backup` opts out, mandatory under
   `--force`). A copy of the DB is taken before any write: last-resort
   recovery for unknown unknowns (a logic bug in the migration script, a
   SQLite engine bug, post-hoc regret).
4. **Transaction**. The migration script is `BEGIN; ...steps...;
   COMMIT;`: any SQL error inside aborts and `ROLLBACK`s automatically,
   so idempotent retries are safe.

`--no-backup` exists for huge DBs on space-constrained hosts where the
1x disk cost is unacceptable; it cannot be combined with `--force`, so
forcing past the fingerprint always keeps the safety net.

In a normal run, `minimoni-migrate` exits 0 only if the final
`PRAGMA user_version` bump succeeded. (Dry-run has its own exit
semantics; see below.)

### Dry-run: rehearse before touching production

`--dry-run` runs the read-only checks against the real database, then
rehearses the migration on a throwaway copy beside the original (same
filesystem, so disk-space conditions match). The original is never
written and no snapshot is left behind. A machine-readable status line
on stdout and a binary exit code (safe to migrate / would not apply)
let a deployment step verify an upgrade *before* swapping the daemon
binary, instead of discovering an incompatibility from a crash-looping
daemon after the swap.

### Schema-mismatch recovery

A fingerprint mismatch means a genuine structural difference from the
canonical schema. The operator either re-runs with `--force` (when the
divergence is irrelevant to the migration) or restores the canonical
schema by hand, then migrates normally. The operator-facing procedure
is in the README (`Upgrading`).
