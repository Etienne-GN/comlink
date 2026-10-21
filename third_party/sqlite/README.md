# SQLite amalgamation, vendored

`sqlite3.c` and `sqlite3.h`, for the Android cross-build only. The
Debian build links the system `libsqlite3`; Android has no such
package, and the NDK does not expose the platform's own SQLite.

| | |
|---|---|
| Version | 3.53.4 |
| Source | https://sqlite.org/2026/sqlite-amalgamation-3530400.zip |
| Fetched | 2026-09-22 |
| sha256 of `sqlite3.c` | `b1dd5d74ec7f2905...` (first 16, see git) |

**Chosen to match the desktop exactly.** `sqlite3 --version` on the
build machine reports 3.53.4, so Android and Debian run the same
SQLite rather than two that might disagree about a migration.

**Why the amalgamation rather than a submodule.** Building SQLite from
its git repository requires tclsh to *generate* this file. The
amalgamation is the form SQLite publishes for embedding, so a tcl
build-dependency would be work to produce something already published.

**Updating it.** Match the system version again, fetch the
corresponding amalgamation, and update the table above. A vendored
blob whose provenance is not written down is one nobody dares touch.
