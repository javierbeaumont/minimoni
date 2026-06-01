# minimoni

Zero-dependency system monitoring in a single C binary. Collects CPU, memory, disk, temperature, and network metrics
into SQLite and serves an interactive canvas dashboard.

Designed for resource-constrained Linux systems (small VPS, single-board computers,
and homelab servers) where every MB counts.

## Architecture

```
minimoni serve   -->  collect metrics  -->  SQLite  -->  HTTP server  -->  dashboard :8080
minimoni collect -->  collect metrics  -->  SQLite     (oneshot — for systemd timer / cron)
```

Metrics are read from `/proc/` and `/sys/`. The dashboard HTML is embedded in the binary at build time.

## Vendored dependencies

All three compile directly into the binary — no runtime dependencies, no package manager.

| Library | Version | Purpose | License |
|---|---|---|---|
| [SQLite](https://www.sqlite.org/) | 3.53.1 | Metric storage (single-file database) | Public domain |
| [civetweb](https://github.com/civetweb/civetweb) | 1.16 | Embedded HTTP server — dashboard + JSON API | MIT |
| [tomlc17](https://github.com/cktan/tomlc17) | R260517 | TOML configuration parser | MIT |
