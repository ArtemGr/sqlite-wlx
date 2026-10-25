/*
** sqlzst: compress a SQLite database into the zstd seekable format, frames aligned to pages.
**
**   sqlzst SRC.db DST.db.zst [--frame-pages N] [--level L] [--raw]
**
** --raw compresses SRC byte-for-byte (it must be fully checkpointed, no -wal content);
** the VFS then presents a WAL-mode header as rollback mode.
**
** Takes a consistent snapshot with VACUUM INTO (works on a live WAL database, compacts it,
** and writes a single file with no -wal), then streams it into frames of N pages each, so a
** page never straddles two frames.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "zstd.h"
#include "zstd_seekable.h"

static void die(const char *m, const char *d){ fprintf(stderr, "sqlzst: %s %s\n", m, d ? d : ""); exit(1); }

static int compress_file(const char *src, const char *dst, int pageSize, int framePages, int level, int removeSrc){
  FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
  if( !in || !out ) die("cannot open files", 0);
  unsigned frameSize = (unsigned)(pageSize * framePages);
  ZSTD_seekable_CStream *zcs = ZSTD_seekable_createCStream();
  size_t r = ZSTD_seekable_initCStream(zcs, level, 1 /* per-frame checksums */, frameSize);
  if( ZSTD_isError(r) ) die("initCStream:", ZSTD_getErrorName(r));
  size_t inCap = ZSTD_CStreamInSize(), outCap = ZSTD_CStreamOutSize();
  unsigned char *ib = malloc(inCap), *ob = malloc(outCap);
  unsigned long long nIn = 0, nOut = 0;
  size_t n;
  while( (n = fread(ib, 1, inCap, in)) > 0 ){
    ZSTD_inBuffer ibuf = { ib, n, 0 };
    nIn += n;
    while( ibuf.pos < ibuf.size ){
      ZSTD_outBuffer obuf = { ob, outCap, 0 };
      r = ZSTD_seekable_compressStream(zcs, &obuf, &ibuf);
      if( ZSTD_isError(r) ) die("compress:", ZSTD_getErrorName(r));
      fwrite(ob, 1, obuf.pos, out); nOut += obuf.pos;
    }
  }
  do{
    ZSTD_outBuffer obuf = { ob, outCap, 0 };
    r = ZSTD_seekable_endStream(zcs, &obuf);
    if( ZSTD_isError(r) ) die("endStream:", ZSTD_getErrorName(r));
    fwrite(ob, 1, obuf.pos, out); nOut += obuf.pos;
  }while( r > 0 );
  fclose(in); fclose(out); if( removeSrc ) remove(src);
  ZSTD_seekable_freeCStream(zcs); free(ib); free(ob);
  printf("%s: %llu -> %llu bytes (%.1f%%), page %d B, frame %u B (%d pages), level %d\n",
         dst, nIn, nOut, 100.0 * nOut / (nIn ? nIn : 1), pageSize, frameSize, framePages, level);
  return 0;
}

int main(int argc, char **argv){
  if( argc < 3 ) die("usage: sqlzst SRC.db DST.db.zst [--frame-pages N] [--level L]", 0);
  int framePages = 16, level = 19, raw = 0;
  for(int i=3; i<argc; i++){
    if( !strcmp(argv[i], "--frame-pages") && i+1<argc ) framePages = atoi(argv[++i]);
    else if( !strcmp(argv[i], "--level") && i+1<argc ) level = atoi(argv[++i]);
    else if( !strcmp(argv[i], "--raw") ) raw = 1;
  }
  char tmp[4096]; snprintf(tmp, sizeof tmp, "%s.snapshot.tmp", argv[2]);
  sqlite3 *db;
  if( raw ){
    snprintf(tmp, sizeof tmp, "%s", argv[1]);
    int ps = 4096; unsigned char h[18]; FILE *f = fopen(tmp, "rb");
    if( f && fread(h, 1, 18, f)==18 ){ ps = (h[16]<<8) | h[17]; if( ps==1 ) ps = 65536; }
    if( f ) fclose(f);
    return compress_file(tmp, argv[2], ps, framePages, level, 0);
  }
  remove(tmp);
  if( sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, 0) ) die("cannot open", argv[1]);
  char *sql = sqlite3_mprintf("VACUUM INTO %Q", tmp), *err = 0;
  if( sqlite3_exec(db, sql, 0, 0, &err) ) die("VACUUM INTO failed:", err);
  sqlite3_free(sql);
  sqlite3_close(db);
  if( sqlite3_open(tmp, &db) ) die("cannot open snapshot", tmp);
  sqlite3_exec(db, "PRAGMA journal_mode=DELETE", 0, 0, 0);
  sqlite3_stmt *st; int pageSize = 4096;
  sqlite3_prepare_v2(db, "PRAGMA page_size", -1, &st, 0);
  if( sqlite3_step(st)==SQLITE_ROW ) pageSize = sqlite3_column_int(st, 0);
  sqlite3_finalize(st); sqlite3_close(db);

  return compress_file(tmp, argv[2], pageSize, framePages, level, 1);
}
