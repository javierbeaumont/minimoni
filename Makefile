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

# Pinned toolchain image (tools/Dockerfile) shared.
CI_IMAGE = minimoni-toolchain

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
SRC = src/main.c src/alerts.c src/config.c src/db.c src/db_cmd.c \
      src/downsample.c src/http.c src/json.c src/metrics.c src/units.c
VENDOR = vendor/civetweb.c vendor/sqlite3.c vendor/tomlc17.c

# minimoni-migrate: standalone binary that calls `minimoni db exec` for
# every SQL statement, so it links no vendored libs.
MIGRATE_SRC = src/migrate/main.c src/migrate/consolidate.c src/migrate/exec.c \
  src/migrate/migrations.c src/migrate/preflight.c src/migrate/snapshot.c

# Vendored amalgamations carry upstream warnings we don't own (e.g. civetweb's
# unused-but-set variables). Compile them as separate objects with that one
# check disabled so src/ stays strict under -Wall -Wextra. $(OPT) carries each
# target's optimisation flags; run "make clean" when switching release/debug.
VENDOR_OBJ = $(patsubst vendor/%.c,build/%.o,$(VENDOR))

.PHONY: all embed release release-linux ci-image debug tidy \
        test-unit test-integration test fmt clean

all: embed minimoni minimoni-migrate

# embed.h: dashboard bundled (CSS + JS + favicon inlined) and serialised as a C byte array.
# tools/bundle.sh inlines dashboard/style.css, app.js, and favicon.svg into index.html,
# then xxd converts the result to a C byte array included by the HTTP handler.
# Not tracked in git; run "make embed" before the first build or after editing the dashboard.
# MINIFY: opt-in HTML minification (no-op when `minify` is not on PATH). The
# release/release-linux targets set MINIFY=1; plain `make` leaves it unset so
# dev builds keep readable source for browser DevTools. See ADR-0007.
MINIFY ?=
embed: | build
	MINIFY=$(MINIFY) sh tools/bundle.sh | xxd -i -n dashboard_index_html - > build/embed.h

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

minimoni-migrate: $(MIGRATE_SRC)
	$(CC) $(CFLAGS) -O2 -Isrc/migrate -o $@ $(MIGRATE_SRC) -static

release: OPT = -Os -flto=auto
release: MINIFY = 1
release: embed $(VENDOR_OBJ) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) -Os -flto=auto $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -Ibuild -o minimoni $(SRC) $(VENDOR_OBJ) \
	  $(BEARSSL_LIB) $(LDFLAGS) -Wl,--gc-sections
	strip minimoni
	$(CC) $(CFLAGS) -Os -flto=auto -Isrc/migrate -o minimoni-migrate $(MIGRATE_SRC) \
	  -static -Wl,--gc-sections
	strip minimoni-migrate

ci-image:
	docker build -q -t $(CI_IMAGE) tools >/dev/null

release-linux: ci-image
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "apk add --quiet gcc musl-dev make xxd git minify && make release"

debug: OPT = -O0 -g -fsanitize=address,undefined
debug: embed $(VENDOR_OBJ) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined \
	  $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) -Ivendor -Isrc -Ibuild \
	  -o build/minimoni-debug $(SRC) $(VENDOR_OBJ) $(BEARSSL_LIB) $(LDFLAGS_DEBUG)

tidy:
	pre-commit run clang-tidy --all-files --hook-stage pre-push

# Unit tests: pure logic across C, Python and JS, all run inside Docker.
# C suites (one per module) use the shared harness in tests/runner.h and
# include the module under test directly so static helpers are exercisable;
# unit-config and unit-json link tomlc17. The devserver (Python) and dashboard
# (JS) pure helpers each get one suite; app.js guards its browser entry point so
# node can require it for the pure helpers without a DOM.
test-unit: ci-image \
      tests/unit-config.c tests/unit-db.c tests/unit-db_cmd.c tests/unit-downsample.c \
      tests/unit-json.c tests/unit-metrics.c tests/unit-migrate.c tests/unit-units.c \
      tests/runner.h tests/test_devserver.py tests/dashboard.test.js
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "apk add --quiet gcc musl-dev nodejs python3 && mkdir -p build && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Ivendor -Itests \
	      tests/unit-config.c vendor/tomlc17.c -o build/unit-config-test && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Ivendor -Itests $(SQLITE_FLAGS) \
	      tests/unit-db.c vendor/sqlite3.c -o build/unit-db-test -lpthread && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Ivendor -Itests $(SQLITE_FLAGS) \
	      tests/unit-db_cmd.c vendor/sqlite3.c -o build/unit-db_cmd-test -lpthread && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Itests \
	      tests/unit-downsample.c -o build/unit-downsample-test && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Ivendor -Itests \
	      tests/unit-json.c vendor/tomlc17.c -o build/unit-json-test && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Itests \
	      tests/unit-metrics.c -o build/unit-metrics-test && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Itests \
	      tests/unit-migrate.c -o build/unit-migrate-test && \
	    gcc -Wall -Wextra -std=c11 -Isrc -Itests \
	      tests/unit-units.c -o build/unit-units-test && \
	    ./build/unit-config-test && ./build/unit-db-test && ./build/unit-db_cmd-test && \
	    ./build/unit-downsample-test && ./build/unit-json-test && ./build/unit-metrics-test && \
	    ./build/unit-migrate-test && ./build/unit-units-test && \
	    python3 tests/test_devserver.py && node tests/dashboard.test.js"

# Integration: build the release binaries once, then run both black-box suites:
# the minimoni CLI (cli.sh) and minimoni-migrate (migrate.sh, which builds its DB
# fixtures with the sqlite3 CLI). One build serves both.
test-integration: ci-image
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "apk add --quiet gcc musl-dev make xxd git sqlite minify && make release && \
	    sh tests/cli.sh && sh tests/migrate.sh"

# Run every test suite the project has.
test: test-unit test-integration

fmt:
	find src tests -name '*.[ch]' | xargs $(CLANG_FORMAT) -i

clean:
	rm -f minimoni minimoni-migrate
	rm -rf build
	-$(MAKE) -C vendor/bearssl clean 2>/dev/null
