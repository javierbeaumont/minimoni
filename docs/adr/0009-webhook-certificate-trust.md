# ADR-0009: Certificate trust for webhook delivery

**Date:** 2026-10-09
**Status:** Accepted

## Context

[ADR-0004](0004-bearssl.md) delivers HTTPS webhooks with transport encryption and no certificate
verification, on the grounds that the operator configures the endpoint. That covers an endpoint the
operator controls, but no endpoint is authenticated. An `https://` URL reads as a request for a
secure channel, and nothing in `config.toml` shows that it is sent unverified.

Authenticating the endpoint needs trust anchors, which minimoni does not have, and they must be
acquired without giving up a single static binary with no runtime dependencies.

## Decision

**minimoni never delivers an unauthenticated webhook implicitly.** When the endpoint's identity
cannot be established, the delivery is refused and the reason logged, unless the operator has
recorded in `config.toml` that they accept an unverified endpoint. Without that record, a verified
and an unverified `https://` webhook would look the same in `config.toml`.

Trust anchors come from one of two sources, in this order of precedence:

1. **A CA certificate named by the operator** in the configuration. This covers an endpoint signed
   by a private authority.
2. **The host's system trust store**, when the operator names none.

When neither yields anchors, which is the case on a minimal container image that ships no store, the
delivery is refused rather than sent unauthenticated.

The opt-out is an explicit setting, off by default. Plain `http://` is not affected, since it does
not request a secure channel.

**The opt-out overrides trust, not validity.** An expired certificate is still refused. The two are
different claims: "untrusted" says minimoni cannot tie the certificate to an authority it knows,
which the operator's own judgement can replace; "expired" says the issuer declared the credential
invalid after a date, which the operator's judgement does not change. The endpoint is the operator's
own, so an expiry is a fixable problem, and refusing reports it with the engine's error code.

Both settings are global rather than per alert.

## Why the system store rather than an embedded bundle

Embedding a CA bundle in the binary was considered and rejected. It would be large next to the
binary, and it goes stale: every CA rotation would require a new minimoni release.

Reading the host's store keeps the binary free of runtime library dependencies, since opening a file
is not one. The cost is that the store's location varies between distributions, so the path has to
be probed rather than assumed, and that a container image without one falls back to refusing.

## Consequences

**Positive**

- On an ordinary host, an endpoint with a publicly signed certificate is verified with no
  configuration at all.
- Unauthenticated delivery is a recorded choice in `config.toml`, auditable by anyone reading the
  file, rather than an invisible property of the binary.

**Negative**

- An operator whose endpoint cannot be verified has to name a CA or record the opt-out before
  anything is delivered there; such a configuration is no longer accepted silently.
- Under the opt-out the endpoint's identity is unproven, so an attacker able to redirect the
  connection can read and alter the alert payload. Acceptable for outbound alert delivery to an
  operator-controlled endpoint; not acceptable for any inbound or authentication use.
- Behaviour now depends on the host: the same configuration verifies on a distribution that ships a
  trust store and refuses on a container image that does not.

## References

- [ADR-0004](0004-bearssl.md): the choice of BearSSL, whose verification decision this supersedes in
  part.
