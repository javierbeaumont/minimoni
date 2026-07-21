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

# Pinned toolchain image (tools/Dockerfile).
CI_IMAGE = minimoni-toolchain

# SQLite: minimal tuning (dead code removed by LTO, not OMIT flags)
SQLITE_FLAGS = -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS

# civetweb: HTTP-only.
CIVETWEB_FLAGS = -DNO_SSL -DNO_CGI -DNO_CACHING \
  -DUSE_WEBSOCKET=0 -DUSE_IPV6=0 -DNO_FILES -DNDEBUG

# BearSSL: vendored TLS for the HTTPS webhook.
BEARSSL_LIB = vendor/bearssl/build/libbearssl.a
BEARSSL_INC = -Ivendor/bearssl/inc

SRC = src/main.c src/alerts.c src/config.c src/db.c src/db_cmd.c \
      src/downsample.c src/http.c src/json.c src/metrics.c src/units.c
VENDOR = vendor/civetweb.c vendor/sqlite3.c vendor/tomlc17.c

# minimoni-migrate: standalone, links no vendored libs (calls `minimoni db exec`).
MIGRATE_SRC = src/migrate/main.c src/migrate/consolidate.c src/migrate/exec.c \
  src/migrate/migrations.c src/migrate/preflight.c src/migrate/snapshot.c

# Vendored amalgamations as separate objects (upstream warnings silenced, src/ stays
# strict); one build/<profile>/ dir per profile so release never reuses another -O's objects.
OPT_DEV     = -O2
OPT_RELEASE = -Os -flto=auto
OPT_DEBUG   = -O0 -g -fsanitize=address,undefined
VENDOR_OBJ_DEV     = $(patsubst vendor/%.c,build/dev/%.o,$(VENDOR))
VENDOR_OBJ_RELEASE = $(patsubst vendor/%.c,build/release/%.o,$(VENDOR))
VENDOR_OBJ_DEBUG   = $(patsubst vendor/%.c,build/debug/%.o,$(VENDOR))

# Vendored-object compile; each profile rule below appends its own $(OPT_*).
VENDOR_CC = $(CC) $(CFLAGS) -Wno-unused-but-set-variable $(SQLITE_FLAGS) \
  $(CIVETWEB_FLAGS) $(BEARSSL_INC) -Ivendor -Isrc -Ibuild -c

.PHONY: all embed release release-linux ci-image debug tidy \
        test-unit test-integration test fmt clean

all: embed minimoni minimoni-migrate

# embed.h: dashboard (CSS/JS/favicon) inlined by tools/bundle.sh, xxd'd to a C array;
# untracked, so run "make embed" before the first build or after editing the dashboard.
# MINIFY=1 (release) minifies via `minify` if present, else readable for DevTools. See ADR-0007.
MINIFY ?=
embed: | build
	MINIFY=$(MINIFY) VERSION=$(VERSION) sh tools/bundle.sh \
	    | xxd -i -n dashboard_index_html - > build/embed.h

build build/dev build/release build/debug:
	mkdir -p $@

build/dev/%.o: vendor/%.c | build/dev
	$(VENDOR_CC) $(OPT_DEV) $< -o $@

build/release/%.o: vendor/%.c | build/release
	$(VENDOR_CC) $(OPT_RELEASE) $< -o $@

build/debug/%.o: vendor/%.c | build/debug
	$(VENDOR_CC) $(OPT_DEBUG) $< -o $@

$(BEARSSL_LIB):
	$(MAKE) -C vendor/bearssl lib CC="$(CC)"

minimoni: $(SRC) $(VENDOR_OBJ_DEV) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) $(OPT_DEV) $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -Ibuild -o $@ $(SRC) $(VENDOR_OBJ_DEV) $(BEARSSL_LIB) $(LDFLAGS)

minimoni-migrate: $(MIGRATE_SRC)
	$(CC) $(CFLAGS) $(OPT_DEV) -Isrc/migrate -o $@ $(MIGRATE_SRC) -static

release: MINIFY = 1
release: embed $(VENDOR_OBJ_RELEASE) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) $(OPT_RELEASE) $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -Ibuild -o minimoni $(SRC) $(VENDOR_OBJ_RELEASE) \
	  $(BEARSSL_LIB) $(LDFLAGS) -Wl,--gc-sections
	strip minimoni
	$(CC) $(CFLAGS) $(OPT_RELEASE) -Isrc/migrate -o minimoni-migrate $(MIGRATE_SRC) \
	  -static -Wl,--gc-sections
	strip minimoni-migrate

ci-image:
	docker build -q -t $(CI_IMAGE) tools >/dev/null

release-linux: ci-image
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "apk add --quiet gcc musl-dev make xxd git minify && make release"

debug: embed $(VENDOR_OBJ_DEBUG) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) $(OPT_DEBUG) \
	  $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) -Ivendor -Isrc -Ibuild \
	  -o build/minimoni-debug $(SRC) $(VENDOR_OBJ_DEBUG) $(BEARSSL_LIB) $(LDFLAGS_DEBUG)

tidy:
	pre-commit run clang-tidy --all-files --hook-stage pre-push

# Unit tests (Docker): one C suite per module (shared tests/runner.h; unit-config/json
# link tomlc17), plus the JS suites via node --test.
test-unit: ci-image \
      tests/unit-config.c tests/unit-db.c tests/unit-db_cmd.c tests/unit-downsample.c \
      tests/unit-json.c tests/unit-metrics.c tests/unit-migrate.c tests/unit-units.c \
      tests/runner.h tests/devserver.test.js tests/dashboard.test.js
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "apk add --quiet gcc musl-dev nodejs && mkdir -p build && \
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
	    node --test tests/dashboard.test.js tests/devserver.test.js"

# Integration (Docker): build release once, then the black-box suites cli.sh + migrate.sh.
test-integration: ci-image
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "apk add --quiet gcc musl-dev make xxd git sqlite minify && make release && \
	    sh tests/cli.sh && sh tests/migrate.sh"

test: test-unit test-integration

fmt:
	find src tests -name '*.[ch]' | xargs $(CLANG_FORMAT) -i

clean:
	rm -f minimoni minimoni-migrate
	rm -rf build
	-$(MAKE) -C vendor/bearssl clean 2>/dev/null
