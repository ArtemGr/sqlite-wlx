#!/bin/sh
# Downloads the third-party sources the plugin is built from (nothing of theirs is kept in git):
#   SQLite amalgamation -> include/sqlite3.c, include/sqlite3.h
#   zstd release        -> deps/zstd/ (lib, contrib/seekable_format; programs/ = the zstd CLI, used by the tests)
# Override versions with e.g.  SQLITE_YEAR=2025 SQLITE_VERSION=3500400 ./fetch-deps.sh
# (pinned checksums are only checked for the default versions).
set -eu
cd "$(dirname "$0")"

SQLITE_YEAR=${SQLITE_YEAR:-2025}
SQLITE_VERSION=${SQLITE_VERSION:-3500200}       # 3.50.2
ZSTD_VERSION=${ZSTD_VERSION:-1.5.7}

# sha256 of the amalgamation's sqlite3.c / sqlite3.h for 3.50.2, and of the zstd 1.5.7 release tarball
SQLITE_C_SHA256=c9a0b6829b81d5f1b78392181f09744c818117a725667411d517b98149fcd3be
SQLITE_H_SHA256=7db44ac3e95c465c30eea8d45d81474c7cda49ab76e8a681e752e535d1560f2e
ZSTD_SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3

check() { # file expected-sha256 default-version-flag
  [ "$3" = 1 ] || { echo "  (non-default version, $1 not checksum-verified)"; return 0; }
  echo "$2  $1" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; exit 1; }
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "SQLite $SQLITE_VERSION"
curl -fsSL -o "$tmp/sqlite.zip" "https://www.sqlite.org/$SQLITE_YEAR/sqlite-amalgamation-$SQLITE_VERSION.zip"
unzip -q -j -o "$tmp/sqlite.zip" "*/sqlite3.c" "*/sqlite3.h" -d "$tmp"
dflt=0; [ "$SQLITE_VERSION" = 3500200 ] && dflt=1
check "$tmp/sqlite3.c" $SQLITE_C_SHA256 $dflt
check "$tmp/sqlite3.h" $SQLITE_H_SHA256 $dflt
mkdir -p include
mv "$tmp/sqlite3.c" "$tmp/sqlite3.h" include/

echo "zstd $ZSTD_VERSION"
curl -fsSL -o "$tmp/zstd.tar.gz" "https://github.com/facebook/zstd/releases/download/v$ZSTD_VERSION/zstd-$ZSTD_VERSION.tar.gz"
dflt=0; [ "$ZSTD_VERSION" = 1.5.7 ] && dflt=1
check "$tmp/zstd.tar.gz" $ZSTD_SHA256 $dflt
rm -rf deps/zstd && mkdir -p deps/zstd
tar -xzf "$tmp/zstd.tar.gz" -C deps/zstd --strip-components=1 \
  "zstd-$ZSTD_VERSION/LICENSE" "zstd-$ZSTD_VERSION/COPYING" "zstd-$ZSTD_VERSION/lib" \
  "zstd-$ZSTD_VERSION/contrib/seekable_format" "zstd-$ZSTD_VERSION/programs"
echo "done"
