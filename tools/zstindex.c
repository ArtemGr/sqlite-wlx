/*
** zstindex: append a seek table to a file of concatenated zstd frames, making it a valid
** seekable-zstd file without recompressing. Lets the stock zstd CLI do the compression:
**
**   split -b $((16384*4)) -d -a 6 db.sqlite chunk.          # 4 pages per frame
**   for f in chunk.*; do zstd -19 -q -c "$f"; done > db.sqlite.zst
**   zstindex db.sqlite.zst                                   # appends the seek table in place
**
** Every frame must record its decompressed size (zstd does this for regular files).
*/
#ifndef _WIN32
# define _FILE_OFFSET_BITS 64  /* 64-bit off_t for fseeko/ftello */
#endif
#include <stdio.h>
#include <stdlib.h>
#include "zstd.h"
#include "zstd_seekable.h"

#ifdef _WIN32  /* long is 32-bit on Windows: use 64-bit offsets for files over 2 GB */
# define fseek64 _fseeki64
# define ftell64 _ftelli64
#else
# define fseek64 fseeko
# define ftell64 ftello
#endif

int main(int argc, char **argv){
  if( argc != 2 ){ fprintf(stderr, "usage: zstindex FILE.zst\n"); return 1; }
  FILE *f = fopen(argv[1], "rb");
  if( !f ){ perror(argv[1]); return 1; }
  fseek64(f, 0, SEEK_END); long long len = ftell64(f); fseek64(f, 0, SEEK_SET);
  unsigned char *buf = (len > 0 && (unsigned long long)len <= (size_t)-1) ? malloc((size_t)len) : 0;
  if( !buf ){ fprintf(stderr, "%s: cannot read %lld bytes into memory\n", argv[1], len); return 1; }
  if( fread(buf, 1, len, f) != (size_t)len ){ perror("read"); return 1; }
  fclose(f);
  ZSTD_seekable *probe = ZSTD_seekable_create();
  if( !ZSTD_isError(ZSTD_seekable_initBuff(probe, buf, len)) ){ fprintf(stderr, "already seekable\n"); return 1; }
  ZSTD_seekable_free(probe);

  ZSTD_frameLog *fl = ZSTD_seekable_createFrameLog(0 /* no checksums: frames carry their own */);
  unsigned n = 0; unsigned long long total = 0;
  for(long long off = 0; off < len; ){
    size_t c = ZSTD_findFrameCompressedSize(buf + off, len - off);
    if( ZSTD_isError(c) ){ fprintf(stderr, "bad frame at %lld: %s\n", off, ZSTD_getErrorName(c)); return 1; }
    unsigned long long d = ZSTD_getFrameContentSize(buf + off, c);
    if( d == ZSTD_CONTENTSIZE_UNKNOWN || d == ZSTD_CONTENTSIZE_ERROR ){
      fprintf(stderr, "frame %u at %lld has no content size (compress files, not stdin, or use --stream-size)\n", n, off); return 1;
    }
    ZSTD_seekable_logFrame(fl, (unsigned)c, (unsigned)d, 0);
    off += c; total += d; n++;
  }
  f = fopen(argv[1], "ab");
  unsigned char out[4096]; size_t r;
  do{
    ZSTD_outBuffer ob = { out, sizeof out, 0 };
    r = ZSTD_seekable_writeSeekTable(fl, &ob);
    if( ZSTD_isError(r) ){ fprintf(stderr, "%s\n", ZSTD_getErrorName(r)); return 1; }
    fwrite(out, 1, ob.pos, f);
  }while( r > 0 );
  fclose(f);
  printf("%s: indexed %u frames, %llu bytes decompressed\n", argv[1], n, total);
  return 0;
}
