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

const js = require("@eslint/js");
const globals = require("globals");

module.exports = [
    js.configs.recommended,
    {
        languageOptions: { ecmaVersion: 2021, sourceType: "commonjs" },
        rules: {
            "no-unused-vars": ["error", { caughtErrors: "none" }],
            "no-var": "error",
            "prefer-const": "error",
        },
    },
    {
        files: ["dashboard/**/*.js"],
        languageOptions: { globals: { ...globals.browser } },
    },
    {
        files: ["tests/**/*.js", "tools/**/*.js", "eslint.config.js"],
        languageOptions: { globals: { ...globals.node } },
    },
];
