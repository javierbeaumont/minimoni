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

# Inline style.css and app.js into index.html.
# Reads dashboard/index.html and writes the bundled result to stdout.
# Used by the Makefile embed target to produce build/embed.h.
#
# Markers in index.html:
#   <link ... href="favicon.svg">             -> replaced with inline data URI
#   <link rel="stylesheet" href="style.css">  -> replaced with <style>...</style>
#   <script src="app.js"></script>            -> replaced with <script>...</script>

awk '
  /href="favicon.svg"/ {
    svg = ""
    while ((getline line < "dashboard/favicon.svg") > 0) {
      gsub(/^[[:space:]]+/, "", line)   # strip leading whitespace
      gsub(/<!--.*-->/, "", line)       # strip comments
      if (line != "") svg = (svg == "" ? "" : svg " ") line
    }
    gsub(/"/, "\047", svg)              # " -> single-quote (safe inside href="...")
    gsub(/#/, "%23", svg)               # encode # for data URI
    sub(/href="favicon\.svg"/, "href=\"data:image/svg+xml," svg "\"")
    print
    next
  }
  /href="style.css"/ {
    print "  <style>"
    while ((getline line < "dashboard/style.css") > 0) print "  " line
    print "  </style>"
    next
  }
  /src="app.js"/ {
    print "  <script>"
    while ((getline line < "dashboard/app.js") > 0) print "  " line
    print "  </script>"
    next
  }
  { print }
' dashboard/index.html
