#!/bin/sh
# Inline style.css and app.js into index.html.
# Reads dashboard/index.html and writes the bundled result to stdout.
# Used by the Makefile embed target to produce build/embed.h.
#
# Markers in index.html:
#   <link ... href="favicon.svg">             -> replaced with inline data URI
#   <link rel="stylesheet" href="style.css">  -> replaced with <style>...</style>
#   <script src="app.js"></script>            -> replaced with <script>...</script>
#
# Set MINIFY=1 to pipe the bundled HTML through `minify --type=html`. No-op
# (with a one-line warning to stderr) when `minify` is not on PATH — keeps
# `make` working for contributors who haven't installed it. See ADR-0007
# for the choice of tool (tdewolff/minify).

bundle() {
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
}

if [ "${MINIFY:-0}" = "1" ] && command -v minify >/dev/null 2>&1; then
  bundle | minify --type=html
elif [ "${MINIFY:-0}" = "1" ]; then
  echo "bundle.sh: MINIFY=1 set but 'minify' not on PATH — emitting unminified" >&2
  bundle
else
  bundle
fi
