# Contributing to minimoni

Thanks for considering a contribution.

## Scope

The project's core constraint is **zero runtime dependencies and minimal
resource use** on systems as small as a Raspberry Pi Zero. Any change that
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
- Keep commits small and focused. Format: `<module>: <imperative>` subject
  with descriptive body paragraphs.
- Code must compile cleanly with `-Wall -Wextra` and pass `make`.
- Follow the existing K&R/Linux style (4-space indent, 100-col limit).
  A `.clang-format` is provided.
- Sign your commits if possible (`git commit -S`).

## Building and testing

```sh
make embed     # bundle dashboard into src/embed.h
make           # development build (-O2)
make release   # release build (-Os -flto, stripped)
```

For release-equivalent binaries identical to the published ones, use the
Alpine Docker target:

```sh
make release-linux
```
