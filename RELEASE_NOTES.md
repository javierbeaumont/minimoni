**v0.1.0 — Pharos**

*The Lighthouse of Alexandria. Where it all begins.*

First public release.

minimoni is a zero-dependency system monitoring tool: a single static binary (~2 MB) that
collects CPU, memory, disk, temperature, and network metrics into SQLite and serves an
interactive canvas dashboard.

**What's included:**
- `linux-amd64` and `linux-arm64` prebuilt static binaries
- `config.example.toml` — annotated reference configuration

**Quick start:**
```sh
ARCH=$(uname -m)
case $ARCH in x86_64) ARCH=amd64 ;; aarch64) ARCH=arm64 ;; esac
curl -fsSL https://github.com/javierbeaumont/minimoni/releases/latest/download/minimoni-linux-$ARCH \
  -o /usr/local/bin/minimoni
chmod +x /usr/local/bin/minimoni
minimoni serve
```

Full documentation in the [README](https://github.com/javierbeaumont/minimoni#readme).
