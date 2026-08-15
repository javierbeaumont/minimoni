# ADR-0002: civetweb as the embedded HTTP server

**Date:** 2026-06-01
**Status:** Accepted

## Context

minimoni needs an embedded HTTP server to serve the dashboard and JSON API. It must be
vendorable as a simple file copy (`.c` + `.h`), GPLv3+ compatible, and allow compile-time
stripping of unused features to keep the binary small.

## Alternatives considered

| Option             | Reason rejected                                                            |
|--------------------|----------------------------------------------------------------------------|
| mongoose           | `GPL-2.0-only`: incompatible with GPLv3+ (see below)                       |
| libmicrohttpd      | LGPL; ~95 source files, requires autotools (not vendorable as a file copy) |
| Custom HTTP parser | ~500+ lines for keep-alive, chunked encoding, concurrent connections       |

**On mongoose incompatibility:** mongoose is licensed `GPL-2.0-only`. GPLv3+ adds clauses
(anti-tivoization, explicit patent grant) that GPLv2-only treats as "further restrictions",
making the two licenses impossible to satisfy simultaneously in a combined work.

## Decision

Use **civetweb** (MIT). It is the MIT-licensed continuation of the original mongoose
codebase, maintained by the same original author (Sergey Lyubka), with 15+ years of
battle-tested HTTP handling. Vendored as `civetweb.c` + `civetweb.h` + `.inl` files.

Everything the daemon does not use is compiled out: TLS, CGI, websockets, IPv6, and serving
files from disk. What remains answers only the routes minimoni registers itself. The exact
flags live in the Makefile.

## Consequences

- No TLS: a reverse proxy (nginx, Caddy) is required for HTTPS.
- No IPv6 in v1.
- No static file serving: civetweb's document-root handling is not compiled in, so flaws in
  that path cannot be reached from this build. Re-evaluate if file serving is ever added.
- ~16 KB stack per connection thread.
- Compile-time flags significantly reduce binary contribution vs. the full civetweb build.
