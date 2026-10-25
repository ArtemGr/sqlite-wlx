# Cross-build with MinGW-w64 (Linux) or natively with MinGW (MSYS2):
#   make            -> runs the tests and benchmark, then builds build/sqlite.wlx64, build/sqlite.wlx
#                      and the 64-bit tools build/sqlzst.exe, build/zstindex.exe; repeats the benchmark table last
#   make zip        -> build/sqlite-wlx.zip: plugin + pluginst.inf (TC auto-install), tools/, licenses
#   make test       -> only the tests: VFS unit tests, tool tests (sqlzst; split + zstd + zstindex),
#                      then a small benchmark; built natively for the host with HOSTCC
# SQLite and zstd sources are downloaded by ./fetch-deps.sh on the first build.
ZSTD    = deps/zstd/lib
SEEK    = deps/zstd/contrib/seekable_format
INC     = -Iinclude -I$(ZSTD) -I$(ZSTD)/common -I$(SEEK)
CFLAGS  = -O2 -DZSTD_DISABLE_ASM -DZSTDVFS_STATIC $(INC)
LDFLAGS = -shared -static -s -Wl,--kill-at
LIBS    = -lcomctl32 -luxtheme -lgdi32 -lcomdlg32 -lshlwapi
DEPS    = include/sqlite3.c $(SEEK)/zstdseek_decompress.c
ZDEC    = $(ZSTD)/common/*.c $(ZSTD)/decompress/*.c $(SEEK)/zstdseek_decompress.c
ZENC    = $(ZSTD)/compress/*.c $(SEEK)/zstdseek_compress.c
SRC     = main.c zstdvfs.c rovfs.c include/sqlite3.c $(ZDEC)

HOSTCC  ?= cc
ifeq ($(OS),Windows_NT)
HOSTLIBS ?= -lm
else
HOSTLIBS ?= -lpthread -lm
endif

all: test build/sqlite.wlx64 build/sqlite.wlx build/sqlzst.exe build/zstindex.exe
	@cat build/test/bench.txt

$(DEPS):
	./fetch-deps.sh

build/sqlite.wlx64: main.c zstdvfs.c rovfs.c $(DEPS)
	mkdir -p build && x86_64-w64-mingw32-gcc $(CFLAGS) $(LDFLAGS) $(SRC) -o $@ $(LIBS)

build/sqlite.wlx: main.c zstdvfs.c rovfs.c $(DEPS)
	mkdir -p build && i686-w64-mingw32-gcc $(CFLAGS) $(LDFLAGS) $(SRC) -o $@ $(LIBS)

build/sqlzst.exe: tools/sqlzst.c $(DEPS)
	mkdir -p build && x86_64-w64-mingw32-gcc -O2 -DZSTD_DISABLE_ASM $(INC) -static -s -o $@ tools/sqlzst.c \
		include/sqlite3.c $(ZDEC) $(ZENC)

build/zstindex.exe: tools/zstindex.c $(DEPS)
	mkdir -p build && x86_64-w64-mingw32-gcc -O2 -DZSTD_DISABLE_ASM $(INC) -static -s -o $@ tools/zstindex.c $(ZDEC) $(ZENC)

# --- tests: native host build ---
HOSTCFLAGS = -O1 -DZSTD_DISABLE_ASM $(INC)

build/test/sqlite3.o: include/sqlite3.c
	mkdir -p build/test && $(HOSTCC) -O1 -DSQLITE_OMIT_LOAD_EXTENSION -c $< -o $@

build/test/vfs_test: test/vfs_test.c zstdvfs.c rovfs.c build/test/sqlite3.o $(DEPS)
	$(HOSTCC) $(HOSTCFLAGS) -DZSTDVFS_STATIC -o $@ test/vfs_test.c zstdvfs.c rovfs.c \
		build/test/sqlite3.o $(ZDEC) $(ZENC) $(HOSTLIBS)

build/test/sqlzst: tools/sqlzst.c build/test/sqlite3.o $(DEPS)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/sqlzst.c build/test/sqlite3.o $(ZDEC) $(ZENC) $(HOSTLIBS)

build/test/zstindex: tools/zstindex.c $(DEPS)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/zstindex.c $(ZDEC) $(ZENC)

# the stock zstd CLI, built from the same release, for the split + zstd + zstindex recipe
build/test/zstd: $(DEPS)
	mkdir -p build/test && $(HOSTCC) -O2 -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DZSTD_MULTITHREAD -I$(ZSTD) -I$(ZSTD)/common \
		-o $@ deps/zstd/programs/*.c $(ZSTD)/common/*.c $(ZSTD)/compress/*.c $(ZSTD)/decompress/*.c $(ZSTD)/dictBuilder/*.c $(HOSTLIBS)

TESTBIN = build/test/vfs_test build/test/sqlzst build/test/zstindex build/test/zstd

test: $(TESTBIN)
	rm -rf build/test/tmp && mkdir -p build/test/tmp
	./test/tools_test.sh build/test build/test/tmp
	./build/test/vfs_test build/test/tmp build/test/bench.txt

build/pluginst.inf:
	mkdir -p build && printf '[plugininstall]\r\ndescription=SQLite viewer (sqlite-wlx), also opens zstd-seekable .zst and WSL paths\r\ntype=wlx\r\nfile=sqlite.wlx\r\ndefaultdir=sqlite-wlx\r\n' > $@

zip: all build/pluginst.inf
	rm -rf build/pkg build/sqlite-wlx.zip && mkdir -p build/pkg/tools
	cp build/sqlite.wlx build/sqlite.wlx64 build/pluginst.inf build/pkg/
	cp build/sqlzst.exe build/zstindex.exe build/pkg/tools/
	cp LICENSE build/pkg/LICENSE.txt && cp deps/zstd/LICENSE build/pkg/LICENSE-zstd.txt
	cd build/pkg && zip -q -r ../sqlite-wlx.zip .

clean:
	rm -rf build

distclean: clean
	rm -rf deps include/sqlite3.c include/sqlite3.h

.PHONY: all test zip clean distclean
