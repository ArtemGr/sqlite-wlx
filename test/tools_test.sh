#!/bin/sh
# End-to-end tests of the command-line paths for making a compressed database:
#   1. sqlzst (snapshot via VACUUM INTO, and --raw)
#   2. the stock-CLI recipe: split + zstd per chunk + zstindex
# usage: tools_test.sh <dir with vfs_test, sqlzst, zstindex, zstd> <scratch dir>
set -eu
BIN=$1; T=$2
VT="$BIN/vfs_test"
pass=0; fail=0
ok()   { pass=$((pass + 1)); }
bad()  { fail=$((fail + 1)); echo "FAIL tools_test: $*"; }
same() { [ "$1" = "$2" ] && ok || bad "$3: got '$1', want '$2'"; }

q() { "$VT" query "$@"; }
check_db() { # file vfs label
  same "$(q "$1" "$2" "select count(*) from t")" "$ROWS" "$3: row count"
  same "$(q "$1" "$2" "$SUM")" "$WANT" "$3: content"
  same "$(q "$1" "$2" "pragma integrity_check")" "ok" "$3: integrity_check"
}

echo "tools"
ROWS=3000
SUM="select sum(id) + sum(length(v)) from t"
"$VT" mkdb "$T/src.db" delete $ROWS
WANT=$(q "$T/src.db" - "$SUM")

# --- sqlzst ---
"$BIN/sqlzst" "$T/src.db" "$T/sqlzst.db.zst" --frame-pages 4 --level 3 > /dev/null
check_db "$T/sqlzst.db.zst" zstd "sqlzst"
"$BIN/sqlzst" "$T/src.db" "$T/sqlzst-raw.db.zst" --frame-pages 2 --level 3 --raw > /dev/null
check_db "$T/sqlzst-raw.db.zst" zstd "sqlzst --raw"
"$BIN/zstd" -d -q -c "$T/sqlzst-raw.db.zst" | cmp -s - "$T/src.db" && ok || bad "sqlzst --raw: zstd -d does not restore the original"

# --- split + zstd CLI + zstindex (the recipe from the prototype's README) ---
mkdir -p "$T/split" && rm -f "$T/split/"*
PAGE=$(q "$T/src.db" - "pragma page_size")
same "$PAGE" 16384 "test database page size"
( cd "$T/split" && split -b $((PAGE * 4)) -d -a 6 ../src.db chunk. )
for c in "$T/split"/chunk.*; do "$BIN/zstd" -19 -q -c "$c"; done > "$T/split.db.zst"
nchunks=$(ls "$T/split"/chunk.* | wc -l | tr -d ' ')
case "$(q "$T/split.db.zst" zstd "select count(*) from sqlite_master" || true)" in error*) ok ;; *) bad "split: opened before zstindex added the seek table" ;; esac
"$BIN/zstindex" "$T/split.db.zst" > "$T/zstindex.out"
grep -q "indexed $nchunks frames" "$T/zstindex.out" && ok || bad "zstindex: expected $nchunks frames, got: $(cat "$T/zstindex.out")"
check_db "$T/split.db.zst" zstd "split + zstd + zstindex"
"$BIN/zstd" -d -q -c "$T/split.db.zst" | cmp -s - "$T/src.db" && ok || bad "split: zstd -d does not restore the original"
if "$BIN/zstindex" "$T/split.db.zst" > /dev/null 2>&1; then bad "zstindex accepted an already-indexed file"; else ok; fi

# frames without a content size (zstd reading stdin) must be rejected, with --stream-size accepted
first=$(ls "$T/split"/chunk.* | head -n 1)
"$BIN/zstd" -q -c < "$first" > "$T/stdin.zst"
if "$BIN/zstindex" "$T/stdin.zst" > /dev/null 2>&1; then bad "zstindex accepted a frame without content size"; else ok; fi
"$BIN/zstd" -q -c --stream-size=$(wc -c < "$first" | tr -d ' ') < "$first" > "$T/stdin-sized.zst"
"$BIN/zstindex" "$T/stdin-sized.zst" > /dev/null && ok || bad "zstindex rejected a frame made with --stream-size"

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
