#!/bin/sh
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

# Run clang-tidy over src/ inside Docker. Compile flags mirror the
# Makefile so the translation units match the real build. build/embed.h is
# generated first because src/http.c includes it.
set -eu

docker build -q --target tidy -t minimoni-toolchain-tidy tools >/dev/null
docker run --rm -v "$PWD":/work -w /work minimoni-toolchain-tidy sh -c '
  mkdir -p build
  sh tools/bundle.sh | xxd -i -n dashboard_index_html - > build/embed.h
  clang-tidy src/*.c -- \
    -std=c23 -DMINIMONI_VERSION=\"dev\" \
    -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DNO_SSL -DNO_CGI -DNO_CACHING -DUSE_WEBSOCKET=0 -DUSE_IPV6=0 \
    -DNO_FILES -DNDEBUG \
    -Ivendor/bearssl/inc -Ivendor -Isrc -Ibuild
'
