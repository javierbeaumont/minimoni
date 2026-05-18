CC = gcc
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS = -static -lpthread

# SQLite: minimal tuning (dead code removed by LTO, not OMIT flags)
SQLITE_FLAGS = -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS

# civetweb: HTTP-only, strip unused features
CIVETWEB_FLAGS = -DNO_SSL -DNO_CGI -DNO_CACHING \
  -DUSE_WEBSOCKET=0 -DUSE_IPV6=0 -DNO_FILES -DNDEBUG

# SRC expands as modules are implemented
SRC = src/main.c
VENDOR = vendor/sqlite3.c vendor/civetweb.c vendor/tomlc17.c

all: embed minimoni

embed:
	xxd -i dashboard/index.html > src/embed.h

minimoni: $(SRC) $(VENDOR)
	$(CC) $(CFLAGS) -O2 $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) -Ivendor -Isrc -o $@ $^ $(LDFLAGS)

release: embed
	$(CC) $(CFLAGS) -Os -flto $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) \
	  -Ivendor -Isrc -o minimoni $(SRC) $(VENDOR) $(LDFLAGS) -Wl,--gc-sections
	strip minimoni

release-linux:
	docker run --rm -v "$(PWD)":/work -w /work alpine:latest \
	  sh -c "apk add --quiet gcc musl-dev make xxd && make release"

clean:
	rm -f minimoni src/embed.h
