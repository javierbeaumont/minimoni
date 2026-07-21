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

# Pinned toolchain, staged so each consumer bakes only the deps it needs (no apk
# at `docker run` time; the multi-arch release build stays lean under QEMU):
#   base - build/release deps (build-and-release.yml builds --target base)
#   test - base + node & sqlite for `make test` (the default CI_IMAGE)
#   tidy - base + clang for tools/clang-tidy.sh
FROM alpine:3.24@sha256:28bd5fe8b56d1bd048e5babf5b10710ebe0bae67db86916198a6eec434943f8b AS base
RUN apk add --no-cache gcc musl-dev make xxd git minify

FROM base AS test
RUN apk add --no-cache nodejs sqlite

FROM base AS tidy
RUN apk add --no-cache clang clang-extra-tools
