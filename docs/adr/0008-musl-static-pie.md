# ADR-0008: Static musl-PIE build toolchain

**Date:** 2026-07-17
**Status:** Accepted

## Context

minimoni ships as a single statically linked binary with zero runtime
dependencies (the vendored libraries compiled in are covered by ADR-0001 through
ADR-0004). Static linking itself is not in question; the open choice is *which
libc* the static binary links against, and how it is linked.

minimoni has shipped musl-static-PIE (built in an Alpine container) since its
first release. Before v0.2 the choice was re-opened against glibc-static, the
more common Linux libc with a broader toolchain ecosystem. Rather than decide by
assumption, the same source was built both ways and measured, with particular
attention to resident memory (PSS) under sustained dashboard query load, the
metric that matters most for a tool whose premise is running on 512 MB boards. A
related knob was probed alongside: how many `sqlite3_db_release_memory()` calls
to wire in, and where, since the C code frees every buffer and hands SQLite's
page cache back after each heavy `/api/metrics` query.

## Comparison setup

One source revision, one host (a Raspberry Pi 3B, arm64), one stress sequence.
Each variant ran as an isolated process against its own fresh copy of the same
database snapshot (copied per variant, so every run starts from an identical
state), settled for about 30 s, then took the same load: 20x
`GET /api/metrics?range=7d` (the heaviest query the API serves) followed by one
sweep over the `1d`, `7d`, `30d` and `90d` tabs. PSS was captured from
`/proc/<pid>/smaps_rollup` before and after.

The comparison reported here is the toolchain axis: glibc-static vs
musl-static-pie, same source and same flags otherwise, both at the shipped
release-call layout (one call, in the metrics handler). Release-call placement
was probed alongside and proved second-order (see Measurements), so the numbers
below fix it at the shipped layout.

## Measurements

Same source (`6ac7536`), same host, same load, each toolchain as an isolated
process against its own copy of the production database snapshot, both carrying
the shipped release-call layout (one call, in the metrics handler). PSS from
`smaps_rollup`, warm (after settle) and again after the stress sweep:

| Toolchain (`6ac7536`, isolated) | PSS warm | PSS post-stress |            Delta |
|---------------------------------|---------:|----------------:|-----------------:|
| glibc static                    |   2.6 MB |         13.3 MB | +11 MB, retained |
| musl static-pie                 |  1.74 MB |          1.6 MB |   -0.1 MB, trims |

The difference is the allocator. glibc keeps freed memory in per-thread malloc
arenas and returns it to the OS only when explicitly trimmed, so the low 2.6 MB
holds only until the first heavy `/api/metrics` sweep; after that PSS sits near
13 MB for the life of the daemon. musl returns the same buffers and stays at
~1.6 MB. Where the single `sqlite3_db_release_memory()` call sits does not change
this (glibc retains either way, musl trims either way), so the toolchain is the
determining factor and the release-call placement is second order. Older
non-isolated glibc builds (earlier revisions, multi-hour uptime) showed the same
direction, sitting at 9 to 12.5 MB without trimming back.

Binary size and virtual footprint, same source and host:

| Property                      | glibc static | musl static-pie |
|-------------------------------|-------------:|----------------:|
| `minimoni` binary (stripped)  | ~1.9 MB      | ~1.24 MB        |
| `minimoni-migrate` (stripped) | ~584 KB      | ~66 KB          |
| PSS, warm daemon              | ~2.6 MB      | ~1.74 MB        |
| PSS, steady under load        | ~13 MB       | ~1.6 MB         |
| VmPeak (virtual reserved)     | ~600 MB      | ~3 MB           |
| Swap under long uptime        | nonzero      | 0               |

At rest the two are close (~2.6 MB glibc vs ~1.74 MB musl); the gap opens under
sustained query load, where a daemon in normal use holds about 8x less resident
memory on musl. musl is also about 35% smaller as a binary and reserves a few MB
of virtual address space against the ~600 MB glibc holds for worker-thread
arenas; static-PIE makes the whole image position-independent (ASLR over the
executable, not just shared objects).

The one cost is latency. musl's stdlib (printf, regex, sorting) is slower per
call, adding roughly 10 to 30 ms to an `/api/metrics` request (the 90d tab:
~397 ms under glibc, ~427 ms under musl). At this request volume it is
invisible, and it is dwarfed by the tiered-read latency win of ADR-0005.

## Decision

**Build every release binary as musl-static-pie.** This applies to both
`minimoni` and `minimoni-migrate`, across all four published architectures
(linux-amd64, linux-arm64, linux-armv7, linux-armv6). Builds run inside Alpine
(musl) containers, which is already how the release pipeline operates for the
minification tool (ADR-0007) and the rest of the toolchain.

Keep the single `sqlite3_db_release_memory()` call in the metrics handler: it is
correct under musl and cheap. Do **not** add a second release call in the
consolidation path: musl already trims on its own, so it is redundant, and under
glibc the placement makes no difference anyway.

## Consequences

**Positive**

- About 35% smaller `minimoni` binary than the glibc-static build (~1.24 MB vs
  ~1.9 MB) and a ~66 KB `minimoni-migrate` (vs ~584 KB), so every download and
  every embedded byte stays small.
- Roughly 8x lower resident memory in steady state than glibc (~1.6 MB vs
  ~13 MB once the dashboard has been served), flat under query load (trims
  slightly rather than growing), and no swap under long uptime. This keeps
  minimoni at a low steady-state footprint on a Pi 3B.
- Tiny virtual footprint (~3 MB VmPeak) and full-image ASLR from static-PIE.

**Negative**

- About 10 to 30 ms extra latency per `/api/metrics` request from musl's slower
  stdlib. Accepted: negligible for the request volume, and far smaller than the
  tiered-read speedup.
- Release builds must run in a musl (Alpine) container. Any build-path tool must
  therefore be available for musl, or be a no-op when absent (the same
  constraint ADR-0007 already imposes). glibc-only prebuilt tools cannot enter
  the release path.

**Neutral / follow-on**

- The earlier reading of glibc "self-trimming" under some query patterns was an
  artifact of arena heuristics happening to release on that workload, not a
  reliable behavior. The musl profile is the reference for what minimoni's
  memory use should look like.
- A glibc `malloc_trim(0)` based workaround was not pursued: the toolchain switch
  solves the problem outright and buys the size and ASLR wins on top, so keeping
  a glibc path was not worth the maintenance.

## References

- [ADR-0005](0005-tiered-consolidation.md): tiered write-time consolidation (the
  read-path latency win that absorbs musl's per-call cost).
- [ADR-0006](0006-minimoni-migrate.md): separate `minimoni-migrate` binary (its
  ~66 KB musl-static target is met by this toolchain).
- [ADR-0007](0007-html-minification.md): Alpine/musl release container and the
  "single static binary or no-op" build-tool constraint.
- [musl libc](https://musl.libc.org/): the C library used for release builds.
- [glibc malloc arenas (mallopt)](https://man7.org/linux/man-pages/man3/mallopt.3.html):
  background on the per-thread arena retention behavior measured above.
