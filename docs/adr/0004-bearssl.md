# ADR-0004: BearSSL for HTTPS webhook delivery

**Date:** 2026-06-05
**Status:** Accepted

## Context

minimoni fires alerts by POSTing JSON to operator-configured webhook URLs.
Webhook services almost universally require HTTPS.
HTTP-only delivery is rejected by these services or silently discarded by reverse proxies.

The binary must remain a single static binary with zero runtime dependencies. Linking
against the system's OpenSSL or accepting a plain-HTTP-only implementation are both
non-options.

## Alternatives considered

| Option                 | Reason rejected                                      |
|------------------------|------------------------------------------------------|
| HTTP only              | Rejected by webhook endpoints; plaintext on the wire |
| OpenSSL (system)       | Runtime dependency; absent on minimal/cross images   |
| mbedTLS                | ~300 KB; needs a platform config header; APACHE-2.0  |
| wolfSSL                | ~100 KB; needs build flags + a generated header      |
| Implement TLS manually | Not feasible; TLS 1.2/1.3 is ~2000 lines of crypto   |

**On mbedTLS:** It is ~300 KB compiled and requires a platform configuration header;
APACHE-2.0 is compatible with GPLv3+, but it carries the largest footprint of the options.

**On wolfSSL:** It is ~100 KB compiled and GPLv3 (compatible with GPLv3+), but it requires
`--enable-*` flags and a generated `user_settings.h`; BearSSL is smaller (~64 KB) and vendors
as a self-contained static archive.

## Decision

Vendor **BearSSL 0.6** (MIT) in `vendor/bearssl/`. BearSSL is built as a static archive
(`libbearssl.a`) via its own Makefile as a prerequisite step; it cannot be amalgamated
into a single `.c` file because it ships multiple independent implementations of the same
algorithms (e.g. `ec_prime_i15.c` and `ec_prime_i31.c` both define `static api_generator`)
that are designed to be compiled as separate translation units and dead-code-stripped by
the linker.

Certificate verification is intentionally skipped via a no-op `br_x509_class` vtable.
Webhook URLs are operator-configured in `config.toml`; the operator controls the endpoint.
Transport encryption (confidentiality and integrity of the alert payload in transit) is
provided without the operational burden of bundling a CA trust store on a constrained device.

## Consequences

- Vendor binary contribution: ~64 KB stripped (eliminated almost entirely by `--gc-sections`
  for the plain-HTTP path; active when at least one `webhook = "https://..."` is configured).
- `vendor/bearssl/build/` is generated at build time and gitignored.
- `BR_SSL_BUFSIZE_BIDI` (~32.5 KB) is allocated on the stack inside `post_webhook()`,
  called at most once per alert firing, never in the hot collect path.
- No certificate verification. Acceptable for outbound alert delivery; not acceptable for
  any inbound or authentication use case.
