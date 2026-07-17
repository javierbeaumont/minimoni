**v0.2.0 (Eddystone)**

*First offshore lighthouse (1698). Built to survive storms.*

Production-ready release: minimoni now runs on all ARM hardware, installs as a system
service, and stays fast at every retention range.

**Highlights:**
- **Tiered write-time consolidation**: long-range queries (30d/90d) stay flat in the hundreds
  of ms instead of scaling to seconds, roughly 5-7x faster than v0.1 on a Pi 3B, by
  consolidating rows progressively at write time (see ADR-0005).
- **Dashboard redesign**: Radix colour palette and design tokens.
- **Four architectures**: `linux-amd64`, `linux-arm64`, `linux-armv7`, `linux-armv6`,
  each a static musl-PIE binary of ~1.24 MB with zero runtime dependencies and a flat,
  low memory profile (see ADR-0008 for musl-static-PIE vs glibc).
- **`minimoni-migrate`**: migrates a v0.1 database to the v0.2 schema, with a dry-run mode and
  an automatic pre-migration backup (see ADR-0006).
- systemd unit files, reverse-proxy guides (Caddy, nginx, Tailscale, Traefik, Apache), and a
  config-validation test suite.

**Supply chain:** every release ships a SHA-256 `checksums.txt`, a CycloneDX SBOM, and a
signed build-provenance attestation (SLSA Build Level 2); releases are immutable.

**Upgrading from v0.1:** the database schema changed. Stop the service, run `minimoni-migrate`
on the database, then start it again. See the
[README](https://github.com/javierbeaumont/minimoni#upgrading).

**Quick start:**
```sh
ARCH=$(uname -m)
case $ARCH in
  x86_64)  ARCH=amd64 ;;
  aarch64) ARCH=arm64 ;;
  armv7l)  ARCH=armv7 ;;
  armv6l)  ARCH=armv6 ;;
esac

BASE=https://github.com/javierbeaumont/minimoni/releases/latest/download
curl -fsSL $BASE/minimoni-linux-$ARCH -o /usr/local/bin/minimoni
chmod +x /usr/local/bin/minimoni

minimoni serve
```

Full documentation in the [README](https://github.com/javierbeaumont/minimoni#readme).
