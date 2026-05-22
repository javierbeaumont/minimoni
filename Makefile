CC     = gcc
STRIP  ?= strip
VERSION := $(shell git describe --tags --always 2>/dev/null || echo unknown)
CFLAGS = -Wall -Wextra -std=c11 -DMINIMONI_VERSION=\"$(VERSION)\"
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

# BearSSL: vendored TLS library for HTTPS webhook support
BEARSSL_LIB = vendor/bearssl/build/libbearssl.a
BEARSSL_INC = -Ivendor/bearssl/inc

# SRC expands as modules are implemented
SRC = src/main.c src/metrics.c src/db.c src/config.c src/http.c src/alerts.c
VENDOR = vendor/sqlite3.c vendor/civetweb.c vendor/tomlc17.c

all: embed minimoni

# embed.h: dashboard bundled (CSS + JS inlined) and serialised as a C byte array.
# tools/bundle.sh inlines dashboard/style.css and dashboard/app.js into index.html,
# then xxd converts the result to a C byte array included by the HTTP handler.
# Not tracked in git — run "make embed" before the first build or after editing the dashboard.
embed:
	sh tools/bundle.sh | xxd -i -n dashboard_index_html - > src/embed.h

$(BEARSSL_LIB):
	$(MAKE) -C vendor/bearssl CC="$(CC)"

minimoni: $(SRC) $(VENDOR) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) -O2 $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -o $@ $(SRC) $(VENDOR) $(BEARSSL_LIB) $(LDFLAGS)

release: embed $(BEARSSL_LIB)
	$(CC) $(CFLAGS) -Os -flto $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -o minimoni $(SRC) $(VENDOR) $(BEARSSL_LIB) $(LDFLAGS) -Wl,--gc-sections
	$(STRIP) minimoni

release-linux:
	docker run --rm -v "$(PWD)":/work -w /work alpine:latest \
	  sh -c "apk add --quiet gcc musl-dev make xxd git && make release"

debug: embed $(BEARSSL_LIB)
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined \
	  $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) -Ivendor -Isrc \
	  -o minimoni-debug $(SRC) $(VENDOR) $(BEARSSL_LIB) $(LDFLAGS_DEBUG)

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
	-$(MAKE) -C vendor/bearssl clean 2>/dev/null
