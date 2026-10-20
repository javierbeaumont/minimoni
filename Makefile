# minimoni: zero-dependency system monitoring
#
# Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
#
# This file is part of minimoni.
#
# minimoni is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# minimoni is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with minimoni. If not, see <https://www.gnu.org/licenses/>.

CC = gcc
VERSION := $(shell git describe --tags --always 2>/dev/null || echo unknown)
# PIE and SSP explicit: not every gcc defaults to them (Debian's adds no SSP).
CFLAGS = -Wall -Wextra -std=c23 -fPIE -fstack-protector-strong -DMINIMONI_VERSION=\"$(VERSION)\"
# musl links static-pie; glibc links its shared libraries, which its static NSS needs anyway.
GLIBC := $(shell $(CC) -dM -E -include stdio.h - </dev/null 2>/dev/null | grep -c ' __GLIBC__ ')
LINK = $(if $(filter 0,$(GLIBC)),-static-pie,-pie)
INTERP = $(if $(filter 0,$(GLIBC)),no,yes)
LDFLAGS = $(LINK) -lpthread -lm
LDFLAGS_DEBUG = -lpthread -lm

CLANG_FORMAT ?= clang-format

# Pinned toolchain image (test stage of tools/Dockerfile).
CI_IMAGE = minimoni-toolchain

# SQLite: minimal tuning (dead code removed by LTO, not OMIT flags)
SQLITE_FLAGS = -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS

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

.PHONY: all embed release release-linux ci-image debug tidy test-unit test-integration test-e2e \
        check-glibc check-musl check-pie check-systemd test fmt clean-build clean

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

# BearSSL keeps its own CFLAGS (its -fPIC suits a PIE), so SSP goes in through CC.
$(BEARSSL_LIB):
	$(MAKE) -C vendor/bearssl lib CC="$(CC) -fstack-protector-strong"

minimoni: $(SRC) $(VENDOR_OBJ_DEV) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) $(OPT_DEV) $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -Ibuild -o $@ $(SRC) $(VENDOR_OBJ_DEV) $(BEARSSL_LIB) $(LDFLAGS)

minimoni-migrate: $(MIGRATE_SRC)
	$(CC) $(CFLAGS) $(OPT_DEV) -Isrc/migrate -o $@ $(MIGRATE_SRC) $(LINK)

release: MINIFY = 1
release: embed $(VENDOR_OBJ_RELEASE) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) $(OPT_RELEASE) $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) \
	  -Ivendor -Isrc -Ibuild -o minimoni $(SRC) $(VENDOR_OBJ_RELEASE) \
	  $(BEARSSL_LIB) $(LDFLAGS) -Wl,--gc-sections
	strip minimoni
	$(CC) $(CFLAGS) $(OPT_RELEASE) -Isrc/migrate -o minimoni-migrate $(MIGRATE_SRC) \
	  $(LINK) -Wl,--gc-sections
	strip minimoni-migrate

ci-image:
	sh tools/toolchain-image.sh test $(CI_IMAGE)

release-linux: ci-image
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "make release"

debug: embed $(VENDOR_OBJ_DEBUG) $(BEARSSL_LIB)
	$(CC) $(CFLAGS) $(OPT_DEBUG) \
	  $(SQLITE_FLAGS) $(CIVETWEB_FLAGS) $(BEARSSL_INC) -Ivendor -Isrc -Ibuild \
	  -o build/minimoni-debug $(SRC) $(VENDOR_OBJ_DEBUG) $(BEARSSL_LIB) $(LDFLAGS_DEBUG)

tidy:
	pre-commit run clang-tidy --all-files --hook-stage pre-push

# Unit tests (Docker): the db.c <-> migrate mirror check, then one C suite per module (shared
# tests/runner.h; unit-config/json link tomlc17; unit-http uses tests/embed.h, hence no -Ibuild),
# plus the JS suites via node --test with a coverage gate (thresholds apply to the loaded non-DOM
# JS: dashboard/format.js + tools/devserver; the DOM files are covered by the browser paths and the
# e2e-cli.sh bundle check instead).
test-unit: ci-image \
      tests/unit-config.c tests/unit-db.c tests/unit-db_cmd.c tests/unit-downsample.c \
      tests/unit-http.c tests/unit-json.c tests/unit-metrics.c tests/unit-migrate.c \
      tests/unit-units.c tests/unit-alerts.c \
      tests/contract-units.c tests/embed.h tests/runner.h tests/devserver.test.js \
      tests/devserver-http.test.js tests/dashboard.test.js tests/mirror-consolidate.sh
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "mkdir -p build && \
	    sh tests/mirror-consolidate.sh && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Ivendor -Itests \
	      tests/unit-config.c vendor/tomlc17.c -o build/unit-config-test && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Ivendor -Itests $(SQLITE_FLAGS) \
	      tests/unit-db.c vendor/sqlite3.c -o build/unit-db-test -lpthread && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Ivendor -Itests $(SQLITE_FLAGS) \
	      tests/unit-db_cmd.c vendor/sqlite3.c -o build/unit-db_cmd-test -lpthread && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Itests \
	      tests/unit-downsample.c -o build/unit-downsample-test && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Ivendor -Itests $(SQLITE_FLAGS) \
	      $(CIVETWEB_FLAGS) \
	      tests/unit-http.c src/db.c src/json.c src/units.c src/downsample.c \
	      src/config.c src/metrics.c vendor/sqlite3.c vendor/tomlc17.c \
	      -o build/unit-http-test -lpthread && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Ivendor -Itests \
	      tests/unit-json.c vendor/tomlc17.c -o build/unit-json-test && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Itests \
	      tests/unit-metrics.c -o build/unit-metrics-test && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Itests \
	      tests/unit-migrate.c -o build/unit-migrate-test && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Itests \
	      tests/unit-units.c -o build/unit-units-test && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Ivendor -Itests -Ivendor/bearssl/inc \
	      tests/unit-alerts.c -o build/unit-alerts-test && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Itests \
	      tests/contract-units.c -o build/contract-units && \
	    ./build/unit-config-test && ./build/unit-db-test && ./build/unit-db_cmd-test && \
	    ./build/unit-downsample-test && ./build/unit-http-test && ./build/unit-json-test && \
	    ./build/unit-metrics-test && \
	    ./build/unit-migrate-test && ./build/unit-units-test && ./build/unit-alerts-test && \
	    node --test --experimental-test-coverage --test-coverage-lines=97 \
      --test-coverage-branches=90 --test-coverage-functions=92 \
      --test-coverage-exclude='dashboard/app.js' \
      --test-coverage-exclude='dashboard/cards.js' \
      --test-coverage-exclude='dashboard/chart.js' \
      --test-coverage-exclude='dashboard/hover.js' \
      --test-coverage-exclude='tests/*' \
      tests/*.test.js"

# Integration (Docker): our X.509 policy against the real br_x509_minimal engine, in process and
# with no sockets. Needs only the BearSSL library, not a built binary. The certificates are
# generated, never stored: a committed valid one would expire and flip the verdict it expects.
test-integration: ci-image
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "mkdir -p build && make $(BEARSSL_LIB) && \
	    openssl req -quiet -x509 -newkey rsa:2048 -nodes -days 1 -outform DER \
	      -keyout /dev/null -out build/x509-selfsigned.der -subj '/CN=minimoni.test' && \
	    openssl req -quiet -x509 -newkey rsa:2048 -nodes -outform DER \
	      -not_before 20200101000000Z -not_after 20200201000000Z \
	      -keyout /dev/null -out build/x509-expired.der -subj '/CN=minimoni.test' && \
	    gcc -Wall -Wextra -std=c23 -Isrc -Ivendor -Itests $(BEARSSL_INC) \
	      tests/integration-alerts.c $(BEARSSL_LIB) -o build/integration-alerts-test && \
	    ./build/integration-alerts-test"

# End to end (Docker): build release once, then the black-box suites that drive it.
test-e2e: ci-image
	docker run --rm -v "$(PWD)":/work -w /work $(CI_IMAGE) \
	  sh -c "make release && make check-pie && \
	    sh tests/e2e-cli.sh && sh tests/e2e-webhook.sh && sh tests/e2e-migrate.sh"

check-pie:
	for b in minimoni minimoni-migrate; do \
	  readelf -h $$b | grep -q 'Type: *DYN' || { echo "$$b: not PIE" >&2; exit 1; }; \
	  if readelf -l $$b | grep -q INTERP; then i=yes; else i=no; fi; \
	  [ $$i = $(INTERP) ] || { echo "$$b: interpreter $$i, want $(INTERP)" >&2; exit 1; }; \
	done

# Source-build checks (Docker). `make clean`, not clean-build: a host-built BearSSL would leak in.
# Dependabot does not watch these digests: bump them by hand.
DEBIAN_DIGEST = 913f6706df59a68922d1dd08f78c2476560a8d367897200a6005b00e5f67c2d5
DEBIAN_IMAGE = public.ecr.aws/docker/library/debian:trixie@sha256:$(DEBIAN_DIGEST)
FEDORA_DIGEST = 43b29f65a41eb9c35e1cd5323e3bdf3b655c2357a9f4f1ff2f9c2798e5045d80
FEDORA_IMAGE = public.ecr.aws/docker/library/fedora:44@sha256:$(FEDORA_DIGEST)
UBUNTU_DIGEST = f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7
UBUNTU_IMAGE = public.ecr.aws/docker/library/ubuntu:26.04@sha256:$(UBUNTU_DIGEST)
APT_INSTALL = apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends \
  gcc make xxd git libc6-dev openssl socat sqlite3 wget
DNF_INSTALL = dnf install -y -q gcc make git vim-common openssl socat sqlite wget
# $(1) image, $(2) install command, $(3) extra make arguments.
source-build = docker run --rm -e DEBIAN_FRONTEND=noninteractive -v "$(PWD)":/src:ro $(1) sh -c " \
	  $(2) >/dev/null && \
	  cp -r /src /w && cd /w && make clean >/dev/null && \
	  { make release $(3) >build.log 2>&1 || { cat build.log; exit 1; }; } && \
	  ! grep 'warning:' build.log && \
	  make check-pie $(3) && \
	  sh tests/e2e-cli.sh && sh tests/e2e-webhook.sh && sh tests/e2e-migrate.sh"

check-glibc:
	$(call source-build,$(DEBIAN_IMAGE),$(APT_INSTALL))
	$(call source-build,$(FEDORA_IMAGE),$(DNF_INSTALL))
	$(call source-build,$(UBUNTU_IMAGE),$(APT_INSTALL))

check-musl:
	$(call source-build,$(DEBIAN_IMAGE),$(APT_INSTALL) musl-tools,CC=musl-gcc)
	$(call source-build,$(FEDORA_IMAGE),$(DNF_INSTALL) musl-gcc musl-libc-static,CC=musl-gcc)
	$(call source-build,$(UBUNTU_IMAGE),$(APT_INSTALL) musl-tools,CC=musl-gcc)

check-systemd:
	sh tools/check-systemd.sh $(DEBIAN_IMAGE)

test: test-unit test-integration test-e2e

fmt:
	find src tests -name '*.[ch]' | xargs $(CLANG_FORMAT) -i

# Everything this repo generates. Keeps the vendored BearSSL lib, which only changes
# on a vendor bump: rebuilding it on every gate run costs more than it catches.
clean-build:
	rm -f minimoni minimoni-migrate
	rm -rf build

clean: clean-build
	-$(MAKE) -C vendor/bearssl clean 2>/dev/null
