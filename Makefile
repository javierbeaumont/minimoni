CC = gcc
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS = -static -lpthread
LDFLAGS_DEBUG = -lpthread

CLANG_FORMAT ?= $(shell command -v clang-format \
  || echo /Library/Developer/CommandLineTools/usr/bin/clang-format)

# SQLite: minimal tuning (dead code removed by LTO, not OMIT flags)
SQLITE_FLAGS = -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS

# civetweb: HTTP-only, strip unused features
CIVETWEB_FLAGS = -DNO_SSL -DNO_CGI -DNO_CACHING \
  -DUSE_WEBSOCKET=0 -DUSE_IPV6=0 -DNO_FILES -DNDEBUG

# SRC expands as modules are implemented
SRC = src/main.c src/metrics.c src/db.c src/config.c src/http.c
VENDOR = vendor/sqlite3.c vendor/civetweb.c vendor/tomlc17.c

all: embed minimoni

# embed.h: dashboard/index.html serialised as a C byte array by xxd.
# Included by the HTTP handler so the dashboard ships inside the binary.
# Not tracked in git — run "make embed" before the first build or after editing the dashboard.
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

debug: embed
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined \
	  $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) -Ivendor -Isrc \
	  -o minimoni-debug $(SRC) $(VENDOR) $(LDFLAGS_DEBUG)

lint:
	docker run --rm -v "$(PWD)":/work -w /work alpine:latest \
	  sh -c "apk add --quiet cppcheck && cppcheck --error-exitcode=1 --quiet src/"

fmt:
	find src -name '*.[ch]' | xargs $(CLANG_FORMAT) -i

hooks:
	cp hooks/pre-commit .git/hooks/pre-commit
	chmod +x .git/hooks/pre-commit
	@echo "pre-commit hook installed"

clean:
	rm -f minimoni minimoni-debug src/embed.h
