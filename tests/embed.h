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

/* Stand-in for the generated build/embed.h, pulled in because unit-http.c includes
 * src/http.c: the two symbols xxd -i emits, so no unit test needs the dashboard
 * bundle. cli.sh checks the real one is embedded and served. */

unsigned char dashboard_index_html[] = "<!doctype html><canvas></canvas>";
unsigned int  dashboard_index_html_len = sizeof(dashboard_index_html) - 1;
