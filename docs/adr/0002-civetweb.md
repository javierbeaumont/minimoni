# ADR-0002: civetweb as the embedded HTTP server

**Date:** 2026-05-18
**Status:** Accepted

## Context

minimoni needs an embedded HTTP server to serve the dashboard and JSON API. It must be
vendorable as a simple file copy (`.c` + `.h`), GPLv3+ compatible, and allow compile-time
stripping of unused features to keep the binary small.

## Alternatives considered

| Option | Reason rejected |
|---|---|
| mongoose | `GPL-2.0-only` — incompatible with GPLv3+ (see below) |
| libmicrohttpd | LGPL; ~95 source files, requires autotools — not vendorable as a file copy |
| Custom HTTP parser | ~500+ lines for keep-alive, chunked encoding, concurrent connections |

**On mongoose incompatibility:** mongoose is licensed `GPL-2.0-only`. GPLv3+ adds clauses
(anti-tivoization, explicit patent grant) that GPLv2-only treats as "further restrictions",
making the two licenses impossible to satisfy simultaneously in a combined work.

## Decision

Use **civetweb 1.16** (MIT). It is the MIT-licensed continuation of the original mongoose
codebase, maintained by the same original author (Sergey Lyubka), with 15+ years of
battle-tested HTTP handling. Vendored as `civetweb.c` + `civetweb.h` + `.inl` files.

Compile-time flags strip all unused features:

```
-DNO_SSL -DNO_CGI -DNO_CACHING -DUSE_WEBSOCKET=0 -DUSE_IPV6=0 -DNO_FILES -DNDEBUG
```

## Consequences

- No TLS — a reverse proxy (nginx, Caddy) is required for HTTPS.
- No IPv6 in v1.
- ~16 KB stack per connection thread.
- Compile-time flags significantly reduce binary contribution vs. the full civetweb build.
