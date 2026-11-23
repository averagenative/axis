# Vendored web assets

These two files are the only JavaScript in Axis. They are third-party
code, checked in unchanged and compiled into the `axisd` binary (see
`data/web/` and the Makefile); nothing is fetched at build or run time.

| File | Upstream | Version | SHA-256 |
|------|----------|---------|---------|
| `htmx.min.js` | https://htmx.org (htmx) | 2.0.4 | `e209dda5c8235479f3166defc7750e1dbcd5a5c1808b7792fc2e6733768fb447` |
| `htmx-ext-sse.js` | `sse.js` of the npm package `htmx-ext-sse`, built from https://github.com/bigskysoftware/htmx-extensions (`src/sse/sse.js`) | 2.2.3 | `83eca6fa0611fe2b0bf1700b424b88b5eced38ef448ef9760a2ea08fbc875611` |

## Where they came from

Both files are upstream release files, unchanged.

- `htmx.min.js` identifies itself: the file contains `version:"2.0.4"`.
- `htmx-ext-sse.js` carries no version string. It is byte for byte the
  `sse.js` that the npm package `htmx-ext-sse` ships in its releases 2.2.3
  and 2.2.2, which hold the same file (compared by SHA-256 with the
  published packages). Releases 2.2.0, 2.2.1 and 2.2.4 differ.

## Licence

htmx is released under the Zero-Clause BSD licence, copyright Big Sky
Software; the text is in `LICENSE-htmx`.

The SSE extension is released under the Zero-Clause BSD licence too.
`LICENSE-htmx-ext-sse` is the `LICENSE` file of the npm package
`htmx-ext-sse` 2.2.3, unchanged, with the copyright line it carries there.

## Updating

Replace the file, update the table above (version and checksum), and run
`sha256sum data/web/vendor/*.js`. The asset version in the URLs changes
by itself at the next build.

Files in this directory named `README.md` or `LICENSE*` are documentation
and are not compiled into the binary.
