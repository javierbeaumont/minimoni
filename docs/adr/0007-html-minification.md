# ADR-0007: Optional HTML/CSS/JS minification

**Date:** 2026-07-10
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
  binary: no Node.js, no npm, no Python runtime. The same constraint we
  apply to minimoni itself applies to its build path.
- **Same Alpine container as the rest of the release build.** Whatever
  tool we pick must be installable with `apk add` or one `curl | tar`
  in the CI workflow, no exotic setup.

A naive solution would have been to require `terser`+`csso` via Node, but
that breaks the constraint. We pick from tools that produce, or can be
distributed as, a single static binary.

## Candidates considered

| Tool                      | Lang | Formats                       | Standalone binary | Licence |
|---------------------------|------|-------------------------------|-------------------|---------|
| **esbuild**               | Go   | JS, CSS                       | yes (prebuilt)    | MIT     |
| **tdewolff/minify**       | Go   | HTML+CSS+JS+JSON+SVG+XML      | yes (prebuilt)    | MIT     |
| **minify-html (minhtml)** | Rust | HTML + inline JS + inline CSS | yes (prebuilt)    | MIT     |

Notes per candidate:

- **esbuild**: no HTML mode, so it minifies JS and CSS separately and
  `bundle.sh` inlines the minified files.
- **tdewolff/minify**: minifies the final inlined HTML in one pass,
  re-minifying the CSS and JS inside the `<style>`/`<script>` blocks too.
- **minify-html (minhtml)**: marketed specifically for "minify HTML with
  inline JS+CSS"; internally uses oxc for the JS.

Rejected because they require Node.js / npm in the build path:

- **swc**: distributed as npm package; CLI exists but tied to Node toolchain.
- **oxc-minify**: no standalone CLI; only available as a library / npm package.
- **terser**, **UglifyJS**: JS-runtime-only.

Rejected because they don't fit our build flow:

- **html-minifier-terser**, **htmlnano**: Node.js; same constraint.
- **Closure Compiler**: Java runtime; heavy.

## Comparison setup

Four pipelines, baseline included as reference. All produce a final
`dashboard/index.html` that `xxd -i` then embeds into `build/embed.h`.

1. **Baseline**: `bundle.sh` inlines CSS+JS as-is (current behaviour).
2. **esbuild**: `esbuild --minify` on each of `style.css` and `app.js`;
   `bundle.sh` inlines the minified versions.
3. **tdewolff minify**: `bundle.sh` inlines as-is; pipe the result
   through `minify --type=html`. CSS/JS inside the inlined blocks are
   re-minified by the HTML pass.
4. **minify-html (minhtml)**: `bundle.sh` inlines as-is; pipe through
   `minhtml --minify-css --minify-js`.

Measured:

- Bytes of the final inlined HTML (before `xxd -i`) for each pipeline.
- Stripped `minimoni` binary after `make release`, for the baseline and the
  chosen pipeline (`xxd -i`'s framing is constant, so the binary delta
  equals the HTML delta).

## Measurements

Re-run on the current v0.2 dashboard (`index.html`, `style.css`, `app.js`)
inside the pinned Alpine 3.24 musl release container (`tools/Dockerfile`),
with the tools the release path uses: esbuild 0.27.1 and minify 2.24.11, both
`apk`-installed. Each pipeline produces the final inlined HTML; `xxd -i` embeds
it into `build/embed.h` and `make release` builds the canonical
`-Os -flto -static` musl-PIE binary (ADR-0008), strip included.

| Pipeline                        | HTML bytes | vs baseline |
|---------------------------------|------------|-------------|
| baseline (no minify)            | 77,599     | 100.0%      |
| minify-html (minhtml)           | 49,992     | 64.4%       |
| esbuild (JS+CSS separate)       | 39,198     | 50.5%       |
| **tdewolff/minify (HTML pass)** | 35,072     | 45.2%       |

tdewolff produces the smallest output: it minifies the whole inlined
document in one pass (the HTML wrapper plus the CSS and JS inside
`<style>`/`<script>`), whereas esbuild minifies only the CSS and JS and
leaves the ~11 KB HTML wrapper untouched. minify-html compresses the least
of the three (largest output): it is measured here with the
`@minify-html/node` 0.18.1 package, since its binary cannot run in the musl
release container (see Decision).

Effect on the shipped binary, baseline vs the chosen tdewolff pipeline:

| Build                    | Binary bytes | Delta              |
|--------------------------|--------------|--------------------|
| baseline (no minify)     | 1,272,464    | n/a                |
| tdewolff (HTML pass)     | 1,229,936    | -42,528 B (-3.34%) |

The binary delta (-42,528 B) matches the HTML delta (-42,527 B) to within
a byte: `xxd -i`'s framing overhead is constant, so the binary saving is
exactly the HTML saving. The minify step adds a negligible fraction of the
build time, dwarfed by the LTO and strip steps.

## Decision

**Use [tdewolff/minify](https://github.com/tdewolff/minify).** It wins on
every axis we measured:

- Smallest output of the viable tools (35,072 B vs 39,198 for esbuild).
- Adds negligible build time, dwarfed by the LTO and strip steps.
- Simplest integration: one tool, one pass. `bundle.sh` produces the
  inlined HTML as today, then `minify --type=html` operates on the
  result and re-minifies the embedded `<script>` and `<style>` blocks
  in the same call.
- Available in the Alpine release container via `apk add minify`. No
  second source of binaries, no `curl | tar` step in CI.
- Broader format support (HTML+CSS+JS+JSON+SVG+XML) leaves room for
  future use (e.g., minifying the SVG favicon).

minify-html is ruled out structurally: its prebuilt binary is glibc-only
(`linux-gnu`) and would not run in our musl Alpine release container without
rebuilding from source, so it cannot enter the release path. It also produced
the largest output of the three (49,992 B); its JS minifier, `oxc_minifier`
since v0.17.0 (2025), compressed this dashboard's JS less than tdewolff and
esbuild did.

esbuild is a respectable runner-up (same Alpine availability), but it
minifies only the CSS and JS and leaves the HTML wrapper untouched, so it
takes two invocations and still produces a larger result than tdewolff's
single HTML pass.

## Consequences

**Positive**

- ~42 KB lighter binary in release builds (3.3% reduction); embedded
  HTML drops to ~45% of its un-minified size.
- Single Alpine package, no curl-from-GitHub steps in CI.
- Same tool covers future static-asset minification needs (SVG, etc.).

**Negative**

- Adds `minify` as a CI-time dependency. Tolerated because (a) it is
  packaged by Alpine, (b) it is a no-op locally when absent.
- Slightly slower local `make release` (the minify step adds negligible
  time), well inside the noise of LTO and strip already there.

**Implementation outline**

- `tools/bundle.sh` gains a `MINIFY=1` env-var branch: if the variable
  is set AND `minify` is on `PATH`, the script pipes its final inlined
  HTML through `minify --type=html` before printing. If `MINIFY=1` is
  set but `minify` is missing, the script emits a single-line warning
  to stderr and continues with the unminified output (no-op).
- The `Makefile` `release` target sets `MINIFY=1` (propagated to its
  `embed` prerequisite); `release-linux` builds `release` inside the
  Alpine container. Plain `make` (development) leaves `MINIFY` unset so
  DevTools shows readable source.
- The release CI workflow adds `minify` to the `apk add` line in the
  Alpine build job. All four arches (amd64 and arm64 native, armv7 and
  armv6 under QEMU) build inside Alpine containers, so a single `apk add`
  line covers every arch: no per-arch special case. `make test`
  (integration) installs it too, so its `make release` step actually
  minifies rather than warning.

## References

- [tdewolff/minify](https://github.com/tdewolff/minify): Go, MIT.
- [wilsonzlin/minify-html](https://github.com/wilsonzlin/minify-html): Rust, MIT.
- [esbuild](https://esbuild.github.io/): Go, MIT.
- [JS minification benchmarks](https://github.com/privatenumber/minification-benchmarks):
  privatenumber, last updated 2026-04-30 (consulted 2026-07-10).
- [HTML minifier comparison](https://github.com/j9t/minifier-benchmarks): j9t, last updated
  2026-07-09 (consulted 2026-07-10).
