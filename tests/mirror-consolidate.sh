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

# build_consolidate_sql() is duplicated on purpose in src/db.c (daemon) and
# src/migrate/consolidate.c (migrate tool): they share no translation unit, so
# nothing but this check stops a fix landing in one copy and not the other.
#
# Compares the function bodies only; the leading comments differ on purpose
# (migrate's names db.c as the original).
#
# Run with: sh tests/mirror-consolidate.sh

set -e

# The definition, not db.c's forward declaration: that one ends in a semicolon,
# so requiring ")" as the last character of the line tells them apart.
extract() {
    awk '/^static int build_consolidate_sql\(.*\)$/,/^}/' "$1"
}

a=$(extract src/db.c)
b=$(extract src/migrate/consolidate.c)

if [ -z "$a" ] || [ -z "$b" ]; then
    echo "FAIL: build_consolidate_sql not found in one of the two files" >&2
    exit 1
fi

if [ "$a" != "$b" ]; then
    echo "FAIL: src/db.c and src/migrate/consolidate.c have drifted" >&2
    printf '%s\n' "$a" >/tmp/mirror-db.c.txt
    printf '%s\n' "$b" >/tmp/mirror-migrate.c.txt
    diff -u /tmp/mirror-db.c.txt /tmp/mirror-migrate.c.txt >&2 || true
    exit 1
fi

echo "mirror-consolidate: build_consolidate_sql identical in both copies"
