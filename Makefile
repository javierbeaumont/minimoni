# minimoni - zero-dependency system monitoring
# Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

CC = gcc
VERSION := $(shell git describe --tags --always 2>/dev/null || echo unknown)
CFLAGS = -Wall -Wextra -std=c11 -DMINIMONI_VERSION=\"$(VERSION)\"
LDFLAGS = -static -lpthread
LDFLAGS_DEBUG = -lpthread

CLANG_FORMAT ?= clang-format

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
SRC = src/main.c src/metrics.c src/db.c src/config.c src/http.c src/downsample.c src/alerts.c
VENDOR = vendor/sqlite3.c vendor/civetweb.c vendor/tomlc17.c

# Vendored amalgamations carry upstream warnings we don't own (e.g. civetweb's
# unused-but-set variables). Compile them as separate objects with that one
# check disabled so src/ stays strict under -Wall -Wextra. $(OPT) carries each
# target's optimisation flags - run "make clean" when switching release/debug.
VENDOR_OBJ = $(patsubst vendor/%.c,build/%.o,$(VENDOR))

all: embed minimoni

# embed.h: dashboard bundled (CSS + JS + favicon inlined) and serialised as a C byte array.
# tools/bundle.sh inlines dashboard/style.css, app.js, and favicon.svg into index.html,
# then xxd converts the result to a C byte array included by the HTTP handler.
# Not tracked in git - run "make embed" before the first build or after editing the dashboard.
embed: | build
	sh tools/bundle.sh | xxd -i -n dashboard_index_html - > build/embed.h

build:
	mkdir -p build

build/%.o: vendor/%.c | build
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable $(OPT) $(SQLITE_FLAGS) \
	  $(CIVETWEB_FLAGS) $(BEARSSL_INC) -Ivendor -Isrc -Ibuild -c $< -o $@

$(BEARSSL_LIB):
	$(MAKE) -C vendor/bearssl lib CC="$(CC)"

minimoni: OPT = -O2
minimoni: $(SRC) $(VENDOR_OBJ) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) -O2 $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -Ibuild -o $@ $(SRC) $(VENDOR_OBJ) $(BEARSSL_LIB) $(LDFLAGS)

release: OPT = -Os -flto=auto
release: embed $(VENDOR_OBJ) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) -Os -flto=auto $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -Ibuild -o minimoni $(SRC) $(VENDOR_OBJ) $(BEARSSL_LIB) $(LDFLAGS) -Wl,--gc-sections
	strip minimoni

release-linux:
	docker run --rm -v "$(PWD)":/work -w /work alpine:latest \
	  sh -c "apk add --quiet gcc musl-dev make xxd git && make release"

debug: OPT = -O0 -g -fsanitize=address,undefined
debug: embed $(VENDOR_OBJ) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined \
	  $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) -Ivendor -Isrc -Ibuild \
	  -o build/minimoni-debug $(SRC) $(VENDOR_OBJ) $(BEARSSL_LIB) $(LDFLAGS_DEBUG)

lint:
	docker run --rm -v "$(PWD)":/work -w /work alpine:latest \
	  sh -c "apk add --quiet cppcheck && cppcheck --error-exitcode=1 --quiet src/"

# Unit tests: pure logic across C, Python and JS, all run inside Docker (never
# the host). C suites (one per module) use the shared harness in tests/runner.h
# and include the module under test directly so static helpers are exercisable;
# unit-config links tomlc17. The devserver (Python) and dashboard (JS) pure
# helpers each get one suite; app.js guards its browser entry point so node can
# require it for the pure helpers without a DOM.
test: tests/unit-config.c tests/unit-downsample.c tests/runner.h \
      tests/test_devserver.py tests/dashboard.test.js
	docker run --rm -v "$(PWD)":/work -w /work alpine:latest \
	  sh -c "apk add --quiet gcc musl-dev nodejs python3 && mkdir -p build && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Ivendor -Itests \
	      tests/unit-config.c vendor/tomlc17.c -o build/unit-config-test && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Itests \
	      tests/unit-downsample.c -o build/unit-downsample-test && \
	    ./build/unit-config-test && ./build/unit-downsample-test && \
	    python3 tests/test_devserver.py && node tests/dashboard.test.js"

fmt:
	find src -name '*.[ch]' | xargs $(CLANG_FORMAT) -i

hooks:
	cp hooks/pre-commit .git/hooks/pre-commit
	chmod +x .git/hooks/pre-commit
	@echo "pre-commit hook installed"

clean:
	rm -f minimoni
	rm -rf build
	-$(MAKE) -C vendor/bearssl clean 2>/dev/null
