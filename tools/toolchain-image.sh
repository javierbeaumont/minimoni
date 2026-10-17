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

# Usage: toolchain-image.sh STAGE TAG
# An image already labelled with the Dockerfile's hash is not rebuilt, so CI, having loaded it from
# its cache, never reaches the registry.
set -eu

want=$(git hash-object tools/Dockerfile)
have=$(docker image inspect -f '{{index .Config.Labels "minimoni.toolchain"}}' "$2" 2>/dev/null ||
    true)

if [ "$have" = "$want" ]; then
    echo "$2: up to date"
    exit 0
fi

docker build -q --target "$1" --label "minimoni.toolchain=$want" -t "$2" tools >/dev/null
