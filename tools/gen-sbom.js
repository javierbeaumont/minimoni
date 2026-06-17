#!/usr/bin/env node
/*
 * minimoni - zero-dependency system monitoring
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/* Generate the CycloneDX SBOM for the vendored dependencies and print it to
 * stdout. The deps are amalgamated/copied source compiled into the binary, so
 * there is no package-manager manifest for an SBOM tool to read; this script is
 * the source of truth instead.
 *
 * SQLite and civetweb expose a version macro in their headers and are read
 * automatically. tomlc17 and BearSSL do not, so their versions are pinned below
 * and must be updated when the vendored copy is bumped.
 *
 *   node tools/gen-sbom.js                  print the SBOM
 *   node tools/gen-sbom.js > sbom.cdx.json  refresh the committed file
 *   node tools/gen-sbom.js --check          exit non-zero if the committed file is stale */

const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "..");
const read = (rel) => fs.readFileSync(path.join(root, rel), "utf8");

/* Read a version macro from a vendored header; fail loudly if it moved. */
function headerVersion(file, re, what) {
    const m = read(file).match(re);
    if (!m) {
        console.error(`gen-sbom: could not read ${what} from ${file}`);
        process.exit(1);
    }
    return m[1];
}

const sqliteVersion = headerVersion(
    "vendor/sqlite3.h", /^#define SQLITE_VERSION\s+"([^"]+)"/m, "SQLite version");
const civetwebVersion = headerVersion(
    "vendor/civetweb.h", /^#define CIVETWEB_VERSION\s+"([^"]+)"/m, "civetweb version");

/* Pinned by hand: no version macro in the source. Update on a vendored bump. */
const tomlc17Version = "R260517"; /* tomlc17.h: "A crude way ... Manually changed." */
const bearsslVersion = "0.6";     /* last upstream release (2018); no version macro */

const cpe = (vendor, product, version) =>
    `cpe:2.3:a:${vendor}:${product}:${version}:*:*:*:*:*:*:*`;

const sbom = {
    bomFormat: "CycloneDX",
    specVersion: "1.5",
    version: 1,
    metadata: {
        component: {
            type: "application",
            name: "minimoni",
            description: "Zero-dependency system monitoring in a single C binary",
        },
    },
    components: [
        {
            type: "library",
            name: "sqlite",
            version: sqliteVersion,
            purl: `pkg:generic/sqlite@${sqliteVersion}`,
            cpe: cpe("sqlite", "sqlite", sqliteVersion),
            licenses: [{ license: { name: "Public Domain" } }],
            description: "Vendored SQLite amalgamation, compiled in",
        },
        {
            type: "library",
            name: "civetweb",
            version: civetwebVersion,
            purl: `pkg:generic/civetweb@${civetwebVersion}`,
            cpe: cpe("civetweb_project", "civetweb", civetwebVersion),
            licenses: [{ license: { id: "MIT" } }],
            description: "Vendored civetweb HTTP server, compiled in",
        },
        {
            type: "library",
            name: "tomlc17",
            version: tomlc17Version,
            purl: `pkg:generic/tomlc17@${tomlc17Version}`,
            licenses: [{ license: { id: "MIT" } }],
            description: "Vendored tomlc17 TOML parser, compiled in",
        },
        {
            type: "library",
            name: "bearssl",
            version: bearsslVersion,
            purl: `pkg:generic/bearssl@${bearsslVersion}`,
            cpe: cpe("bearssl", "bearssl", bearsslVersion),
            licenses: [{ license: { id: "MIT" } }],
            description: "Vendored BearSSL TLS client, compiled in (outbound HTTPS webhooks)",
        },
    ],
};

const output = JSON.stringify(sbom, null, 2) + "\n";
const arg = process.argv[2];

if (arg === "--check") {
    if (read("sbom.cdx.json") !== output) {
        console.error(
            "gen-sbom: sbom.cdx.json is stale -- regenerate: node tools/gen-sbom.js > sbom.cdx.json");
        process.exit(1);
    }
} else if (arg) {
    console.error(`gen-sbom: unknown argument '${arg}' (use --check, or no argument)`);
    process.exit(2);
} else {
    process.stdout.write(output);
}
