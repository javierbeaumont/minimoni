# ADR-0007: Optional HTML/CSS/JS minification

**Date:** 2026-05-28
**Status:** Accepted

## Context

The dashboard is embedded in the minimoni binary via `xxd -i` (see
`Makefile:embed` target and `tools/bundle.sh`). The bundled HTML carries
`<style>` and `<script>` blocks inlined verbatim from `dashboard/style.css`
and `dashboard/app.js`. Bytes that ship in the binary are bytes the user
downloads on every release.

v0.2 introduces an opt-in minification step (`MINIFY=1 make release`) that
shrinks the embedded HTML and is a no-op when the chosen tool is not on
`PATH`. Two hard constraints follow from minimoni's project promise:

- **No runtime dependencies.** The tool must distribute as a standalone
  binary — no Node.js, no npm, no Python runtime. The same constraint we
  apply to minimoni itself applies to its build path.
- **Same Alpine container as the rest of the release build.** Whatever
  tool we pick must be installable with `apk add` or one `curl | tar`
  in the CI workflow, no exotic setup.

A naïve solution would have been to require `terser`+`csso` via Node, but
that breaks the constraint. We pick from tools that produce, or can be
distributed as, a single static binary.

## Candidates considered

| Tool                  | Lang | Formats                       | Standalone binary | Licence | Notes                                                                                 |
|-----------------------|------|-------------------------------|-------------------|---------|---------------------------------------------------------------------------------------|
| **esbuild**           | Go   | JS, CSS                       | yes (prebuilt)    | MIT     | No HTML — minify JS and CSS separately, then `bundle.sh` inlines.                     |
| **tdewolff/minify**   | Go   | HTML+CSS+JS+JSON+SVG+XML      | yes (prebuilt)    | MIT     | Can minify the final inlined HTML in one pass; CSS+JS inside `<style>`/`<script>` too.|
| **minify-html (minhtml)** | Rust | HTML + inline JS + inline CSS | yes (prebuilt)    | MIT     | Specifically optimised for "minify HTML with inline JS+CSS"; internally uses oxc for JS.|

Rejected because they require Node.js / npm in the build path:

- **swc** — distributed as npm package; CLI exists but tied to Node toolchain.
- **oxc-minify** — no standalone CLI; only available as a library / npm package.
- **terser**, **UglifyJS** — JS-runtime-only.

Rejected because they don't fit our build flow:

- **html-minifier-terser**, **htmlnano** — Node.js; same constraint.
- **Closure Compiler** — Java runtime; heavy.

## Comparison setup

Three pipelines, baseline included as reference. All produce a final
`dashboard/index.html` that `xxd -i` then embeds into `src/embed.h`.

1. **Baseline** — `bundle.sh` inlines CSS+JS as-is (current behaviour).
2. **esbuild** — `esbuild --minify` on each of `style.css` and `app.js`;
   `bundle.sh` inlines the minified versions.
3. **tdewolff minify** — `bundle.sh` inlines as-is; pipe the result
   through `minify --type=html`. CSS/JS inside the inlined blocks are
   re-minified by the HTML pass.
4. **minify-html (minhtml)** — `bundle.sh` inlines as-is; pipe through
   `minhtml --minify-css --minify-js`.

Measured per pipeline:

- Bytes of the final HTML before `xxd -i`.
- Size of `src/embed.h` after `xxd -i` (the literal bytes shipped).
- Size of the stripped `minimoni` binary after `make release`.
- Wall-clock time of the minify step in CI.

## Measurements

Same `dashboard/index.html`, `style.css` and `app.js` as input. Each
pipeline produces the final inlined HTML; we then `xxd -i` it into
`src/embed.h` and run `make release` (the canonical `-Os -flto -static`
+ strip path). Built inside an Alpine musl arm64 container, the way the
linux-amd64 and linux-arm64 release artifacts are built. Wall-clock
times measured on the same host, mean of 3 runs.

| Pipeline                  | HTML bytes | HTML %      | Binary bytes | Binary delta vs baseline | Minify time |
|---------------------------|------------|-------------|--------------|--------------------------|-------------|
| baseline (no minify)      | 77,686     | 100.0%      | 1,272,552    | —                        | —           |
| esbuild (JS+CSS, separate)| 35,672     | 45.9%       | 1,230,536    | −42,016 B (−3.30%)       | ~7 ms       |
| **tdewolff/minify (HTML pass)** | **32,945** | **42.4%** | **1,227,816** | **−44,736 B (−3.51%)** | **~6 ms**   |
| minify-html (minhtml)     | 47,804     | 61.5%       | 1,242,672    | −29,880 B (−2.35%)       | ~7 ms       |

Binary delta matches HTML delta to within a byte — `xxd -i`'s framing
overhead is constant across runs, so the binary saving is the HTML
saving, no more, no less.

## Decision

**Use [tdewolff/minify](https://github.com/tdewolff/minify).** It wins on
every axis we measured:

- Smallest output (32,945 B vs 35,672 esbuild vs 47,804 minify-html).
- Fastest (~6 ms vs ~7 ms for the other two; difference inside noise).
- Simplest integration: one tool, one pass — `bundle.sh` produces the
  inlined HTML as today, then `minify --type=html` operates on the
  result and re-minifies the embedded `<script>` and `<style>` blocks
  in the same call.
- Available in the Alpine release container via `apk add minify`. No
  second source of binaries, no `curl | tar` step in CI.
- Broader format support (HTML+CSS+JS+JSON+SVG+XML) leaves room for
  future use (e.g., minifying the SVG favicon).

minify-html's loss is notable: despite being marketed for exactly this
case (HTML with inline JS+CSS), it produces the largest output. The
project recently swapped its internal JS minifier to `oxc_minifier`,
which still lacks constant inlining and dead-code removal — features
mature minifiers have had for years. Its prebuilt binary is also
glibc-only (`linux-gnu`); it would not run in our musl Alpine release
container without rebuilding from source. Two strikes.

esbuild is a respectable runner-up — same Alpine availability, slightly
larger output, two-tool integration (run twice: once for CSS, once for
JS, then `bundle.sh` inlines). Not worth the small extra savings vs
tdewolff/minify given the more complex pipeline.

## Consequences

**Positive**

- ~44 KB lighter binary in release builds (3.5% reduction); embedded
  HTML drops to ~42% of its un-minified size.
- Single Alpine package, no curl-from-GitHub steps in CI.
- Same tool covers future static-asset minification needs (SVG, etc.).

**Negative**

- Adds `minify` as a CI-time dependency. Tolerated because (a) it is
  packaged by Alpine, (b) it is a no-op locally when absent.
- Slightly slower local `make release` (the +6 ms minify step) — well
  inside the noise of LTO and strip already in the pipeline.

**Implementation outline**

- `tools/bundle.sh` gains a `MINIFY=1` env-var branch: if the variable
  is set AND `minify` is on `PATH`, the script pipes its final inlined
  HTML through `minify --type=html` before printing. If `MINIFY=1` is
  set but `minify` is missing, the script emits a single-line warning
  to stderr and continues with the unminified output (no-op).
- The `Makefile` `release` and `release-linux` targets pass
  `MINIFY=1` into the `embed` invocation; `make` (development) leaves
  it unset so DevTools shows readable source.
- The release CI workflow adds `minify` to the `apk add` line in the
  Alpine build job. arm32 builds inherit it via the same Alpine
  pattern; arm64 (cross-compile on Ubuntu) installs the prebuilt
  `minify_linux_amd64.tar.gz` from the GitHub release in one curl
  step (Ubuntu does not have an apt package as of 2026).

## References

- [tdewolff/minify](https://github.com/tdewolff/minify) — Go, MIT.
- [wilsonzlin/minify-html](https://github.com/wilsonzlin/minify-html) — Rust, MIT.
- [esbuild](https://esbuild.github.io/) — Go, MIT.
- [JS minification benchmarks (privatenumber, 2026-04-30)](https://github.com/privatenumber/minification-benchmarks).
- [HTML minifier comparison (j9t, 2026-04-07)](https://github.com/j9t/minifier-benchmarks).
