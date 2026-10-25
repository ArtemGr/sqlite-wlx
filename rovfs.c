/*
** rovfs: read-only, lock-free SQLite VFS shim for shares that don't support file locking
** or shared-memory mapping, e.g. \\wsl.localhost\<distro>\... (the WSL 9P redirector).
**
**   sqlite3_rovfs_register();
**   sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, "rovfs");
**
** - The database, journal and WAL files are opened read-only through the default VFS;
**   nothing is ever created, written or deleted next to them.
** - File locks are no-ops (an EXCLUSIVE lock is refused, so a closing connection never
**   tries to checkpoint or delete the WAL).
** - A WAL-mode database whose -wal file doesn't exist (cleanly closed) is presented as
**   rollback mode, since SQLite would otherwise have to create the -wal file to read it.
** - The WAL index (-shm) lives in heap memory, so a live WAL database is read correctly:
**   SQLite rebuilds the index from the -wal file on open instead of mapping the -shm file.
** - Temp files (sorters, transient indexes) are passed through to the default VFS unchanged.
**
** The view is a snapshot as of the moment the read transaction starts; locks held by other
** processes (e.g. a Linux writer) are not observed.
*/
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>

#define ROVFS_NAME "rovfs"

typedef struct RoFile {
  sqlite3_file base;
  sqlite3_file *pReal;   /* the default VFS's file; storage follows this struct */
  int isShared;          /* main db / journal / WAL on the share (vs. a local temp file) */
  char *zWal;            /* main db only: path of its -wal file */
  int nRegion;           /* heap WAL index regions */
  void **aRegion;
} RoFile;

static sqlite3_vfs *pBase;

#define REAL(f) (((RoFile*)(f))->pReal)

static int roClose(sqlite3_file *f){
  RoFile *p = (RoFile*)f;
  for(int i=0; i<p->nRegion; i++) free(p->aRegion[i]);
  free(p->aRegion);
  p->aRegion = 0; p->nRegion = 0;
  sqlite3_free(p->zWal); p->zWal = 0;
  return p->pReal->pMethods ? p->pReal->pMethods->xClose(p->pReal) : SQLITE_OK;
}
static int roRead(sqlite3_file *f, void *b, int n, sqlite3_int64 o){
  RoFile *p = (RoFile*)f;
  int rc = p->pReal->pMethods->xRead(p->pReal, b, n, o);
  unsigned char *h = (unsigned char*)b - o;   /* h[i] = file byte i */
  if( rc == SQLITE_OK && p->zWal && o <= 18 && o + n >= 20 && h[18] == 2 && h[19] == 2 ){
    int exists = 0;
    if( pBase->xAccess(pBase, p->zWal, SQLITE_ACCESS_EXISTS, &exists) == SQLITE_OK && !exists ){ h[18] = 1; h[19] = 1; }
  }
  return rc;
}
static int roWrite(sqlite3_file *f, const void *b, int n, sqlite3_int64 o){
  RoFile *p = (RoFile*)f; return p->isShared ? SQLITE_READONLY : p->pReal->pMethods->xWrite(p->pReal, b, n, o);
}
static int roTruncate(sqlite3_file *f, sqlite3_int64 s){
  RoFile *p = (RoFile*)f; return p->isShared ? SQLITE_READONLY : p->pReal->pMethods->xTruncate(p->pReal, s);
}
static int roSync(sqlite3_file *f, int fl){
  RoFile *p = (RoFile*)f; return p->isShared ? SQLITE_OK : p->pReal->pMethods->xSync(p->pReal, fl);
}
static int roFileSize(sqlite3_file *f, sqlite3_int64 *s){ return REAL(f)->pMethods->xFileSize(REAL(f), s); }
static int roLock(sqlite3_file *f, int l){
  RoFile *p = (RoFile*)f;
  if( !p->isShared ) return p->pReal->pMethods->xLock(p->pReal, l);
  return l >= SQLITE_LOCK_EXCLUSIVE ? SQLITE_BUSY : SQLITE_OK;
}
static int roUnlock(sqlite3_file *f, int l){
  RoFile *p = (RoFile*)f; return p->isShared ? SQLITE_OK : p->pReal->pMethods->xUnlock(p->pReal, l);
}
static int roCheckRes(sqlite3_file *f, int *r){
  RoFile *p = (RoFile*)f;
  if( p->isShared ){ *r = 0; return SQLITE_OK; }
  return p->pReal->pMethods->xCheckReservedLock(p->pReal, r);
}
static int roFileControl(sqlite3_file *f, int op, void *a){
  RoFile *p = (RoFile*)f;
  if( p->isShared && op == SQLITE_FCNTL_MMAP_SIZE ) return SQLITE_NOTFOUND;  /* keep it to plain reads */
  return p->pReal->pMethods->xFileControl(p->pReal, op, a);
}
static int roSectorSize(sqlite3_file *f){ return REAL(f)->pMethods->xSectorSize(REAL(f)); }
static int roDevChar(sqlite3_file *f){ return REAL(f)->pMethods->xDeviceCharacteristics(REAL(f)); }

/* WAL index in heap memory, private to this connection */
static int roShmMap(sqlite3_file *f, int iPg, int pgsz, int bExtend, void volatile **pp){
  RoFile *p = (RoFile*)f;
  if( iPg >= p->nRegion ){
    void **a = realloc(p->aRegion, (iPg + 1) * sizeof(void*));
    if( !a ) return SQLITE_NOMEM;
    memset(a + p->nRegion, 0, (iPg + 1 - p->nRegion) * sizeof(void*));
    p->aRegion = a; p->nRegion = iPg + 1;
  }
  if( !p->aRegion[iPg] ){
    p->aRegion[iPg] = calloc(1, pgsz);
    if( !p->aRegion[iPg] ) return SQLITE_NOMEM;
  }
  *pp = p->aRegion[iPg];
  return SQLITE_OK;
}
static int roShmLock(sqlite3_file *f, int ofst, int n, int flags){ return SQLITE_OK; }
static void roShmBarrier(sqlite3_file *f){ }
static int roShmUnmap(sqlite3_file *f, int deleteFlag){
  RoFile *p = (RoFile*)f;
  for(int i=0; i<p->nRegion; i++) free(p->aRegion[i]);
  free(p->aRegion);
  p->aRegion = 0; p->nRegion = 0;
  return SQLITE_OK;
}

static const sqlite3_io_methods roMethods = {
  2, roClose, roRead, roWrite, roTruncate, roSync, roFileSize, roLock, roUnlock, roCheckRes,
  roFileControl, roSectorSize, roDevChar, roShmMap, roShmLock, roShmBarrier, roShmUnmap
};

static int roOpen(sqlite3_vfs *vfs, const char *zName, sqlite3_file *f, int flags, int *pOutFlags){
  RoFile *p = (RoFile*)f;
  memset(p, 0, sizeof(*p));
  p->pReal = (sqlite3_file*)&p[1];
  p->isShared = (flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL | SQLITE_OPEN_WAL)) != 0;
  if( p->isShared ){
    flags &= ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_EXCLUSIVE);
    flags |= SQLITE_OPEN_READONLY;
  }
  int rc = pBase->xOpen(pBase, zName, p->pReal, flags, pOutFlags);
  if( rc != SQLITE_OK ){ p->pReal->pMethods = 0; return rc; }
  p->base.pMethods = &roMethods;
  if( (flags & SQLITE_OPEN_MAIN_DB) && zName ) p->zWal = sqlite3_mprintf("%s-wal", zName);
  return SQLITE_OK;
}

/* Files on the share are never deleted; only local temp files can reach this. */
static int roDelete(sqlite3_vfs *v, const char *z, int s){
  (void)v; (void)z; (void)s; return SQLITE_IOERR_DELETE;
}
static int roAccess(sqlite3_vfs *v, const char *z, int fl, int *r){ return pBase->xAccess(pBase, z, fl, r); }
static int roFullPath(sqlite3_vfs *v, const char *z, int n, char *o){ return pBase->xFullPathname(pBase, z, n, o); }
static void *roDlOpen(sqlite3_vfs *v, const char *z){ return pBase->xDlOpen(pBase, z); }
static void roDlError(sqlite3_vfs *v, int n, char *z){ pBase->xDlError(pBase, n, z); }
static void (*roDlSym(sqlite3_vfs *v, void *h, const char *z))(void){ return pBase->xDlSym(pBase, h, z); }
static void roDlClose(sqlite3_vfs *v, void *h){ pBase->xDlClose(pBase, h); }
static int roRandomness(sqlite3_vfs *v, int n, char *z){ return pBase->xRandomness(pBase, n, z); }
static int roSleep(sqlite3_vfs *v, int us){ return pBase->xSleep(pBase, us); }
static int roCurrentTime(sqlite3_vfs *v, double *t){ return pBase->xCurrentTime(pBase, t); }
static int roGetLastError(sqlite3_vfs *v, int n, char *z){ return pBase->xGetLastError ? pBase->xGetLastError(pBase, n, z) : 0; }
static int roCurrentTimeInt64(sqlite3_vfs *v, sqlite3_int64 *t){ return pBase->xCurrentTimeInt64(pBase, t); }

static sqlite3_vfs roVfs = {
  2, 0, 0, 0, ROVFS_NAME, 0,
  roOpen, roDelete, roAccess, roFullPath, roDlOpen, roDlError, roDlSym, roDlClose,
  roRandomness, roSleep, roCurrentTime, roGetLastError, roCurrentTimeInt64
};

int sqlite3_rovfs_register(void){
  if( sqlite3_vfs_find(ROVFS_NAME) ) return SQLITE_OK;
  pBase = sqlite3_vfs_find(0);
  if( !pBase ) return SQLITE_ERROR;
  roVfs.szOsFile = (int)sizeof(RoFile) + pBase->szOsFile;
  roVfs.mxPathname = pBase->mxPathname;
  return sqlite3_vfs_register(&roVfs, 0);
}
