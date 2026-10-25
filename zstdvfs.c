/*
** zstdvfs: read-only SQLite VFS over a database compressed in the zstd *seekable* format
** (zstd/contrib/seekable_format). Same file format the Go `jtarchie/sqlitezstd` reads.
**
**   SELECT load_extension('./zstdvfs');                      -- registers VFS "zstd"
**   sqlite3_open_v2("file:db.sqlite.zst?vfs=zstd&cache_frames=16", ..., SQLITE_OPEN_READONLY|SQLITE_OPEN_URI, 0)
**
** Only the main database goes through the compressed reader; temp files, journals etc. are
** delegated to the default VFS. Each open file keeps a small LRU cache of decompressed frames.
** If the image was taken from a WAL-mode database, header bytes 18/19 are presented as 1
** (rollback mode) so SQLite never looks for -wal/-shm files.
**
** Build as a loadable extension (default) or define ZSTDVFS_STATIC and call
** sqlite3_zstdvfs_register() when compiling into the application / amalgamation.
*/
#ifdef ZSTDVFS_STATIC
# include "sqlite3.h"
#else
# include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zstd_seekable.h"
#ifdef _WIN32
# include <windows.h>
/* SQLite hands us UTF-8 paths; the CRT's fopen expects the ANSI code page. */
static FILE *zOpenFile(const char *zUtf8){
  int n = MultiByteToWideChar(CP_UTF8, 0, zUtf8, -1, 0, 0);
  if( n<=0 ) return 0;
  wchar_t *w = malloc(n * sizeof(wchar_t));
  if( !w ) return 0;
  MultiByteToWideChar(CP_UTF8, 0, zUtf8, -1, w, n);
  FILE *fp = _wfopen(w, L"rb");
  free(w);
  return fp;
}
#else
# define zOpenFile(z) fopen((z), "rb")
#endif

#define ZVFS_NAME "zstd"
#define ZVFS_DEFAULT_CACHE 8

typedef struct ZFrame { unsigned idx; unsigned long long off; size_t len; unsigned long long used; unsigned char *buf; } ZFrame;

typedef struct ZFile {
  sqlite3_file base;
  sqlite3_file *pReal;          /* non-NULL: delegated (temp/journal) file; real file follows struct */
  FILE *fp;
  ZSTD_seekable *zs;
  sqlite3_int64 size;           /* decompressed size */
  unsigned nFrames;
  int nCache;
  ZFrame *cache;
  unsigned long long tick, hits, misses;
} ZFile;

static sqlite3_vfs *pOrig;      /* default VFS we delegate to */

/* ---------- compressed main-db file ---------- */

static ZFrame *zGetFrame(ZFile *p, unsigned idx){
  ZFrame *victim = &p->cache[0];
  for(int i=0; i<p->nCache; i++){
    if( p->cache[i].buf && p->cache[i].idx==idx ){ p->cache[i].used = ++p->tick; p->hits++; return &p->cache[i]; }
    if( !p->cache[i].buf ){ victim = &p->cache[i]; break; }
    if( p->cache[i].used < victim->used ) victim = &p->cache[i];
  }
  size_t len = (size_t)ZSTD_seekable_getFrameDecompressedSize(p->zs, idx);
  unsigned char *buf = victim->buf && victim->len >= len ? victim->buf : realloc(victim->buf, len);
  if( !buf ) return 0;
  victim->buf = buf;
  size_t r = ZSTD_seekable_decompressFrame(p->zs, buf, len, idx);
  if( ZSTD_isError(r) || r!=len ){ victim->buf = 0; free(buf); return 0; }
  victim->idx = idx; victim->len = len; victim->used = ++p->tick;
  victim->off = ZSTD_seekable_getFrameDecompressedOffset(p->zs, idx);
  p->misses++;
  return victim;
}

static int zRead(sqlite3_file *f, void *dst, int n, sqlite3_int64 off){
  ZFile *p = (ZFile*)f;
  if( p->pReal ) return p->pReal->pMethods->xRead(p->pReal, dst, n, off);
  unsigned char *out = dst;
  sqlite3_int64 pos = off, end = off + n;
  int short_read = 0;
  if( end > p->size ){ short_read = 1; memset(out, 0, n); end = p->size; }
  while( pos < end ){
    unsigned idx = ZSTD_seekable_offsetToFrameIndex(p->zs, (unsigned long long)pos);
    ZFrame *fr = zGetFrame(p, idx);
    if( !fr ) return SQLITE_IOERR_READ;
    size_t in = (size_t)(pos - fr->off);
    size_t take = fr->len - in;
    if( (sqlite3_int64)take > end - pos ) take = (size_t)(end - pos);
    memcpy(out + (pos - off), fr->buf + in, take);
    pos += take;
  }
  /* present a WAL-mode image as rollback mode: no -wal/-shm needed */
  if( off < 20 && off + n > 19 && out[18-off]==2 && out[19-off]==2 ){ out[18-off] = 1; out[19-off] = 1; }
  return short_read ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
}

static int zClose(sqlite3_file *f){
  ZFile *p = (ZFile*)f;
  if( p->pReal ){ int rc = p->pReal->pMethods ? p->pReal->pMethods->xClose(p->pReal) : SQLITE_OK; return rc; }
  if( getenv("ZSTDVFS_STATS") ) fprintf(stderr, "zstdvfs: %u frames, cache %d, hits %llu, misses (frames decompressed) %llu\n",
                                         p->nFrames, p->nCache, p->hits, p->misses);
  for(int i=0; i<p->nCache; i++) free(p->cache[i].buf);
  free(p->cache);
  if( p->zs ) ZSTD_seekable_free(p->zs);
  if( p->fp ) fclose(p->fp);
  return SQLITE_OK;
}
static int zReadOnly(sqlite3_file *f, const void *b, int n, sqlite3_int64 o){
  ZFile *p = (ZFile*)f; return p->pReal ? p->pReal->pMethods->xWrite(p->pReal, b, n, o) : SQLITE_READONLY;
}
static int zTruncate(sqlite3_file *f, sqlite3_int64 s){ ZFile *p=(ZFile*)f; return p->pReal ? p->pReal->pMethods->xTruncate(p->pReal, s) : SQLITE_READONLY; }
static int zSync(sqlite3_file *f, int fl){ ZFile *p=(ZFile*)f; return p->pReal ? p->pReal->pMethods->xSync(p->pReal, fl) : SQLITE_OK; }
static int zFileSize(sqlite3_file *f, sqlite3_int64 *s){ ZFile *p=(ZFile*)f; if( p->pReal ) return p->pReal->pMethods->xFileSize(p->pReal, s); *s = p->size; return SQLITE_OK; }
static int zLock(sqlite3_file *f, int l){ ZFile *p=(ZFile*)f; return p->pReal ? p->pReal->pMethods->xLock(p->pReal, l) : SQLITE_OK; }
static int zUnlock(sqlite3_file *f, int l){ ZFile *p=(ZFile*)f; return p->pReal ? p->pReal->pMethods->xUnlock(p->pReal, l) : SQLITE_OK; }
static int zCheckRes(sqlite3_file *f, int *r){ ZFile *p=(ZFile*)f; if( p->pReal ) return p->pReal->pMethods->xCheckReservedLock(p->pReal, r); *r = 0; return SQLITE_OK; }
static int zFileControl(sqlite3_file *f, int op, void *a){ ZFile *p=(ZFile*)f; return p->pReal ? p->pReal->pMethods->xFileControl(p->pReal, op, a) : SQLITE_NOTFOUND; }
static int zSectorSize(sqlite3_file *f){ ZFile *p=(ZFile*)f; return p->pReal ? p->pReal->pMethods->xSectorSize(p->pReal) : 4096; }
static int zDevChar(sqlite3_file *f){ ZFile *p=(ZFile*)f; return p->pReal ? p->pReal->pMethods->xDeviceCharacteristics(p->pReal) : SQLITE_IOCAP_IMMUTABLE; }

static const sqlite3_io_methods zMethods = {
  1, zClose, zRead, zReadOnly, zTruncate, zSync, zFileSize, zLock, zUnlock, zCheckRes,
  zFileControl, zSectorSize, zDevChar
};

/* ---------- VFS ---------- */

static int zOpen(sqlite3_vfs *vfs, const char *zName, sqlite3_file *f, int flags, int *pOutFlags){
  ZFile *p = (ZFile*)f;
  memset(p, 0, sizeof(*p));
  p->base.pMethods = &zMethods;
  if( !(flags & SQLITE_OPEN_MAIN_DB) ){
    /* temp files, journals, ...: hand to the default VFS (real file lives right after our struct) */
    p->pReal = (sqlite3_file*)&p[1];
    int rc = pOrig->xOpen(pOrig, zName, p->pReal, flags, pOutFlags);
    if( rc!=SQLITE_OK ) p->pReal->pMethods = 0;
    return rc;
  }
  if( flags & (SQLITE_OPEN_CREATE) && !(flags & SQLITE_OPEN_READONLY) ){
    /* allowed: SQLite asks for READWRITE|CREATE by default; we just report read-only */
  }
  p->fp = zOpenFile(zName);
  if( !p->fp ) return SQLITE_CANTOPEN;
  p->zs = ZSTD_seekable_create();
  size_t r = p->zs ? ZSTD_seekable_initFile(p->zs, p->fp) : (size_t)-1;
  if( !p->zs || ZSTD_isError(r) ){ zClose(f); p->base.pMethods = 0; return SQLITE_CANTOPEN; }
  p->nFrames = ZSTD_seekable_getNumFrames(p->zs);
  p->size = p->nFrames ? (sqlite3_int64)(ZSTD_seekable_getFrameDecompressedOffset(p->zs, p->nFrames-1)
                                         + ZSTD_seekable_getFrameDecompressedSize(p->zs, p->nFrames-1)) : 0;
  p->nCache = (int)sqlite3_uri_int64(zName, "cache_frames", ZVFS_DEFAULT_CACHE);
  if( p->nCache < 1 ) p->nCache = 1;
  p->cache = calloc(p->nCache, sizeof(ZFrame));
  if( !p->cache ){ zClose(f); p->base.pMethods = 0; return SQLITE_NOMEM; }
  if( pOutFlags ) *pOutFlags = (flags & ~(SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE)) | SQLITE_OPEN_READONLY;
  return SQLITE_OK;
}

static int zAccess(sqlite3_vfs *vfs, const char *zName, int flags, int *pResOut){
  return pOrig->xAccess(pOrig, zName, flags, pResOut);
}
static int zDelete(sqlite3_vfs *v, const char *z, int s){ return pOrig->xDelete(pOrig, z, s); }
static int zFullPath(sqlite3_vfs *v, const char *z, int n, char *o){ return pOrig->xFullPathname(pOrig, z, n, o); }
static void *zDlOpen(sqlite3_vfs *v, const char *z){ return pOrig->xDlOpen(pOrig, z); }
static void zDlError(sqlite3_vfs *v, int n, char *z){ pOrig->xDlError(pOrig, n, z); }
static void (*zDlSym(sqlite3_vfs *v, void *h, const char *z))(void){ return pOrig->xDlSym(pOrig, h, z); }
static void zDlClose(sqlite3_vfs *v, void *h){ pOrig->xDlClose(pOrig, h); }
static int zRandomness(sqlite3_vfs *v, int n, char *z){ return pOrig->xRandomness(pOrig, n, z); }
static int zSleep(sqlite3_vfs *v, int us){ return pOrig->xSleep(pOrig, us); }
static int zCurrentTime(sqlite3_vfs *v, double *t){ return pOrig->xCurrentTime(pOrig, t); }
static int zGetLastError(sqlite3_vfs *v, int n, char *z){ return pOrig->xGetLastError ? pOrig->xGetLastError(pOrig, n, z) : 0; }
static int zCurrentTimeInt64(sqlite3_vfs *v, sqlite3_int64 *t){ return pOrig->xCurrentTimeInt64(pOrig, t); }

static sqlite3_vfs zVfs = {
  2, 0, 1024, 0, ZVFS_NAME, 0,
  zOpen, zDelete, zAccess, zFullPath, zDlOpen, zDlError, zDlSym, zDlClose,
  zRandomness, zSleep, zCurrentTime, zGetLastError, zCurrentTimeInt64
};

int sqlite3_zstdvfs_register(int makeDefault){
  if( sqlite3_vfs_find(ZVFS_NAME) ) return SQLITE_OK;
  pOrig = sqlite3_vfs_find(0);
  if( !pOrig ) return SQLITE_ERROR;
  zVfs.szOsFile = (int)sizeof(ZFile) + pOrig->szOsFile;
  zVfs.mxPathname = pOrig->mxPathname;
  return sqlite3_vfs_register(&zVfs, makeDefault);
}

#ifndef ZSTDVFS_STATIC
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_zstdvfs_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi){
  SQLITE_EXTENSION_INIT2(pApi);
  int rc = sqlite3_zstdvfs_register(0);
  return rc==SQLITE_OK ? SQLITE_OK_LOAD_PERMANENTLY : rc;
}
#endif
