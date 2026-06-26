# SQLite amalgamation (vendored)

These files are the official SQLite **amalgamation** — the entire SQLite C
library as a single compilation unit. They are committed directly (not a git
submodule) because the amalgamation is a generated artifact with no canonical
git source; SQLite distributes it as a zip from sqlite.org.

- **Version:** 3.53.2 (`SQLITE_VERSION` `3.53.2`)
- **Source:** https://www.sqlite.org/2026/sqlite-amalgamation-3530200.zip
- **SHA3-256 (zip):** `81142986038e18f96c4a54e1a72562ae17e502a916f2a7701eff43388cbf1a40`
- **License:** Public Domain (https://www.sqlite.org/copyright.html)

Files:
- `sqlite3.c` — the amalgamated library (compiled only into the gated
  `culebra_rt_sqlite` feature archive, never into the core build).
- `sqlite3.h` — the public API header.
- `sqlite3ext.h` — loadable-extension header (kept for completeness).

## Updating

Download a newer amalgamation from https://www.sqlite.org/download.html,
verify its SHA3-256 against the `PRODUCT,` line on that page, and replace the
files here. Bump the version/hash above.
