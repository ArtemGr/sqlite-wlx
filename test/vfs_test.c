/*
** Sanity tests and a small benchmark for the two VFSes, built natively for the build host (Linux or MSYS2).
**   vfs_test <scratch-dir> [bench-file]       unit tests, then the benchmark (table goes to bench-file if given)
**   vfs_test mkdb <file> <wal|delete> <rows>   helpers for test/tools_test.sh
**   vfs_test query <file> <vfs|-> <sql>        prints the first column of the first row
** rovfs:   rollback db, live WAL db (data only in -wal), WAL-mode db without -wal,
**          writes refused, -wal left untouched, no -shm created, temp-file spills.
** zstdvfs: seekable images at 1 and 4 pages/frame match the plain db, integrity_check,
**          writes refused, WAL-mode image readable, non-seekable .zst refused.
*/
#include "sqlite3.h"
#include "zstd.h"
#include "zstd_seekable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

int sqlite3_zstdvfs_register(int makeDefault);
int sqlite3_rovfs_register(void);

static int nFail = 0, nPass = 0;
static char dir[512];

#define PAGE_SIZE 16384  /* all test and benchmark databases */

#define CHECK(cond, ...) do { if (cond) nPass++; else { nFail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *path(const char *name) {
	static char buf[4][1024]; static int i;
	i = (i + 1) % 4;
	snprintf(buf[i], sizeof(buf[i]), "%s/%s", dir, name);
	return buf[i];
}

static unsigned char *slurp(const char *p, long *len) {
	FILE *f = fopen(p, "rb");
	if (!f) { *len = -1; return 0; }
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc(*len + 1);
	if (fread(b, 1, *len, f) != (size_t)*len) { free(b); b = 0; *len = -1; }
	fclose(f);
	return b;
}

static int spit(const char *p, const void *b, long len) {
	FILE *f = fopen(p, "wb");
	if (!f) return 0;
	int ok = fwrite(b, 1, len, f) == (size_t)len;
	return fclose(f) == 0 && ok;
}

static int exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != 0; }

static void rmdb(const char *name) {
	char b[1100];
	remove(path(name));
	snprintf(b, sizeof(b), "%s-wal", path(name)); remove(b);
	snprintf(b, sizeof(b), "%s-shm", path(name)); remove(b);
	snprintf(b, sizeof(b), "%s-journal", path(name)); remove(b);
}

static int exec(sqlite3 *db, const char *sql) {
	int rc = sqlite3_exec(db, sql, 0, 0, 0);
	if (rc != SQLITE_OK) printf("  sql error %d (%s): %s\n", rc, sqlite3_errmsg(db), sql);
	return rc;
}

/* first column of the first row as int64, or -1 on error */
static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
	sqlite3_stmt *s; sqlite3_int64 v = -1;
	if (sqlite3_prepare_v2(db, sql, -1, &s, 0) != SQLITE_OK) return -1;
	if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
	sqlite3_finalize(s);
	return v;
}

static int text_is(sqlite3 *db, const char *sql, const char *want) {
	sqlite3_stmt *s; int ok = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &s, 0) != SQLITE_OK) return 0;
	if (sqlite3_step(s) == SQLITE_ROW) ok = strcmp((const char*)sqlite3_column_text(s, 0), want) == 0;
	sqlite3_finalize(s);
	return ok;
}

static sqlite3 *openv(const char *p, const char *vfs) {
	sqlite3 *db = 0;
	int rc = sqlite3_open_v2(p, &db, SQLITE_OPEN_READONLY, vfs);
	if (rc != SQLITE_OK) { sqlite3_close(db); return 0; }
	sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, 0);
	return db;
}

static const char *QUERY_SUM = "select sum(id) + sum(length(v)) from t";

static void make_db(const char *name, const char *journal, int rows) {
	sqlite3 *db; char sql[256];
	rmdb(name);
	sqlite3_open(path(name), &db);
	snprintf(sql, sizeof(sql), "pragma main.page_size=%d; pragma journal_mode=%s", PAGE_SIZE, journal);
	exec(db, sql);
	exec(db, "create table t(id integer primary key, v text); create index t_v on t(v)");
	snprintf(sql, sizeof(sql), "with recursive c(i) as (select 1 union all select i+1 from c where i<%d) "
		"insert into t select i, printf('row %%08d %%s', i, hex(randomblob(20))) from c", rows);
	exec(db, sql);
	sqlite3_close(db);
}

/* compress src into a seekable .zst, framePages pages per frame (0 = one plain, non-seekable frame) */
static int compress_db(const char *src, const char *dst, int framePages) {
	long len; unsigned char *in = slurp(path(src), &len);
	if (!in) return 0;
	size_t cap = ZSTD_compressBound(len) + 1024 + len / 1000;
	unsigned char *out = malloc(cap); size_t outLen = 0;
	if (framePages == 0) {
		outLen = ZSTD_compress(out, cap, in, len, 3);
	} else {
		ZSTD_seekable_CStream *zcs = ZSTD_seekable_createCStream();
		ZSTD_seekable_initCStream(zcs, 3, 1, (unsigned)(framePages * PAGE_SIZE));
		ZSTD_inBuffer ib = { in, (size_t)len, 0 };
		ZSTD_outBuffer ob = { out, cap, 0 };
		while (ib.pos < ib.size) ZSTD_seekable_compressStream(zcs, &ob, &ib);
		while (ZSTD_seekable_endStream(zcs, &ob) > 0) ;
		outLen = ob.pos;
		ZSTD_seekable_freeCStream(zcs);
	}
	int ok = !ZSTD_isError(outLen) && spit(path(dst), out, (long)outLen);
	free(in); free(out);
	return ok;
}

static void test_rovfs(void) {
	printf("rovfs\n");
	char wal[1100], shm[1100];

	/* rollback-journal db */
	make_db("plain.db", "delete", 2000);
	sqlite3 *ref = openv(path("plain.db"), 0);
	sqlite3_int64 want = scalar(ref, QUERY_SUM);
	sqlite3_close(ref);
	sqlite3 *db = openv(path("plain.db"), "rovfs");
	CHECK(db, "open plain.db");
	CHECK(scalar(db, "select count(*) from t") == 2000, "row count, rollback db");
	CHECK(scalar(db, QUERY_SUM) == want, "content, rollback db");
	CHECK(sqlite3_exec(db, "insert into t(v) values('x')", 0, 0, 0) == SQLITE_READONLY, "write refused");
	/* temp-file spill: tiny page cache and a sort that needs a sorter on disk */
	exec(db, "pragma cache_size=2; pragma temp_store=file");
	CHECK(scalar(db, "select count(*) from (select v from t order by substr(v, 14), id)") == 2000, "sort spills to temp file");
	sqlite3_close(db);
	snprintf(shm, sizeof(shm), "%s-shm", path("plain.db"));
	snprintf(wal, sizeof(wal), "%s-journal", path("plain.db"));
	CHECK(!exists(shm) && !exists(wal), "no sidecar files created next to rollback db");

	/* live WAL db: committed rows only in -wal; copy db + wal while the writer is still open */
	rmdb("live.db"); rmdb("snap.db");
	sqlite3 *w; sqlite3_open(path("live.db"), &w);
	exec(w, "pragma journal_mode=wal; pragma wal_autocheckpoint=0; create table t(id integer primary key, v text)");
	exec(w, "with recursive c(i) as (select 1 union all select i+1 from c where i<1500) insert into t select i, 'v' || i from c");
	long n1, n2; unsigned char *b1 = slurp(path("live.db"), &n1);
	snprintf(wal, sizeof(wal), "%s-wal", path("live.db"));
	unsigned char *w1 = slurp(wal, &n2);
	spit(path("snap.db"), b1, n1);
	snprintf(wal, sizeof(wal), "%s-wal", path("snap.db"));
	spit(wal, w1, n2);
	free(b1);
	sqlite3_close(w);

	db = openv(path("snap.db"), "rovfs");
	CHECK(db, "open snap.db");
	CHECK(scalar(db, "select count(*) from t") == 1500, "rows only present in -wal are visible");
	CHECK(text_is(db, "pragma integrity_check", "ok"), "integrity_check, WAL db");
	sqlite3_close(db);
	/* again without NO_CKPT_ON_CLOSE: the VFS alone must stop a checkpoint / WAL delete on close */
	db = 0;
	sqlite3_open_v2(path("snap.db"), &db, SQLITE_OPEN_READONLY, "rovfs");
	CHECK(scalar(db, "select count(*) from t") == 1500, "WAL db, default close behaviour");
	sqlite3_close(db);
	long n3; unsigned char *w2 = slurp(wal, &n3);
	CHECK(n3 == n2 && w2 && memcmp(w1, w2, n2) == 0, "-wal left byte-for-byte unchanged");
	free(w1); free(w2);
	snprintf(shm, sizeof(shm), "%s-shm", path("snap.db"));
	CHECK(!exists(shm), "no -shm created");

	/* WAL-mode db closed cleanly: header says WAL, no -wal file exists */
	make_db("walclean.db", "wal", 500);
	snprintf(wal, sizeof(wal), "%s-wal", path("walclean.db"));
	CHECK(!exists(wal), "precondition: walclean.db has no -wal");
	db = openv(path("walclean.db"), "rovfs");
	CHECK(db && scalar(db, "select count(*) from t") == 500, "WAL-mode db without -wal readable");
	sqlite3_close(db);
	CHECK(!exists(wal), "no -wal created");
}

static void test_zstdvfs(void) {
	printf("zstdvfs\n");
	sqlite3 *ref = openv(path("plain.db"), 0);
	sqlite3_int64 want = scalar(ref, QUERY_SUM);
	sqlite3_int64 wantIdx = scalar(ref, "select count(*) from t where v > 'row 00001000'");
	sqlite3_close(ref);

	int frames[] = { 1, 4 };
	for (int i = 0; i < 2; i++) {
		char name[64]; snprintf(name, sizeof(name), "plain.f%d.db.zst", frames[i]);
		CHECK(compress_db("plain.db", name, frames[i]), "compress %s", name);
		sqlite3 *db = openv(path(name), "zstd");
		CHECK(db, "open %s", name);
		if (!db) continue;
		CHECK(scalar(db, QUERY_SUM) == want, "content matches plain db, %d page(s)/frame", frames[i]);
		CHECK(scalar(db, "select count(*) from t where v > 'row 00001000'") == wantIdx, "index range scan, %d page(s)/frame", frames[i]);
		CHECK(scalar(db, "select length(v) from t where id = 1777") == 53, "point lookup, %d page(s)/frame", frames[i]);
		CHECK(text_is(db, "pragma integrity_check", "ok"), "integrity_check, %d page(s)/frame", frames[i]);
		CHECK(sqlite3_exec(db, "insert into t(v) values('x')", 0, 0, 0) == SQLITE_READONLY, "write refused");
		sqlite3_close(db);
	}

	/* image of a WAL-mode db (header bytes 18/19 = 2) is presented as rollback mode */
	CHECK(compress_db("walclean.db", "walclean.db.zst", 2), "compress walclean.db");
	sqlite3 *db = openv(path("walclean.db.zst"), "zstd");
	CHECK(db && scalar(db, "select count(*) from t") == 500, "WAL-mode image readable");
	CHECK(db && text_is(db, "pragma journal_mode", "delete"), "WAL-mode image reported as rollback mode");
	sqlite3_close(db);
	char wal[1100]; snprintf(wal, sizeof(wal), "%s-wal", path("walclean.db.zst"));
	CHECK(!exists(wal), "no -wal created next to .zst");

	/* ordinary single-frame .zst has no seek table: must be refused, not misread */
	CHECK(compress_db("plain.db", "plain.single.zst", 0), "compress single frame");
	db = 0;
	int rc = sqlite3_open_v2(path("plain.single.zst"), &db, SQLITE_OPEN_READONLY, "zstd");
	if (rc == SQLITE_OK) rc = sqlite3_exec(db, "select 1 from sqlite_master limit 1", 0, 0, 0);
	CHECK(rc != SQLITE_OK, "non-seekable .zst refused (rc=%d)", rc);
	sqlite3_close(db);
}

/* ---------- benchmark ---------- */

static double now_ms(void) { return 1000.0 * clock() / CLOCKS_PER_SEC; }

typedef struct Bench { const char *name; const char *file; const char *vfs; int framePages; } Bench;

static FILE *benchOut;

/* benchmark lines go to the bench file if one is given, otherwise to stdout */
static void bout(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt); vfprintf(benchOut ? benchOut : stdout, fmt, ap); va_end(ap);
}

static void bench(void) {
	const int ROWS = 50000, LOOKUPS = 2000, RANGES = 200;
	make_db("bench.db", "delete", ROWS);
	compress_db("bench.db", "bench.f1.db.zst", 1);
	compress_db("bench.db", "bench.f4.db.zst", 4);
	compress_db("bench.db", "bench.f16.db.zst", 16);
	long plainLen; free(slurp(path("bench.db"), &plainLen));

	Bench runs[] = {
		{ "plain (default VFS)", "bench.db", 0 },
		{ "plain via rovfs", "bench.db", "rovfs" },
		{ 0, "bench.f1.db.zst", "zstd", 1 },
		{ 0, "bench.f4.db.zst", "zstd", 4 },
		{ 0, "bench.f16.db.zst", "zstd", 16 },
	};
	bout("\nbenchmark: %d rows, %d KiB pages, %.1f MiB, zstd level 3, cache_frames=8, CPU ms\n", ROWS, PAGE_SIZE / 1024, plainLen / 1048576.0);
	bout("  %-34s %8s %10s %12s %12s\n", "", "size", "full scan", "point x2000", "range x200");
	for (int i = 0; i < (int)(sizeof(runs) / sizeof(runs[0])); i++) {
		char label[64];
		if (runs[i].framePages) {  /* frame size in uncompressed bytes */
			snprintf(label, sizeof(label), "zstd, %d KiB frames (%d page%s)", runs[i].framePages * PAGE_SIZE / 1024,
				runs[i].framePages, runs[i].framePages > 1 ? "s" : "");
			runs[i].name = label;
		}
		long len; free(slurp(path(runs[i].file), &len));
		sqlite3 *db = openv(path(runs[i].file), runs[i].vfs);
		if (!db) { bout("  %-34s open failed\n", runs[i].name); nFail++; continue; }
		sqlite3_stmt *pt, *rg;
		sqlite3_prepare_v2(db, "select v from t where id = ?", -1, &pt, 0);
		sqlite3_prepare_v2(db, "select count(*) from t where v between ? and ?", -1, &rg, 0);

		double t0 = now_ms();
		sqlite3_int64 sum = scalar(db, QUERY_SUM);
		double t1 = now_ms();
		unsigned seed = 12345;
		for (int k = 0; k < LOOKUPS; k++) {
			seed = seed * 1103515245 + 12345;
			sqlite3_bind_int(pt, 1, 1 + (int)((seed >> 8) % ROWS));
			sqlite3_step(pt); sqlite3_reset(pt);
		}
		double t2 = now_ms();
		for (int k = 0; k < RANGES; k++) {
			char lo[32], hi[32]; int a = 1 + (k * 7919) % (ROWS - 200);
			snprintf(lo, sizeof(lo), "row %08d", a); snprintf(hi, sizeof(hi), "row %08d", a + 100);
			sqlite3_bind_text(rg, 1, lo, -1, SQLITE_TRANSIENT); sqlite3_bind_text(rg, 2, hi, -1, SQLITE_TRANSIENT);
			sqlite3_step(rg); sqlite3_reset(rg);
		}
		double t3 = now_ms();
		sqlite3_finalize(pt); sqlite3_finalize(rg);
		sqlite3_close(db);
		CHECK(sum > 0, "benchmark scan on %s", runs[i].name);
		bout("  %-34s %7.1f%% %10.0f %12.0f %12.0f\n", runs[i].name, 100.0 * len / plainLen, t1 - t0, t2 - t1, t3 - t2);
	}
}

/* ---------- entry ---------- */

int main(int argc, char **argv) {
	if (sqlite3_zstdvfs_register(0) != SQLITE_OK || sqlite3_rovfs_register() != SQLITE_OK) {
		fprintf(stderr, "VFS registration failed\n");
		return 1;
	}
	if (argc == 5 && !strcmp(argv[1], "mkdb")) {
		char *slash = strrchr(argv[2], '/');
		if (slash) { *slash = 0; snprintf(dir, sizeof(dir), "%s", argv[2]); } else snprintf(dir, sizeof(dir), ".");
		make_db(slash ? slash + 1 : argv[2], argv[3], atoi(argv[4]));
		return 0;
	}
	if (argc == 5 && !strcmp(argv[1], "query")) {
		sqlite3 *db = openv(argv[2], strcmp(argv[3], "-") ? argv[3] : 0);
		sqlite3_stmt *s;
		if (!db || sqlite3_prepare_v2(db, argv[4], -1, &s, 0) != SQLITE_OK) { printf("error: %s\n", db ? sqlite3_errmsg(db) : "cannot open"); return 1; }
		int rc = sqlite3_step(s);
		if (rc == SQLITE_ROW) printf("%s\n", (const char*)sqlite3_column_text(s, 0));
		else if (rc != SQLITE_DONE) { printf("error: %s\n", sqlite3_errmsg(db)); return 1; }
		sqlite3_finalize(s); sqlite3_close(db);
		return 0;
	}
	if (argc != 2 && argc != 3) { fprintf(stderr, "usage: vfs_test <scratch-dir> [bench-file] | mkdb <file> <wal|delete> <rows> | query <file> <vfs|-> <sql>\n"); return 2; }
	snprintf(dir, sizeof(dir), "%s", argv[1]);
	test_rovfs();
	test_zstdvfs();
	printf("%d passed, %d failed\n", nPass, nFail);
	if (nFail) return 1;
	if (argc == 3 && !(benchOut = fopen(argv[2], "w"))) { perror(argv[2]); return 1; }
	bench();
	if (benchOut) { fclose(benchOut); printf("benchmark written to %s\n", argv[2]); }
	return nFail ? 1 : 0;
}
