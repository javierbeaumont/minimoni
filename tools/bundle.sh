#!/bin/sh
# Inline style.css and app.js into index.html.
# Reads dashboard/index.html and writes the bundled result to stdout.
# Used by the Makefile embed target to produce src/embed.h.
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
