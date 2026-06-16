# Contributing to minimoni

Thanks for considering a contribution.

## Scope

The project's core constraint is **zero runtime dependencies and minimal
resource use** on systems as small as a Raspberry Pi Zero 2 W. Any change that
adds a runtime dependency, a build dependency that isn't already vendored,
or a non-trivial RSS / binary-size increase will be rejected.

Feature requests outside the current scope are unlikely to be merged
regardless of quality. Open an issue first to discuss fit before sending
a non-trivial PR.

## Reporting bugs

Use the GitHub issue templates. Include `minimoni --version`, your OS, and
the config (with secrets redacted).

For security-sensitive issues, see [SECURITY.md](SECURITY.md).

## Pull requests

- Discuss in an issue before sending non-trivial PRs.
- Keep commits small and focused. Format: `<module>: <imperative>` subject;
  keep the body minimal: subject-only unless it adds what the diff doesn't show.
- Code must compile cleanly with `-Wall -Wextra` (`make`) and pass `make test`.
- Follow the existing K&R/Linux style (4-space indent, 100-col limit).
  A `.clang-format` is provided.
- Sign your commits if possible (`git commit -S`).

## Building and testing

```sh
make embed     # bundle dashboard into build/embed.h
make           # development build (-O2)
make release   # release build (-Os -flto, stripped)
make test      # run unit tests in Docker
```

For release-equivalent binaries identical to the published ones, use the
Alpine Docker target:

```sh
make release-linux
```

## Linting

Formatting and static checks run through [pre-commit](https://pre-commit.com).
Install the hooks once and they run automatically on every commit (and on push
for the heavier checks):

```sh
pip install pre-commit
pre-commit install --install-hooks
pre-commit install --hook-type pre-push   # clang-tidy runs here
```

To run the checks on demand:

```sh
pre-commit run --all-files   # format, ruff, eslint, shellcheck, gitleaks, ...
make tidy                    # clang-tidy over src/ in Docker
```

## Continuous integration

Every push and pull request to `main` runs three workflows: **Lint and static
analysis** (the pre-commit checks above), **Tests** (`make test`), and **Security
analysis** (CodeQL).

## Dashboard development

To iterate on `dashboard/` without recompiling, run the mock dev server. It
serves the dashboard files directly (no build needed):

```sh
python3 tools/devserver/          # http://localhost:9090
```

It serves `dashboard/index.html` with mocked metrics, taking the presentation
settings (units, theme, charts/cards, etc.) from a real minimoni config so unit
and layout changes show on reload. Requires Python 3.11+ (uses tomllib).

Full options:

```
python3 tools/devserver/ [port] [config] [--scenario {normal,warn,critical,cycle}] [-v]
```

`--scenario` drives the values so the warning/critical states can be exercised:
`cycle` (default) sweeps good -> warning -> critical over time; `warn` and
`critical` pin a band; `normal` stays healthy. `-v` logs every request.
