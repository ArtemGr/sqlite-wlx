**sqlite-wlx** is a [Total Commander](https://www.ghisler.com/) plugin to view SQLite3 files. The plugin has a [standalone](https://github.com/little-brother/sqlite-x) version.

|[**Download the latest version**](https://github.com/little-brother/sqlite-wlx/releases/latest/download/sqlite-wlx.zip)|
|-------------------------------------------------------------------------------------------|

![View](sqlite-wlx.png)

### Features
* Column filters
* Sort data by column click
* Data editing (off by default)
* Doesn't require SQLite library (built-in)
* Supports huge tables
* Blob export

If you have any problems, comments or suggestions, check [Wiki](https://github.com/little-brother/sqlite-wlx/wiki), create [issue](https://github.com/little-brother/sqlite-wlx/issues) or just let me know <a href="mailto:lb.im@ya.ru?subject=sqlite-wlx">lb.im@ya.ru</a>.



### Compressed databases (`.zst`)
The plugin opens SQLite databases compressed in the [zstd seekable format](https://github.com/facebook/zstd/blob/dev/contrib/seekable_format/zstd_seekable_compression_format.md) read-only, with random access (only the frames a query touches are decompressed). A plain single-frame `.zst` is not seekable and is skipped.

**With `sqlzst.exe`** (built by `make`): takes a consistent snapshot with `VACUUM INTO` (works on a live WAL database) and writes frames of exactly N pages.
```
sqlzst SRC.db DST.db.zst [--frame-pages N] [--level L] [--raw]
```
Small frames (1–4 pages) suit point lookups, larger ones compress better and suit scans.

**With `split` and the stock `zstd` CLI**, which can't write a seekable file by itself (`-T#`, `-B#` and `--rsyncable` still produce one frame): compress page-aligned chunks separately, then append a seek table with `zstindex` (`tools/zstindex.c`), which doesn't recompress anything.
```sh
PAGE=$(sqlite3 db.sqlite "pragma page_size")             # chunk size must be a multiple of the page size
split -b $((PAGE * 4)) -d -a 6 db.sqlite chunk.          # 4 pages per frame
for c in chunk.*; do zstd -19 -q -c "$c"; done > db.sqlite.zst
zstindex db.sqlite.zst                                    # appends the seek table in place
```
* The database must be fully checkpointed (no pending `-wal` content), since the file is compressed byte for byte. A WAL-mode header is presented as rollback mode.
* Every frame must record its decompressed size. `zstd` does this when compressing a file; from stdin it needs `--stream-size=N`, otherwise `zstindex` refuses the frame.
* `zstd -d db.sqlite.zst` still restores the original byte for byte (the seek table is a skippable frame).
* `make test` runs this recipe every time, with a `zstd` CLI built from the same zstd release.

### WSL paths
Databases under `\\wsl.localhost\<distro>\...` or `\\wsl$\...` (e.g. `/dev/shm`) are opened read-only through a lock-free VFS (`rovfs.c`): that share supports neither file locks nor shared-memory mapping. A live WAL database is read correctly (the WAL index is rebuilt in memory from `-wal`), and nothing is created, written or deleted next to the database. The same mode is used as a fallback when a normal open fails. It's a snapshot: reopen (F3) to refresh.

### Building
`make` downloads SQLite and zstd (`fetch-deps.sh`, pinned versions and checksums), runs the tests and a small benchmark, then builds `build/sqlite.wlx64`, `build/sqlite.wlx` and `build/sqlzst.exe`. `make zip` adds `build/sqlite-wlx.zip` with `pluginst.inf` for installing from Total Commander.

Cross-compiling on Linux worked on Ubuntu 24.04 with the `mingw-w64` package (`apt install mingw-w64`): GCC 13.2 (`x86_64-w64-mingw32-gcc` / `i686-w64-mingw32-gcc`, the default `win32` thread-model variants) and mingw-w64 runtime 11.0.1. Also needed: `make`, `curl`, `unzip`, `zip`, and a native `cc` for the tests. Natively on Windows, MSYS2 with the MinGW-w64 toolchains should work the same way (not tested).
